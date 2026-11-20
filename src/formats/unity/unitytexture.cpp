#include "unity/unitytexture.h"

#include "textures/texturedecode.h"

#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace {

constexpr qint32 kSwitch = 38;

enum UnityFormat {
    Alpha8 = 1, ARGB4444 = 2, RGB24 = 3, RGBA32 = 4, ARGB32 = 5, RGB565 = 7, R16 = 9, DXT1 = 10, DXT5 = 12,
    RGBA4444 = 13, BGRA32 = 14, BC6H = 24, BC7 = 25, BC4 = 26, BC5 = 27, DXT1Crunched = 28, DXT5Crunched = 29,
    ASTC_RGB_4x4 = 48, ASTC_RGBA_12x12 = 59, RG16 = 62, R8 = 63, ETC_RGB4Crunched = 64, ETC2_RGBA8Crunched = 65,
};

struct Layout
{
    int blockWidth = 1;
    int blockHeight = 1;
    int bytes = 4;
    std::optional<PixelFormat> gpu;
};

std::optional<Layout> layoutOf(int format)
{
    if (format >= ASTC_RGB_4x4 && format <= ASTC_RGBA_12x12) {
        static constexpr int sizes[] = {4, 5, 6, 8, 10, 12};
        const int s = sizes[(format - ASTC_RGB_4x4) % 6];
        return Layout{s, s, 16, astcFormat(s, s, false)};
    }
    switch (format) {
    case Alpha8: case R8: return Layout{1, 1, 1, {}};
    case ARGB4444: case RGBA4444: case RGB565: case R16: case RG16: return Layout{1, 1, 2, {}};
    case RGB24: return Layout{1, 1, 3, {}};
    case RGBA32: case ARGB32: case BGRA32: return Layout{1, 1, 4, {}};
    case DXT1: return Layout{4, 4, 8, PixelFormat::BC1};
    case DXT5: return Layout{4, 4, 16, PixelFormat::BC3};
    case BC4: return Layout{4, 4, 8, PixelFormat::BC4};
    case BC5: return Layout{4, 4, 16, PixelFormat::BC5};
    case BC7: return Layout{4, 4, 16, PixelFormat::BC7};
    default: return std::nullopt;
    }
}

QByteArray expandPixels(int format, QByteArrayView src, qsizetype pixels)
{
    QByteArray out(pixels * 4, Qt::Uninitialized);
    auto *o = reinterpret_cast<uchar *>(out.data());
    const auto *s = reinterpret_cast<const uchar *>(src.data());
    for (qsizetype p = 0; p < pixels; ++p, o += 4) {
        switch (format) {
        case Alpha8: o[0] = o[1] = o[2] = 255; o[3] = s[p]; break;
        case R8: o[0] = o[1] = o[2] = s[p]; o[3] = 255; break;
        case R16: o[0] = o[1] = o[2] = s[p * 2 + 1]; o[3] = 255; break;
        case RG16: o[0] = s[p * 2]; o[1] = s[p * 2 + 1]; o[2] = 0; o[3] = 255; break;
        case RGB24: o[0] = s[p * 3]; o[1] = s[p * 3 + 1]; o[2] = s[p * 3 + 2]; o[3] = 255; break;
        case RGBA32: std::memcpy(o, s + p * 4, 4); break;
        case ARGB32: o[0] = s[p * 4 + 1]; o[1] = s[p * 4 + 2]; o[2] = s[p * 4 + 3]; o[3] = s[p * 4]; break;
        case BGRA32: o[0] = s[p * 4 + 2]; o[1] = s[p * 4 + 1]; o[2] = s[p * 4]; o[3] = s[p * 4 + 3]; break;
        case RGB565: {
            const quint16 v = qFromLittleEndian<quint16>(s + p * 2);
            o[0] = uchar((v >> 11) * 255 / 31);
            o[1] = uchar(((v >> 5) & 63) * 255 / 63);
            o[2] = uchar((v & 31) * 255 / 31);
            o[3] = 255;
            break;
        }
        case ARGB4444:
        case RGBA4444: {
            const quint16 v = qFromLittleEndian<quint16>(s + p * 2);
            const int n[4] = {(v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15};
            const int order[4] = {1, 2, 3, 0};
            for (int c = 0; c < 4; ++c)
                o[c] = uchar((format == ARGB4444 ? n[order[c]] : n[c]) * 17);
            break;
        }
        default: std::memset(o, 0, 4); break;
        }
    }
    return out;
}

}

int switchGobsPerBlock(const UValue &texture, qint32 platform)
{
    const UValue *blob = texture.child(u"m_PlatformBlob");
    if (platform != kSwitch || !blob || blob->kind != UValue::Kind::Bytes || blob->bytes.size() < 12)
        return 0;
    return 1 << std::min(5u, qFromLittleEndian<quint32>(blob->bytes.constData() + 8));
}

QString unityTextureFormatName(int format)
{
    if (format >= ASTC_RGB_4x4 && format <= ASTC_RGBA_12x12) {
        static constexpr int sizes[] = {4, 5, 6, 8, 10, 12};
        const int s = sizes[(format - ASTC_RGB_4x4) % 6];
        return QStringLiteral("ASTC %1×%1 (%2)").arg(s).arg(format < ASTC_RGB_4x4 + 6 ? "RGB" : "RGBA");
    }
    switch (format) {
    case Alpha8: return QStringLiteral("Alpha8");
    case ARGB4444: return QStringLiteral("ARGB4444");
    case RGB24: return QStringLiteral("RGB24");
    case RGBA32: return QStringLiteral("RGBA32");
    case ARGB32: return QStringLiteral("ARGB32");
    case RGB565: return QStringLiteral("RGB565");
    case R16: return QStringLiteral("R16");
    case DXT1: return QStringLiteral("DXT1 (BC1)");
    case DXT5: return QStringLiteral("DXT5 (BC3)");
    case RGBA4444: return QStringLiteral("RGBA4444");
    case BGRA32: return QStringLiteral("BGRA32");
    case BC6H: return QStringLiteral("BC6H");
    case BC7: return QStringLiteral("BC7");
    case BC4: return QStringLiteral("BC4");
    case BC5: return QStringLiteral("BC5");
    case DXT1Crunched: return QStringLiteral("DXT1 Crunched");
    case DXT5Crunched: return QStringLiteral("DXT5 Crunched");
    case RG16: return QStringLiteral("RG16");
    case R8: return QStringLiteral("R8");
    default: return QObject::tr("format %1").arg(format);
    }
}

std::optional<Image> decodeUnityTexture(const UValue &texture, QByteArrayView imageData, qint32 platform,
                                        QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Image> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const UValue *w = texture.child(u"m_Width"), *h = texture.child(u"m_Height"),
                 *f = texture.child(u"m_TextureFormat");
    if (!w || !h || !f)
        return fail(QObject::tr("The texture has no size or format."));
    const int width = int(w->toInt()), height = int(h->toInt());
    int format = int(f->toInt());
    if (width <= 0 || height <= 0 || width > kMaxTextureSide || height > kMaxTextureSide)
        return fail(QObject::tr("The texture size %1×%2 is not valid.").arg(width).arg(height));
    if (imageData.isEmpty())
        return fail(QObject::tr("The texture's pixels are stored in another file that is not available here."));

    const int gobs = switchGobsPerBlock(texture, platform);
    const bool swizzled = gobs > 0;
    if (swizzled && format == RGB24)
        format = RGBA32;

    const std::optional<Layout> layout = layoutOf(format);
    if (!layout)
        return fail(QObject::tr("Textures in %1 are not supported yet.").arg(unityTextureFormatName(format)));
    const int columns = (width + layout->blockWidth - 1) / layout->blockWidth;
    const int rows = (height + layout->blockHeight - 1) / layout->blockHeight;
    QByteArray linear;
    if (swizzled) {
        QByteArray padded = imageData.toByteArray();
        const qsizetype needed = blockLinearSize(columns, rows, layout->bytes, gobs);
        if (padded.size() < needed)
            padded.append(needed - padded.size(), '\0');
        const std::optional<QByteArray> out = deswizzleBlockLinear(padded, columns, rows, layout->bytes, gobs);
        if (!out)
            return fail(QObject::tr("The texture's Switch layout could not be undone."));
        linear = *out;
    } else {
        linear = imageData.first(std::min<qsizetype>(imageData.size(), qsizetype(columns) * rows * layout->bytes)).toByteArray();
    }
    if (linear.size() < qsizetype(columns) * rows * layout->bytes)
        return fail(QObject::tr("The texture data is shorter than its size needs."));

    Image image{width, height, {}};
    if (layout->gpu) {
        const std::optional<QByteArray> rgba = decodePixels(*layout->gpu, linear, width, height);
        if (!rgba)
            return fail(QObject::tr("The %1 texture could not be decoded.").arg(unityTextureFormatName(format)));
        image.rgba = *rgba;
    } else {
        image.rgba = expandPixels(format, linear, qsizetype(width) * height);
    }
    const qsizetype stride = qsizetype(width) * 4;
    QByteArray flipped(image.rgba.size(), Qt::Uninitialized);
    for (int y = 0; y < height; ++y)
        std::memcpy(flipped.data() + y * stride, image.rgba.constData() + (height - 1 - y) * stride, size_t(stride));
    image.rgba = flipped;
    return image;
}

Image cropImage(const Image &image, int x, int y, int width, int height)
{
    x = std::clamp(x, 0, image.width);
    y = std::clamp(y, 0, image.height);
    width = std::clamp(width, 0, image.width - x);
    height = std::clamp(height, 0, image.height - y);
    Image out{width, height, QByteArray(qsizetype(width) * height * 4, Qt::Uninitialized)};
    for (int row = 0; row < height; ++row)
        std::memcpy(out.rgba.data() + qsizetype(row) * width * 4,
                    image.rgba.constData() + (qsizetype(y + row) * image.width + x) * 4, size_t(width) * 4);
    return out;
}
