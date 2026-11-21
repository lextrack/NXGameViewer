#include "textures/ddsreader.h"

#include "core/bytes.h"
#include "core/simpletable.h"
#include "core/storedreader.h"
#include "textures/texturedecode.h"

#include <QObject>

#include <optional>

namespace {

constexpr quint32 kFourCC = 0x4;
constexpr quint32 kRgb = 0x40;
constexpr quint32 kAlphaOnly = 0x2;
constexpr quint32 kLuminance = 0x20000;
constexpr quint32 kCubemap = 0x200;

std::optional<PixelFormat> fromFourCC(QByteArrayView code)
{
    struct Entry { const char *code; PixelFormat format; };
    static const Entry table[] = {
        {"DXT1", PixelFormat::BC1}, {"DXT2", PixelFormat::BC2}, {"DXT3", PixelFormat::BC2},
        {"DXT4", PixelFormat::BC3}, {"DXT5", PixelFormat::BC3}, {"ATI1", PixelFormat::BC4},
        {"BC4U", PixelFormat::BC4}, {"BC4S", PixelFormat::BC4S}, {"ATI2", PixelFormat::BC5},
        {"BC5U", PixelFormat::BC5}, {"BC5S", PixelFormat::BC5S},
    };
    for (const Entry &e : table)
        if (code == QByteArrayView(e.code, 4))
            return e.format;
    return std::nullopt;
}

std::optional<PixelFormat> fromDxgi(quint32 dxgi)
{
    switch (dxgi) {
    case 28: case 29: return PixelFormat::RGBA8;
    case 87: case 91: return PixelFormat::BGRA8;
    case 61: return PixelFormat::R8;
    case 49: return PixelFormat::RG8;
    case 71: case 72: return PixelFormat::BC1;
    case 74: case 75: return PixelFormat::BC2;
    case 77: case 78: return PixelFormat::BC3;
    case 80: return PixelFormat::BC4;
    case 81: return PixelFormat::BC4S;
    case 83: return PixelFormat::BC5;
    case 84: return PixelFormat::BC5S;
    case 10: return PixelFormat::RGBA16F;
    case 56: return PixelFormat::R16;
    case 95: return PixelFormat::BC6H;
    case 96: return PixelFormat::BC6HS;
    case 98: case 99: return PixelFormat::BC7;
    default: return std::nullopt;
    }
}

std::unique_ptr<Document> parse(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < 128 || !data.startsWith("DDS ") || le32(data, 4) != 124)
        return fail(QObject::tr("Not a DDS texture (no \"DDS \" header)."));
    const int height = int(le32(data, 12)), width = int(le32(data, 16));
    const quint32 mips = std::max<quint32>(1, le32(data, 28));
    const quint32 pfFlags = le32(data, 80);
    const QByteArrayView fourCC = QByteArrayView(data).sliced(84, 4);
    const bool cubemap = le32(data, 112) & kCubemap;
    if (width <= 0 || height <= 0 || width > kMaxTextureSide || height > kMaxTextureSide)
        return fail(QObject::tr("The DDS texture has an unexpected size."));

    qsizetype start = 128;
    std::optional<PixelFormat> format;
    std::optional<PixelMasks> masks;
    QString formatName;
    quint32 arraySize = 1;
    if ((pfFlags & kFourCC) && fourCC == "DX10") {
        if (data.size() < 148)
            return fail(QObject::tr("The DDS texture's DX10 header is cut off."));
        const quint32 dxgi = le32(data, 128);
        arraySize = std::max<quint32>(1, le32(data, 140));
        start = 148;
        format = fromDxgi(dxgi);
        formatName = format ? pixelFormatName(*format) : QObject::tr("DXGI format %1").arg(dxgi);
    } else if (pfFlags & kFourCC) {
        format = fromFourCC(fourCC);
        formatName = format ? pixelFormatName(*format) : QString::fromLatin1(fourCC.toByteArray());
    } else if (pfFlags & (kRgb | kLuminance | kAlphaOnly)) {
        PixelMasks m;
        m.bits = int(le32(data, 88));
        m.r = le32(data, 92);
        m.g = le32(data, 96);
        m.b = le32(data, 100);
        m.a = le32(data, 104);
        m.luminance = (pfFlags & kLuminance) && !(pfFlags & kRgb);
        if (pfFlags & kAlphaOnly && !(pfFlags & (kRgb | kLuminance)))
            m.r = m.g = m.b = 0;
        if (m.bits == 8 || m.bits == 16 || m.bits == 24 || m.bits == 32) {
            masks = m;
            formatName = QObject::tr("%1-bit uncompressed").arg(m.bits);
        }
    }
    const int faces = cubemap ? 6 : 1;

    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("size"), QStringLiteral("%1 × %2").arg(width).arg(height)},
        {QObject::tr("format"), formatName.isEmpty() ? QObject::tr("unknown") : formatName},
        {QObject::tr("mip levels"), qint64(mips)},
    };
    if (cubemap)
        rows.push_back({QObject::tr("faces"), QObject::tr("6 (cubemap)")});
    if (arraySize > 1)
        rows.push_back({QObject::tr("array layers"), qint64(arraySize)});
    auto table = std::make_shared<SimpleTable>(QObject::tr("Texture"),
                                               std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                               std::move(rows));

    qsizetype needed = 0;
    if (format)
        needed = qsizetype(elementColumns(*format, width)) * elementRows(*format, height) * bytesPerElement(*format);
    else if (masks)
        needed = qsizetype(width) * height * (masks->bits / 8);
    if (!format && !masks) {
        table->setDescription(QObject::tr("DDS texture in %1, which cannot be shown yet").arg(formatName));
    } else if (start + needed > data.size()) {
        table->setDescription(QObject::tr("The DDS pixel data is shorter than the texture needs"));
    } else {
        const QByteArray pixels = data.mid(start, needed);
        table->setDescription(QObject::tr("%1 × %2 %3 DDS texture").arg(width).arg(height).arg(formatName)
                              + (faces * arraySize > 1 || mips > 1 ? QObject::tr(" (showing the first image)") : QString()));
        table->setImageMaker([pixels, format, masks, width, height] {
            Image image;
            std::optional<QByteArray> rgba =
                format ? decodePixels(*format, pixels, width, height) : decodeMaskedPixels(pixels, width, height, *masks);
            if (!rgba)
                return image;
            image.width = width;
            image.height = height;
            image.rgba = std::move(*rgba);
            return image;
        });
    }
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

}

int DdsReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("DDS ") ? 100 : 0;
}

std::unique_ptr<Document> DdsReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return parse(*bytes, error);
}

std::unique_ptr<Document> DdsReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    return parse(data, error);
}
