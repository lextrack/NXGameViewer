#include "textures/xtxreader.h"
#include "core/bytes.h"
#include "core/simpletable.h"
#include "core/storedreader.h"
#include "textures/texturedecode.h"

#include <QObject>

#include <algorithm>
#include <optional>

namespace {

constexpr quint32 kBlockTextureInfo = 2;
constexpr quint32 kBlockData = 3;
constexpr qsizetype kBlockHeaderSize = 0x24;
constexpr qsizetype kTextureInfoSize = 0x78;

QString targetName(quint32 target)
{
    switch (target) {
    case 0: return QStringLiteral("1D");
    case 1: return QStringLiteral("2D");
    case 2: return QStringLiteral("3D");
    case 3: return QStringLiteral("1D array");
    case 4: return QStringLiteral("2D array");
    case 8: return QStringLiteral("cube map");
    case 9: return QStringLiteral("cube map array");
    default: return QObject::tr("unknown (%1)").arg(target);
    }
}

void addTextureTables(const XtxTextureInfo &info, const QByteArray &data, qsizetype start, qsizetype size,
                      int textureIndex, int textureCount, Document &doc)
{
    const std::optional<PixelFormat> format = xtxPixelFormat(info.format);
    QString problem;
    if (!format)
        problem = QObject::tr("Pixel format 0x%1 cannot be shown yet").arg(info.format, 0, 16);
    else if (info.width == 0 || info.height == 0 || info.width > quint32(kMaxTextureSide)
             || info.height > quint32(kMaxTextureSide))
        problem = QObject::tr("Unexpected size %1 × %2").arg(info.width).arg(info.height);

    int columns = 0, rows = 0, bpe = 0, blockHeight = 1;
    qsizetype mip0Size = 0;
    if (problem.isEmpty()) {

        bpe = bytesPerElement(*format);
        columns = elementColumns(*format, int(info.width));
        rows = elementRows(*format, int(info.height));
        blockHeight = info.blockHeightLog2 <= 5 ? 1 << info.blockHeightLog2 : defaultBlockHeight(rows);
        mip0Size = blockLinearSize(columns, rows, bpe, blockHeight);
    }

    int sliceCount = 1;
    if (info.sliceSize > 0 && info.dataSize / info.sliceSize > 1 && quint64(mip0Size) <= info.sliceSize) {
        const quint64 n = std::min(info.dataSize, quint64(size)) / info.sliceSize;
        sliceCount = int(std::clamp<quint64>(n, 1, XtxReader::kMaxSlices));
    }

    const std::vector<SimpleTable::Row> properties = {
        {QObject::tr("width"), QString::number(info.width)},
        {QObject::tr("height"), QString::number(info.height)},
        {QObject::tr("pixel format"), (format ? pixelFormatName(*format) : QObject::tr("unknown"))
                                          + QStringLiteral("  (NVN 0x%1)").arg(info.format, 2, 16, QLatin1Char('0'))},
        {QObject::tr("kind"), targetName(info.target)},
        {QObject::tr("mip levels"), QString::number(info.mipCount)},
        {QObject::tr("slices"), QString::number(sliceCount)},
        {QObject::tr("depth"), QString::number(info.depth)},
        {QObject::tr("block height"), QObject::tr("%1 GOBs").arg(1u << std::min(info.blockHeightLog2, 31u))},
        {QObject::tr("data size"), QString::number(info.dataSize)},
    };

    static const char *const faceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    for (int slice = 0; slice < sliceCount; ++slice) {
        QString name = textureCount > 1 ? QObject::tr("Texture %1").arg(textureIndex + 1) : QObject::tr("Texture");
        if (sliceCount > 1)
            name += info.target == 8 && sliceCount == 6 ? QObject::tr(" face %1").arg(QLatin1String(faceNames[slice]))
                                                        : QObject::tr(" slice %1").arg(slice);

        auto table = std::make_shared<SimpleTable>(
            name, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}}, properties);
        const qsizetype sliceStart = qsizetype(slice) * info.sliceSize;
        if (!problem.isEmpty()) {
            table->setDescription(problem);
        } else if (sliceStart + mip0Size > size) {
            table->setDescription(QObject::tr("The pixel data is shorter than the texture needs"));
        } else {
            const PixelFormat f = *format;
            const int width = int(info.width), height = int(info.height);
            table->setImageMaker([data, start = start + sliceStart, mip0Size, f, width, height, columns, rows, bpe,
                                  blockHeight] {
                Image image;
                const std::optional<QByteArray> linear = deswizzleBlockLinear(
                    QByteArrayView(data).sliced(start, mip0Size), columns, rows, bpe, blockHeight);
                if (std::optional<QByteArray> rgba = linear ? decodePixels(f, *linear, width, height) : std::nullopt) {
                    image.width = width;
                    image.height = height;
                    image.rgba = std::move(*rgba);
                }
                return image;
            });
            table->setDescription(QObject::tr("%1 × %2").arg(width).arg(height));
            if (slice == 0)
                table->setTextureSlot({0, textureIndex, {}});
        }
        doc.tables.push_back(table);
    }
}


}

std::optional<PixelFormat> xtxPixelFormat(quint32 nvn)
{
    switch (nvn) {
    case 0x01: return PixelFormat::R8;
    case 0x0D: return PixelFormat::RG8;
    case 0x25: return PixelFormat::RGBA8;
    case 0x38: return PixelFormat::RGBA8;
    case 0x39: return PixelFormat::RGBA4;
    case 0x3B: return PixelFormat::RGB5A1;
    case 0x3C: return PixelFormat::RGB565;
    case 0x3D: return PixelFormat::RGB10A2;
    case 0x42: return PixelFormat::BC1;
    case 0x43: return PixelFormat::BC2;
    case 0x44: return PixelFormat::BC3;
    case 0x49: return PixelFormat::BC4;
    case 0x4A: return PixelFormat::BC4S;
    case 0x4B: return PixelFormat::BC5;
    case 0x4C: return PixelFormat::BC5S;
    case 0x4D: return PixelFormat::BC7;
    default: return std::nullopt;
    }
}

int XtxReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("DFvN") ? 100 : 0;
}

std::unique_ptr<Document> XtxReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return fromBytes(*bytes, error);
}

std::unique_ptr<Document> XtxReader::fromBytes(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    const std::optional<XtxLayout> layout = parseXtxLayout(data, error);
    if (!layout)
        return nullptr;
    const std::vector<XtxTexture> &textures = layout->textures;
    if (textures.empty())
        return fail(QObject::tr("The file has no texture."));

    auto doc = std::make_unique<Document>();
    for (size_t i = 0; i < textures.size(); ++i)
        addTextureTables(textures[i].info, data, textures[i].start, textures[i].size, int(i), int(textures.size()),
                         *doc);
    return doc;
}

std::optional<XtxLayout> parseXtxLayout(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<XtxLayout> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < 0x10 || !data.startsWith("DFvN"))
        return fail(QObject::tr("Not an XTX texture (no \"DFvN\" at the start)."));

    XtxLayout layout;
    std::optional<XtxTextureInfo> pendingInfo;
    qsizetype pos = le32(data, 4);
    for (int guard = 0; pos >= 0x10 && pos + kBlockHeaderSize <= data.size() && guard < 100000; ++guard) {
        if (data.mid(pos, 4) != "HBvN")
            return fail(QObject::tr("Broken block at 0x%1.").arg(pos, 0, 16));
        const quint64 size = le64(data, pos + 8), offset = le64(data, pos + 16);
        const quint32 type = le32(data, pos + 24);
        if (offset < quint64(kBlockHeaderSize) || offset > quint64(data.size() - pos)
            || size > quint64(data.size() - pos) - offset)
            return fail(QObject::tr("Block at 0x%1 goes past the end of the file.").arg(pos, 0, 16));
        const qsizetype start = pos + qsizetype(offset);

        if (type == kBlockTextureInfo) {
            if (size < quint64(kTextureInfoSize))
                return fail(QObject::tr("Texture info at 0x%1 is too short.").arg(start, 0, 16));
            XtxTextureInfo info;
            info.at = start;
            info.dataSize = le64(data, start);
            info.width = le32(data, start + 0x0C);
            info.height = le32(data, start + 0x10);
            info.depth = le32(data, start + 0x14);
            info.target = le32(data, start + 0x18);
            info.format = le32(data, start + 0x1C);
            info.mipCount = le32(data, start + 0x20);
            info.sliceSize = le32(data, start + 0x24);
            info.blockHeightLog2 = le32(data, start + 0x6C);
            info.arrayLength = le32(data, start + 0x74);
            pendingInfo = info;
            ++layout.infoBlocks;
        } else if (type == kBlockData && pendingInfo) {
            layout.textures.push_back({*pendingInfo, start, qsizetype(size)});
            pendingInfo.reset();
        }
        pos = start + qsizetype(size);
    }
    return layout;
}
