#include "textures/texturedecode.h"

#include "core/bytes.h"
#include "textures/astc.h"

#include <QtEndian>

#include <algorithm>
#include <bit>
#include <array>
#include <cstring>

namespace {

using Rgba = std::array<quint8, 4>;

quint8 expand(quint32 value, int bits)
{
    return quint8((value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

void setPixel(uchar *out, int width, int x, int y, Rgba c)
{
    std::memcpy(out + (qsizetype(y) * width + x) * 4, c.data(), 4);
}

Rgba from565(quint16 v)
{
    quint8 r = quint8((v & 0xF800) >> 8), g = quint8((v & 0x07E0) >> 3), b = quint8((v & 0x001F) << 3);
    return {quint8(r | r >> 5), quint8(g | g >> 6), quint8(b | b >> 5), 255};
}

void decodeBc1Colours(const uchar *block, Rgba out[16], bool alwaysFourColours)
{
    const quint16 c0 = le16(block), c1 = le16(block + 2);
    Rgba p[4] = {from565(c0), from565(c1), {}, {}};
    if (c0 > c1 || alwaysFourColours) {
        for (int i = 0; i < 3; ++i) {
            p[2][i] = quint8((2 * p[0][i] + p[1][i]) / 3);
            p[3][i] = quint8((p[0][i] + 2 * p[1][i]) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i)
            p[2][i] = quint8((p[0][i] + p[1][i]) / 2);
        p[2][3] = 255;
        p[3] = {0, 0, 0, 0};
    }
    const quint32 indices = le32(block + 4);
    for (int i = 0; i < 16; ++i)
        out[i] = p[(indices >> (2 * i)) & 3];
}

void decodeBc4Channel(const uchar *block, quint8 out[16], bool isSigned)
{
    int a0 = block[0], a1 = block[1];
    if (isSigned) {
        a0 = std::max(-127, int(qint8(block[0])));
        a1 = std::max(-127, int(qint8(block[1])));
    }
    int p[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            p[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; ++i)
            p[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        p[6] = isSigned ? -127 : 0;
        p[7] = isSigned ? 127 : 255;
    }
    quint64 bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= quint64(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
        const int v = p[(bits >> (3 * i)) & 7];
        out[i] = isSigned ? quint8((v + 127) * 255 / 254) : quint8(v);
    }
}

struct Bc7Mode
{
    int subsets, partitionBits, rotationBits, indexSelectionBits, colorBits, alphaBits;
    int endpointPBits, sharedPBits, indexBits, indexBits2;
};

constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

constexpr quint16 kPartition2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

constexpr const char *kPartition3[64] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122",
    "0011001100220022", "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222",
    "0000111122222222", "0012001200120012", "0112011201120112", "0122012201220122", "0011011211221222",
    "0011200122002220", "0001001101121122", "0111001120012200", "0000112211221122", "0022002200221111",
    "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210", "0122012200110000",
    "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222",
    "0120012001200120", "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011",
    "0011112222000011", "0101010122222222", "0000000021212121", "0022112200221122", "0022001100220011",
    "0220122102201221", "0101222222220101", "0000212121212121", "0101010101012222", "0222011102220111",
    "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002", "0110011001102222",
    "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220",
};

constexpr quint8 kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
    15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
constexpr quint8 kAnchor3a[64] = {
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
    8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3,
};
constexpr quint8 kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
    15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr int kWeights2[4] = {0, 21, 43, 64};
constexpr int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const int *weightsFor(int bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

class BitReader
{
public:
    explicit BitReader(const uchar *block) : m_lo(qFromLittleEndian<quint64>(block)), m_hi(qFromLittleEndian<quint64>(block + 8)) {}
    quint32 read(int count)
    {
        quint32 value = 0;
        for (int i = 0; i < count; ++i, ++m_pos) {
            const quint64 word = m_pos < 64 ? m_lo : m_hi;
            value |= quint32((word >> (m_pos & 63)) & 1) << i;
        }
        return value;
    }

private:
    quint64 m_lo, m_hi;
    int m_pos = 0;
};

int subsetOf(const Bc7Mode &mode, int partition, int pixel)
{
    if (mode.subsets == 2)
        return (kPartition2[partition] >> pixel) & 1;
    if (mode.subsets == 3)
        return kPartition3[partition][pixel] - '0';
    return 0;
}

bool isAnchor(const Bc7Mode &mode, int partition, int pixel)
{
    if (pixel == 0)
        return true;
    if (mode.subsets == 2)
        return pixel == kAnchor2[partition];
    if (mode.subsets == 3)
        return pixel == kAnchor3a[partition] || pixel == kAnchor3b[partition];
    return false;
}

void decodeBc7Block(const uchar *block, Rgba out[16])
{
    int modeIndex = 0;
    while (modeIndex < 8 && !(block[0] & (1 << modeIndex)))
        ++modeIndex;
    if (modeIndex == 8) {
        std::fill(out, out + 16, Rgba{0, 0, 0, 0});
        return;
    }
    const Bc7Mode &mode = kBc7Modes[modeIndex];
    BitReader bits(block);
    bits.read(modeIndex + 1);

    const int partition = int(bits.read(mode.partitionBits));
    const int rotation = int(bits.read(mode.rotationBits));
    const int indexSelection = int(bits.read(mode.indexSelectionBits));

    int endpoints[6][4] = {};
    const int endpointCount = mode.subsets * 2;
    for (int channel = 0; channel < 3; ++channel)
        for (int e = 0; e < endpointCount; ++e)
            endpoints[e][channel] = int(bits.read(mode.colorBits));
    for (int e = 0; e < endpointCount; ++e)
        endpoints[e][3] = mode.alphaBits ? int(bits.read(mode.alphaBits)) : 255;

    int colorBits = mode.colorBits, alphaBits = mode.alphaBits;
    if (mode.endpointPBits || mode.sharedPBits) {
        int pbits[6] = {};
        if (mode.endpointPBits) {
            for (int e = 0; e < endpointCount; ++e)
                pbits[e] = int(bits.read(1));
        } else {
            for (int s = 0; s < mode.subsets; ++s)
                pbits[s * 2] = pbits[s * 2 + 1] = int(bits.read(1));
        }
        for (int e = 0; e < endpointCount; ++e) {
            for (int c = 0; c < 3; ++c)
                endpoints[e][c] = endpoints[e][c] << 1 | pbits[e];
            if (mode.alphaBits)
                endpoints[e][3] = endpoints[e][3] << 1 | pbits[e];
        }
        ++colorBits;
        if (mode.alphaBits)
            ++alphaBits;
    }
    for (int e = 0; e < endpointCount; ++e) {
        for (int c = 0; c < 3; ++c)
            endpoints[e][c] = (endpoints[e][c] << (8 - colorBits)) | (endpoints[e][c] >> (2 * colorBits - 8));
        if (mode.alphaBits)
            endpoints[e][3] = (endpoints[e][3] << (8 - alphaBits)) | (endpoints[e][3] >> (2 * alphaBits - 8));
    }

    int indices[16], indices2[16] = {};
    for (int i = 0; i < 16; ++i)
        indices[i] = int(bits.read(mode.indexBits - (isAnchor(mode, partition, i) ? 1 : 0)));
    if (mode.indexBits2) {
        for (int i = 0; i < 16; ++i)
            indices2[i] = int(bits.read(mode.indexBits2 - (i == 0 ? 1 : 0)));
    }

    for (int i = 0; i < 16; ++i) {
        const int s = subsetOf(mode, partition, i);
        const int *e0 = endpoints[s * 2], *e1 = endpoints[s * 2 + 1];
        int colorWeight, alphaWeight;
        if (!mode.indexBits2) {
            colorWeight = alphaWeight = weightsFor(mode.indexBits)[indices[i]];
        } else if (indexSelection == 0) {
            colorWeight = weightsFor(mode.indexBits)[indices[i]];
            alphaWeight = weightsFor(mode.indexBits2)[indices2[i]];
        } else {
            colorWeight = weightsFor(mode.indexBits2)[indices2[i]];
            alphaWeight = weightsFor(mode.indexBits)[indices[i]];
        }
        Rgba px;
        for (int c = 0; c < 3; ++c)
            px[c] = quint8(((64 - colorWeight) * e0[c] + colorWeight * e1[c] + 32) >> 6);
        px[3] = quint8(((64 - alphaWeight) * e0[3] + alphaWeight * e1[3] + 32) >> 6);
        if (rotation)
            std::swap(px[3], px[rotation - 1]);
        out[i] = px;
    }
}

Rgba decodeUncompressed(PixelFormat format, const uchar *p)
{
    switch (format) {
    case PixelFormat::RGBA8: return {p[0], p[1], p[2], p[3]};
    case PixelFormat::BGRA8: return {p[2], p[1], p[0], p[3]};
    case PixelFormat::R8: return {p[0], p[0], p[0], 255};
    case PixelFormat::RG8: return {p[0], p[1], 0, 255};
    case PixelFormat::RGB565: {
        const quint16 v = le16(p);
        return {expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5), 255};
    }
    case PixelFormat::RGBA4: {
        const quint16 v = le16(p);
        return {expand(v & 15, 4), expand((v >> 4) & 15, 4), expand((v >> 8) & 15, 4), expand(v >> 12, 4)};
    }
    case PixelFormat::RGB5A1: {
        const quint16 v = le16(p);
        return {expand(v & 31, 5), expand((v >> 5) & 31, 5), expand((v >> 10) & 31, 5), quint8(v & 0x8000 ? 255 : 0)};
    }
    case PixelFormat::RGB10A2: {
        const quint32 v = le32(p);
        return {quint8((v >> 2) & 255), quint8((v >> 12) & 255), quint8((v >> 22) & 255), expand(v >> 30, 2)};
    }
    default: return {};
    }
}

void decodeBlock(PixelFormat format, const uchar *block, Rgba out[16])
{
    quint8 a[16], b[16];
    switch (format) {
    case PixelFormat::BC1:
        decodeBc1Colours(block, out, false);
        break;
    case PixelFormat::BC2:
        decodeBc1Colours(block + 8, out, true);
        for (int i = 0; i < 16; ++i)
            out[i][3] = quint8(((block[i / 2] >> (4 * (i % 2))) & 15) * 17);
        break;
    case PixelFormat::BC3:
        decodeBc1Colours(block + 8, out, true);
        decodeBc4Channel(block, a, false);
        for (int i = 0; i < 16; ++i)
            out[i][3] = a[i];
        break;
    case PixelFormat::BC4:
    case PixelFormat::BC4S:
        decodeBc4Channel(block, a, format == PixelFormat::BC4S);
        for (int i = 0; i < 16; ++i)
            out[i] = {a[i], a[i], a[i], 255};
        break;
    case PixelFormat::BC5:
    case PixelFormat::BC5S:
        decodeBc4Channel(block, a, format == PixelFormat::BC5S);
        decodeBc4Channel(block + 8, b, format == PixelFormat::BC5S);
        for (int i = 0; i < 16; ++i)
            out[i] = {a[i], b[i], 0, 255};
        break;
    case PixelFormat::BC7:
        decodeBc7Block(block, out);
        break;
    default:
        break;
    }
}

int pow2AtLeast(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

}

QString pixelFormatName(PixelFormat format)
{
    switch (format) {
    case PixelFormat::RGBA8: return QStringLiteral("RGBA8");
    case PixelFormat::BGRA8: return QStringLiteral("BGRA8");
    case PixelFormat::R8: return QStringLiteral("R8");
    case PixelFormat::RG8: return QStringLiteral("RG8");
    case PixelFormat::RGB565: return QStringLiteral("RGB565");
    case PixelFormat::RGBA4: return QStringLiteral("RGBA4");
    case PixelFormat::RGB5A1: return QStringLiteral("RGB5A1");
    case PixelFormat::RGB10A2: return QStringLiteral("RGB10A2");
    case PixelFormat::BC1: return QStringLiteral("BC1 (DXT1)");
    case PixelFormat::BC2: return QStringLiteral("BC2 (DXT3)");
    case PixelFormat::BC3: return QStringLiteral("BC3 (DXT5)");
    case PixelFormat::BC4: return QStringLiteral("BC4");
    case PixelFormat::BC4S: return QStringLiteral("BC4 signed");
    case PixelFormat::BC5: return QStringLiteral("BC5");
    case PixelFormat::BC5S: return QStringLiteral("BC5 signed");
    case PixelFormat::BC7: return QStringLiteral("BC7");
    case PixelFormat::ASTC_4x4:
    case PixelFormat::ASTC_4x4_SRGB:
    case PixelFormat::ASTC_5x4:
    case PixelFormat::ASTC_5x4_SRGB:
    case PixelFormat::ASTC_5x5:
    case PixelFormat::ASTC_5x5_SRGB:
    case PixelFormat::ASTC_6x5:
    case PixelFormat::ASTC_6x5_SRGB:
    case PixelFormat::ASTC_6x6:
    case PixelFormat::ASTC_6x6_SRGB:
    case PixelFormat::ASTC_8x5:
    case PixelFormat::ASTC_8x5_SRGB:
    case PixelFormat::ASTC_8x6:
    case PixelFormat::ASTC_8x6_SRGB:
    case PixelFormat::ASTC_8x8:
    case PixelFormat::ASTC_8x8_SRGB:
    case PixelFormat::ASTC_10x5:
    case PixelFormat::ASTC_10x5_SRGB:
    case PixelFormat::ASTC_10x6:
    case PixelFormat::ASTC_10x6_SRGB:
    case PixelFormat::ASTC_10x8:
    case PixelFormat::ASTC_10x8_SRGB:
    case PixelFormat::ASTC_10x10:
    case PixelFormat::ASTC_10x10_SRGB:
    case PixelFormat::ASTC_12x10:
    case PixelFormat::ASTC_12x10_SRGB:
    case PixelFormat::ASTC_12x12:
    case PixelFormat::ASTC_12x12_SRGB:
        return QStringLiteral("ASTC %1×%2").arg(elementWidth(format)).arg(elementHeight(format))
               + (isSrgb(format) ? QStringLiteral(" sRGB") : QString());
    }
    return {};
}

bool isAstc(PixelFormat format) { return format >= PixelFormat::ASTC_4x4; }

bool isSrgb(PixelFormat format) { return format >= PixelFormat::ASTC_4x4_SRGB; }

namespace {

constexpr int kAstcBlocks[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                  {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
constexpr int kAstcCount = int(std::size(kAstcBlocks));

int astcIndex(PixelFormat format)
{
    return (int(format) - int(PixelFormat::ASTC_4x4)) % kAstcCount;
}

}

int elementWidth(PixelFormat format)
{
    if (isAstc(format))
        return kAstcBlocks[astcIndex(format)][0];
    return format >= PixelFormat::BC1 ? 4 : 1;
}

int elementHeight(PixelFormat format)
{
    if (isAstc(format))
        return kAstcBlocks[astcIndex(format)][1];
    return format >= PixelFormat::BC1 ? 4 : 1;
}

int elementColumns(PixelFormat format, int width)
{
    return (width + elementWidth(format) - 1) / elementWidth(format);
}

int elementRows(PixelFormat format, int height)
{
    return (height + elementHeight(format) - 1) / elementHeight(format);
}

std::optional<PixelFormat> astcFormat(int blockWidth, int blockHeight, bool srgb)
{
    for (int i = 0; i < kAstcCount; ++i)
        if (kAstcBlocks[i][0] == blockWidth && kAstcBlocks[i][1] == blockHeight)
            return PixelFormat(int(srgb ? PixelFormat::ASTC_4x4_SRGB : PixelFormat::ASTC_4x4) + i);
    return std::nullopt;
}

int bytesPerElement(PixelFormat format)
{
    switch (format) {
    case PixelFormat::R8: return 1;
    case PixelFormat::RG8:
    case PixelFormat::RGB565:
    case PixelFormat::RGBA4:
    case PixelFormat::RGB5A1: return 2;
    case PixelFormat::BC1:
    case PixelFormat::BC4:
    case PixelFormat::BC4S: return 8;
    case PixelFormat::BC2:
    case PixelFormat::BC3:
    case PixelFormat::BC5:
    case PixelFormat::BC5S:
    case PixelFormat::BC7: return 16;
    default: return isAstc(format) ? 16 : 4;
    }
}

std::optional<QByteArray> decodePixels(PixelFormat format, QByteArrayView data, int width, int height)
{
    if (width <= 0 || height <= 0)
        return std::nullopt;
    const int bpe = bytesPerElement(format);
    const int columns = elementColumns(format, width), rows = elementRows(format, height);
    if (data.size() < qsizetype(columns) * rows * bpe)
        return std::nullopt;
    if (isAstc(format))
        return decodeAstc(data.first(qsizetype(columns) * rows * bpe), width, height, elementWidth(format),
                          elementHeight(format), isSrgb(format));
    const int unit = elementWidth(format);

    QByteArray pixels(qsizetype(width) * height * 4, Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(pixels.data());
    const auto *in = reinterpret_cast<const uchar *>(data.data());

    if (unit == 1) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                setPixel(out, width, x, y, decodeUncompressed(format, in + (qsizetype(y) * width + x) * bpe));
        return pixels;
    }

    Rgba block[16];
    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < columns; ++bx) {
            decodeBlock(format, in + (qsizetype(by) * columns + bx) * bpe, block);
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < width && y < height)
                    setPixel(out, width, x, y, block[i]);
            }
        }
    }
    return pixels;
}

qsizetype blockLinearSize(int widthInElements, int heightInElements, int bytesPerElement, int blockHeight)
{
    const qsizetype widthInGobs = (qsizetype(widthInElements) * bytesPerElement + 63) / 64;
    const qsizetype rowsOfBlocks = (heightInElements + 8 * blockHeight - 1) / (8 * blockHeight);
    return widthInGobs * rowsOfBlocks * 512 * blockHeight;
}

int defaultBlockHeight(int heightInElements)
{
    return std::clamp(pow2AtLeast((heightInElements + 7) / 8), 1, 16);
}

int mipBlockHeight(int baseBlockHeight, int heightInElements)
{
    int blockHeight = std::max(1, baseBlockHeight);
    const int heightInGobs = (heightInElements + 7) / 8;
    while (blockHeight > 1 && heightInGobs <= blockHeight / 2)
        blockHeight /= 2;
    return blockHeight;
}

namespace {

bool validBlockLinear(int widthInElements, int heightInElements, int bytesPerElement, int blockHeight)
{
    return widthInElements > 0 && heightInElements > 0 && bytesPerElement > 0 && bytesPerElement <= 16
        && blockHeight >= 1 && blockHeight <= 32 && (blockHeight & (blockHeight - 1)) == 0;
}

template <typename Copy>
void forEachBlockLinearElement(int widthInElements, int heightInElements, int bytesPerElement, int blockHeight,
                               Copy &&copy)
{
    const qsizetype rowBytes = qsizetype(widthInElements) * bytesPerElement;
    const qsizetype widthInGobs = (rowBytes + 63) / 64;
    for (int y = 0; y < heightInElements; ++y) {
        const qsizetype rowBase = qsizetype(y / (8 * blockHeight)) * 512 * blockHeight * widthInGobs
                                + qsizetype((y % (8 * blockHeight)) / 8) * 512
                                + ((y % 8) / 2) * 64 + (y % 2) * 16;
        for (int x = 0; x < widthInElements; ++x) {
            const qsizetype xb = qsizetype(x) * bytesPerElement;
            const qsizetype address = rowBase + (xb / 64) * 512 * blockHeight
                                    + ((xb % 64) / 32) * 256 + ((xb % 32) / 16) * 32 + (xb % 16);
            copy(y * rowBytes + xb, address);
        }
    }
}

}

std::optional<QByteArray> deswizzleBlockLinear(QByteArrayView data, int widthInElements, int heightInElements,
                                               int bytesPerElement, int blockHeight)
{
    if (!validBlockLinear(widthInElements, heightInElements, bytesPerElement, blockHeight)
        || data.size() < blockLinearSize(widthInElements, heightInElements, bytesPerElement, blockHeight))
        return std::nullopt;
    QByteArray out(qsizetype(widthInElements) * bytesPerElement * heightInElements, Qt::Uninitialized);
    forEachBlockLinearElement(widthInElements, heightInElements, bytesPerElement, blockHeight,
                              [&](qsizetype linear, qsizetype swizzled) {
                                  std::memcpy(out.data() + linear, data.data() + swizzled, size_t(bytesPerElement));
                              });
    return out;
}

std::optional<QByteArray> swizzleBlockLinear(QByteArrayView linear, int widthInElements, int heightInElements,
                                             int bytesPerElement, int blockHeight)
{
    if (!validBlockLinear(widthInElements, heightInElements, bytesPerElement, blockHeight)
        || linear.size() < qsizetype(widthInElements) * bytesPerElement * heightInElements)
        return std::nullopt;
    QByteArray out(blockLinearSize(widthInElements, heightInElements, bytesPerElement, blockHeight), '\0');
    forEachBlockLinearElement(widthInElements, heightInElements, bytesPerElement, blockHeight,
                              [&](qsizetype from, qsizetype swizzled) {
                                  std::memcpy(out.data() + swizzled, linear.data() + from, size_t(bytesPerElement));
                              });
    return out;
}

namespace {

quint8 maskedChannel(quint32 pixel, quint32 mask)
{
    if (mask == 0)
        return 0;
    const int shift = std::countr_zero(mask);
    const quint64 max = mask >> shift;
    return quint8(quint64((pixel & mask) >> shift) * 255u / max);
}

}

std::optional<QByteArray> decodeMaskedPixels(QByteArrayView data, int width, int height, const PixelMasks &m)
{
    if (width <= 0 || height <= 0 || (m.bits != 8 && m.bits != 16 && m.bits != 24 && m.bits != 32))
        return std::nullopt;
    const int bytes = m.bits / 8;
    const qsizetype count = qsizetype(width) * height;
    if (data.size() < count * bytes)
        return std::nullopt;
    QByteArray out(count * 4, Qt::Uninitialized);
    auto *dst = reinterpret_cast<uchar *>(out.data());
    const auto *src = reinterpret_cast<const uchar *>(data.data());
    for (qsizetype i = 0; i < count; ++i) {
        quint32 pixel = 0;
        for (int k = 0; k < bytes; ++k)
            pixel |= quint32(src[i * bytes + k]) << (8 * k);
        const quint8 r = maskedChannel(pixel, m.r);
        dst[i * 4] = r;
        dst[i * 4 + 1] = m.luminance ? r : maskedChannel(pixel, m.g);
        dst[i * 4 + 2] = m.luminance ? r : maskedChannel(pixel, m.b);
        dst[i * 4 + 3] = m.a ? maskedChannel(pixel, m.a) : 255;
    }
    return out;
}

void swapGobSectors(QByteArray &data)
{
    for (qsizetype group = 0; group + 128 <= data.size(); group += 128)
        std::swap_ranges(data.begin() + group + 32, data.begin() + group + 64, data.begin() + group + 64);
}
