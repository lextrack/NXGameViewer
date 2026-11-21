#include "textures/bntxreader.h"

#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QObject>
#include <QtEndian>

#include <algorithm>

namespace {

constexpr qsizetype kHeaderSize = 0x40;
constexpr qsizetype kInfoSize = 0xA0;
constexpr int kMaxMips = 17;

const char *const kEmbeddingContainers[] = {"FRES", "FFNT", "VFXB"};

struct Reader
{
    const QByteArray &data;
    qsizetype base;
    qsizetype end;

    bool fits(qsizetype at, qsizetype size) const { return at >= 0 && size >= 0 && at <= end - base - size; }
    quint16 u16(qsizetype at) const { return qFromLittleEndian<quint16>(data.constData() + base + at); }
    quint32 u32(qsizetype at) const { return qFromLittleEndian<quint32>(data.constData() + base + at); }
    quint64 u64(qsizetype at) const { return qFromLittleEndian<quint64>(data.constData() + base + at); }
    QByteArray bytes(qsizetype at, qsizetype size) const { return data.mid(base + at, size); }
};

QString astcName(quint32 type)
{
    static const char *const blocks[] = {"4×4", "5×4", "5×5", "6×5", "6×6", "8×5", "8×6", "8×8",
                                         "10×5", "10×6", "10×8", "10×10", "12×10", "12×12"};
    return QStringLiteral("ASTC %1").arg(QString::fromUtf8(blocks[type - 0x2D]));
}

}

std::optional<PixelFormat> bntxPixelFormat(quint32 format)
{
    const quint32 type = format >> 8, variant = format & 0xFF;
    switch (type) {
    case 0x02: return PixelFormat::R8;
    case 0x07: return PixelFormat::RGB565;
    case 0x09: return PixelFormat::RG8;
    case 0x0A: return PixelFormat::R16;
    case 0x0B: return PixelFormat::RGBA8;
    case 0x0C: return PixelFormat::BGRA8;
    case 0x15: return variant == 0x05 ? std::optional(PixelFormat::RGBA16F) : std::nullopt;
    case 0x1A: return PixelFormat::BC1;
    case 0x1B: return PixelFormat::BC2;
    case 0x1C: return PixelFormat::BC3;
    case 0x1D: return variant == 0x02 ? PixelFormat::BC4S : PixelFormat::BC4;
    case 0x1E: return variant == 0x02 ? PixelFormat::BC5S : PixelFormat::BC5;
    case 0x1F: return variant == 0x05 ? PixelFormat::BC6HS : PixelFormat::BC6H;
    case 0x20: return PixelFormat::BC7;
    default:
        if (type >= 0x2D && type <= 0x3A && (variant == 0x01 || variant == 0x06)) {
            static const int blocks[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                            {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
            return astcFormat(blocks[type - 0x2D][0], blocks[type - 0x2D][1], variant == 0x06);
        }
        return std::nullopt;
    }
}

QString bntxFormatName(quint32 format)
{
    const quint32 type = format >> 8, variant = format & 0xFF;
    QString name;
    if (const std::optional<PixelFormat> f = bntxPixelFormat(format))
        name = pixelFormatName(*f);
    else if (type >= 0x2D && type <= 0x3A)
        name = astcName(type);
    else
        name = QObject::tr("unknown");
    if (variant == 0x06 && !name.contains(QLatin1String("sRGB")))
        name += QStringLiteral(" sRGB");
    return name + QStringLiteral("  (BNTX 0x%1)").arg(format, 4, 16, QLatin1Char('0'));
}

QString channelSelectorText(quint32 channels)
{
    static const char *const names[] = {"0", "1", "R", "G", "B", "A"};
    QStringList parts;
    for (int i = 0; i < 4; ++i) {
        const quint32 s = (channels >> (8 * i)) & 0xFF;
        parts << (s < 6 ? QString::fromLatin1(names[s]) : QStringLiteral("?"));
    }
    return parts.join(u' ');
}

void applyChannelSelectors(QByteArray &rgba, quint32 channels)
{
    if (channels == 0x05040302)
        return;
    quint8 sel[4];
    for (int i = 0; i < 4; ++i) {
        sel[i] = quint8((channels >> (8 * i)) & 0xFF);
        if (sel[i] > 5)
            sel[i] = quint8(2 + i);
    }
    auto *p = reinterpret_cast<uchar *>(rgba.data());
    for (qsizetype i = 0; i + 4 <= rgba.size(); i += 4) {
        const uchar in[4] = {p[i], p[i + 1], p[i + 2], p[i + 3]};
        for (int c = 0; c < 4; ++c)
            p[i + c] = sel[c] == 0 ? 0 : sel[c] == 1 ? 255 : in[sel[c] - 2];
    }
}

QByteArray unapplyChannelSelectors(const QByteArray &shown, const QByteArray &stored, quint32 channels)
{
    if (channels == 0x05040302)
        return shown;
    QByteArray out = stored;
    const auto *s = reinterpret_cast<const uchar *>(shown.constData());
    auto *o = reinterpret_cast<uchar *>(out.data());
    const qsizetype size = std::min(shown.size(), out.size());
    for (int c = 3; c >= 0; --c) {
        const quint32 sel = (channels >> (8 * c)) & 0xFF;
        const int source = sel >= 2 && sel <= 5 ? int(sel) - 2 : sel > 5 ? c : -1;
        if (source < 0)
            continue;
        for (qsizetype i = 0; i + 4 <= size; i += 4)
            o[i + source] = s[i + c];
    }
    return out;
}

std::optional<BntxFile> parseBntx(const QByteArray &data, qsizetype offset, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<BntxFile> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (offset < 0 || offset > data.size() - kHeaderSize || data.mid(offset, 4) != "BNTX")
        return fail(QObject::tr("Not a BNTX texture (no \"BNTX\" at the start)."));
    Reader r{data, offset, data.size()};
    if (r.u16(0x0C) != 0xFEFF)
        return fail(QObject::tr("Only little-endian BNTX files are supported."));
    const QByteArray platform = r.bytes(0x20, 4);
    if (platform != "NX  " && platform != "Ounc")
        return fail(QObject::tr("The BNTX texture container is missing."));
    const bool swappedSectors = platform == "Ounc";
    const qsizetype declared = r.u32(0x1C);
    if (declared < kHeaderSize || declared > data.size() - offset)
        return fail(QObject::tr("The BNTX file is cut short (it should be %1 bytes).").arg(declared));
    r.end = offset + declared;

    BntxFile file;
    file.offset = offset;
    file.size = declared;
    const qsizetype count = r.u32(0x24);
    const quint64 pointers = r.u64(0x28);
    if (count > BntxReader::kMaxTextures)
        return fail(QObject::tr("The BNTX file says it has %1 textures.").arg(count));
    if (pointers > quint64(declared) || !r.fits(qsizetype(pointers), count * 8))
        return fail(QObject::tr("The BNTX texture list is outside the file."));
    const quint32 nameAt = r.u32(0x10);
    if (nameAt >= 2 && r.fits(nameAt - 2, 2) && r.fits(nameAt, r.u16(nameAt - 2)))
        file.name = QString::fromUtf8(r.bytes(nameAt, r.u16(nameAt - 2)));

    for (qsizetype i = 0; i < count; ++i) {
        const quint64 info = r.u64(qsizetype(pointers) + i * 8);
        if (info > quint64(declared) || !r.fits(qsizetype(info), kInfoSize) || r.bytes(qsizetype(info), 4) != "BRTI")
            return fail(QObject::tr("Texture %1 of the BNTX file is damaged.").arg(i + 1));
        const qsizetype b = qsizetype(info);
        BntxTexture t;
        t.infoOffset = b;
        t.dimension = uchar(data[offset + b + 0x11]);
        t.swappedGobSectors = swappedSectors;
        t.tileMode = r.u16(b + 0x12);
        t.mipCount = r.u16(b + 0x16);
        t.format = r.u32(b + 0x1C);
        t.width = int(r.u32(b + 0x24));
        t.height = int(r.u32(b + 0x28));
        t.depth = int(r.u32(b + 0x2C));
        t.arrayLength = int(r.u32(b + 0x30));
        t.blockHeightLog2 = int(r.u32(b + 0x34) & 7);
        t.imageSize = r.u32(b + 0x50);
        t.channels = r.u32(b + 0x58);
        const quint64 name = r.u64(b + 0x60);
        if (name < quint64(declared) && r.fits(qsizetype(name), 2) && r.fits(qsizetype(name) + 2, r.u16(qsizetype(name))))
            t.name = QString::fromUtf8(r.bytes(qsizetype(name) + 2, r.u16(qsizetype(name))));
        if (t.name.isEmpty())
            t.name = QObject::tr("Texture %1").arg(i + 1);
        if (t.mipCount < 1 || t.mipCount > kMaxMips)
            return fail(QObject::tr("%1 has %2 mip levels.").arg(t.name).arg(t.mipCount));
        const quint64 mips = r.u64(b + 0x70);
        if (mips > quint64(declared) || !r.fits(qsizetype(mips), qsizetype(t.mipCount) * 8))
            return fail(QObject::tr("The mip levels of %1 are outside the file.").arg(t.name));
        for (int m = 0; m < t.mipCount; ++m) {
            const quint64 at = r.u64(qsizetype(mips) + m * 8);
            if (at > quint64(declared))
                return fail(QObject::tr("Mip level %1 of %2 is outside the file.").arg(m).arg(t.name));
            t.mipOffsets.push_back(qsizetype(at));
        }
        if (t.imageSize < 0 || !r.fits(t.mipOffsets[0], t.imageSize))
            return fail(QObject::tr("The pixel data of %1 is outside the file.").arg(t.name));
        file.textures.push_back(std::move(t));
    }
    return file;
}

std::vector<qsizetype> findEmbeddedBntx(const QByteArray &data)
{
    std::vector<qsizetype> found;
    qsizetype at = data.indexOf("BNTX", 4);
    while (at >= 0 && found.size() < size_t(BntxReader::kMaxEmbedded)) {
        if (const std::optional<BntxFile> file = parseBntx(data, at, nullptr)) {
            found.push_back(at);
            at = data.indexOf("BNTX", at + std::max<qsizetype>(file->size, 4));
        } else {
            at = data.indexOf("BNTX", at + 4);
        }
    }
    return found;
}

QString bntxTextureProblem(const BntxTexture &t)
{
    const std::optional<PixelFormat> format = bntxPixelFormat(t.format);
    if (!format)
        return QObject::tr("Pixel format %1 cannot be shown yet").arg(bntxFormatName(t.format));
    if (t.width <= 0 || t.height <= 0 || t.width > kMaxTextureSide || t.height > kMaxTextureSide)
        return QObject::tr("Unexpected size %1 × %2").arg(t.width).arg(t.height);
    if (t.tileMode != 0)
        return QObject::tr("Linear (not block-linear) textures cannot be shown yet");
    if (t.mipOffsets.empty())
        return QObject::tr("The texture has no pixel data");
    const int bpe = bytesPerElement(*format);
    const qsizetype size = blockLinearSize(elementColumns(*format, t.width), elementRows(*format, t.height), bpe,
                                           1 << t.blockHeightLog2);
    if (size > t.imageSize)
        return QObject::tr("The pixel data is shorter than the texture needs");
    return {};
}

std::optional<Image> decodeBntxTexture(const QByteArray &data, qsizetype bntxOffset, const BntxTexture &t)
{
    if (!bntxTextureProblem(t).isEmpty())
        return std::nullopt;
    const PixelFormat format = *bntxPixelFormat(t.format);
    const int bpe = bytesPerElement(format);
    const int columns = elementColumns(format, t.width);
    const int rows = elementRows(format, t.height);
    const int blockHeight = 1 << t.blockHeightLog2;
    const qsizetype size = blockLinearSize(columns, rows, bpe, blockHeight);
    const qsizetype start = bntxOffset + t.mipOffsets[0];
    if (start < 0 || start > data.size() - size)
        return std::nullopt;
    QByteArray swizzled = data.mid(start, size);
    if (t.swappedGobSectors)
        swapGobSectors(swizzled);
    const std::optional<QByteArray> linear = deswizzleBlockLinear(swizzled, columns, rows, bpe, blockHeight);
    std::optional<QByteArray> rgba = linear ? decodePixels(format, *linear, t.width, t.height) : std::nullopt;
    if (!rgba)
        return std::nullopt;
    applyChannelSelectors(*rgba, t.channels);
    Image image;
    image.width = t.width;
    image.height = t.height;
    image.rgba = std::move(*rgba);
    return image;
}

int BntxReader::score(const QString &, QByteArrayView head) const
{
    if (head.startsWith("BNTX"))
        return 100;
    for (const char *magic : kEmbeddingContainers)
        if (head.startsWith(magic))
            return 60;
    return 0;
}

std::unique_ptr<Document> BntxReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> BntxReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    std::vector<qsizetype> offsets;
    if (data.startsWith("BNTX")) {
        offsets.push_back(0);
    } else {
        offsets = findEmbeddedBntx(data);
        if (offsets.empty()) {
            if (error)
                *error = QObject::tr("No BNTX textures were found in this file.");
            return nullptr;
        }
    }

    auto doc = std::make_unique<Document>();
    for (qsizetype offset : offsets) {
        const std::optional<BntxFile> file = parseBntx(data, offset, error);
        if (!file)
            return nullptr;
        for (size_t i = 0; i < file->textures.size(); ++i) {
            const BntxTexture &t = file->textures[i];
            std::vector<SimpleTable::Row> rows = {
                {QObject::tr("name"), t.name},
                {QObject::tr("width"), t.width},
                {QObject::tr("height"), t.height},
                {QObject::tr("pixel format"), bntxFormatName(t.format)},
                {QObject::tr("channels"), channelSelectorText(t.channels)},
                {QObject::tr("mip levels"), t.mipCount},
                {QObject::tr("array length"), t.arrayLength},
                {QObject::tr("depth"), t.depth},
                {QObject::tr("block height"), QObject::tr("%1 GOBs").arg(1 << t.blockHeightLog2)},
                {QObject::tr("data size"), qint64(t.imageSize)},
            };
            if (offset > 0)
                rows.push_back({QObject::tr("BNTX offset"), QStringLiteral("0x%1").arg(offset, 0, 16)});
            QString name = t.name;
            if (offsets.size() > 1 && !file->name.isEmpty())
                name = QStringLiteral("%1 / %2").arg(file->name, t.name);
            auto table = std::make_shared<SimpleTable>(
                name, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows));

            QString problem = bntxTextureProblem(t);
            if (!problem.isEmpty()) {
                table->setDescription(problem);
            } else {
                table->setImageMaker([data, offset, t] {
                    std::optional<Image> image = decodeBntxTexture(data, offset, t);
                    return image ? std::move(*image) : Image{};
                });
                table->setDescription(
                    QObject::tr("%1 × %2, %3").arg(t.width).arg(t.height).arg(bntxFormatName(t.format)));
            }
            table->setTextureSlot({offset, int(i), t.name});
            doc->tables.push_back(std::move(table));
        }
    }
    if (doc->tables.empty()) {
        if (error)
            *error = QObject::tr("The BNTX file has no textures.");
        return nullptr;
    }
    return doc;
}
