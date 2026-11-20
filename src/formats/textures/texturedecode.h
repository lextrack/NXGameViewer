#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

constexpr int kMaxTextureSide = 16384;

enum class PixelFormat {
    RGBA8,    // 4 bytes per pixel
    BGRA8,
    R8,       // greyscale
    RG8,
    RGB565,
    RGBA4,
    RGB5A1,
    RGB10A2,
    BC1,      // 4×4 blocks of 8 bytes (DXT1)
    BC2,      // 16 bytes (DXT3)
    BC3,      // 16 bytes (DXT5)
    BC4,      // 8 bytes, one channel, shown as grey
    BC4S,
    BC5,      // 16 bytes, two channels (usually normal maps), shown as red/green
    BC5S,
    BC7,      // 16 bytes
    ASTC_4x4,
    ASTC_5x4,
    ASTC_5x5,
    ASTC_6x5,
    ASTC_6x6,
    ASTC_8x5,
    ASTC_8x6,
    ASTC_8x8,
    ASTC_10x5,
    ASTC_10x6,
    ASTC_10x8,
    ASTC_10x10,
    ASTC_12x10,
    ASTC_12x12,
    ASTC_4x4_SRGB,
    ASTC_5x4_SRGB,
    ASTC_5x5_SRGB,
    ASTC_6x5_SRGB,
    ASTC_6x6_SRGB,
    ASTC_8x5_SRGB,
    ASTC_8x6_SRGB,
    ASTC_8x8_SRGB,
    ASTC_10x5_SRGB,
    ASTC_10x6_SRGB,
    ASTC_10x8_SRGB,
    ASTC_10x10_SRGB,
    ASTC_12x10_SRGB,
    ASTC_12x12_SRGB,
};

QString pixelFormatName(PixelFormat format);

int bytesPerElement(PixelFormat format);
int elementWidth(PixelFormat format);
int elementHeight(PixelFormat format);
int elementColumns(PixelFormat format, int width);
int elementRows(PixelFormat format, int height);
bool isAstc(PixelFormat format);
bool isSrgb(PixelFormat format);
std::optional<PixelFormat> astcFormat(int blockWidth, int blockHeight, bool srgb);

std::optional<QByteArray> decodePixels(PixelFormat format, QByteArrayView data, int width, int height);

struct PixelMasks
{
    int bits = 0;
    quint32 r = 0, g = 0, b = 0, a = 0;
    bool luminance = false;
};

std::optional<QByteArray> decodeMaskedPixels(QByteArrayView data, int width, int height, const PixelMasks &masks);

std::optional<QByteArray> deswizzleBlockLinear(QByteArrayView data, int widthInElements, int heightInElements,
                                               int bytesPerElement, int blockHeight);

std::optional<QByteArray> swizzleBlockLinear(QByteArrayView linear, int widthInElements, int heightInElements,
                                             int bytesPerElement, int blockHeight);

void swapGobSectors(QByteArray &data);

qsizetype blockLinearSize(int widthInElements, int heightInElements, int bytesPerElement, int blockHeight);

int defaultBlockHeight(int heightInElements);

int mipBlockHeight(int baseBlockHeight, int heightInElements);
