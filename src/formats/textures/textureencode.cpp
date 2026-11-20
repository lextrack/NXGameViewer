#include "textures/textureencode.h"

#include <QHash>
#include <QObject>

#include "textures/astc.h"

#include <bc7enc.h>
#define RGBCX_IMPLEMENTATION
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace {

void initEncoders()
{
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

QByteArray blockKey(const Image &image, int bx, int by, int bw, int bh)
{
    QByteArray out(qsizetype(bw) * bh * 4, Qt::Uninitialized);
    const auto *rgba = reinterpret_cast<const uchar *>(image.rgba.constData());
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const int sx = std::min(bx * bw + x, image.width - 1), sy = std::min(by * bh + y, image.height - 1);
            std::memcpy(out.data() + (y * bw + x) * 4, rgba + (qsizetype(sy) * image.width + sx) * 4, 4);
        }
    return out;
}

bool encodeBlock(PixelFormat format, const std::array<uchar, 64> &pixels, uchar *out)
{
    switch (format) {
    case PixelFormat::BC7: {
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        params.m_uber_level = 2;
        bc7enc_compress_block(out, pixels.data(), &params);
        return true;
    }
    case PixelFormat::BC1: rgbcx::encode_bc1(10, out, pixels.data(), true, false); return true;
    case PixelFormat::BC3: rgbcx::encode_bc3(10, out, pixels.data()); return true;
    case PixelFormat::BC4: rgbcx::encode_bc4(out, pixels.data(), 4); return true;
    case PixelFormat::BC5: rgbcx::encode_bc5(out, pixels.data(), 0, 1, 4); return true;
    default: return false;
    }
}

bool encodePixel(PixelFormat format, const uchar *rgba, uchar *out)
{
    switch (format) {
    case PixelFormat::RGBA8: std::memcpy(out, rgba, 4); return true;
    case PixelFormat::BGRA8: out[0] = rgba[2]; out[1] = rgba[1]; out[2] = rgba[0]; out[3] = rgba[3]; return true;
    case PixelFormat::R8: out[0] = rgba[0]; return true;
    case PixelFormat::RG8: out[0] = rgba[0]; out[1] = rgba[1]; return true;
    case PixelFormat::RGB565: {
        const auto q = [](uchar v, int bits) { return quint16((v * ((1 << bits) - 1) + 127) / 255); };
        const quint16 v = quint16(q(rgba[0], 5) << 11 | q(rgba[1], 6) << 5 | q(rgba[2], 5));
        out[0] = uchar(v & 0xFF);
        out[1] = uchar(v >> 8);
        return true;
    }
    default: return false;
    }
}

}

bool canEncodePixels(PixelFormat format)
{
    switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
    case PixelFormat::R8:
    case PixelFormat::RG8:
    case PixelFormat::RGB565:
    case PixelFormat::BC1:
    case PixelFormat::BC3:
    case PixelFormat::BC4:
    case PixelFormat::BC5:
    case PixelFormat::BC7:
        return true;
    default:
        return isAstc(format);
    }
}

std::optional<EncodedPixels> encodePixels(PixelFormat format, const Image &image, QByteArrayView originalLinear,
                                          QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<EncodedPixels> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() < qsizetype(image.width) * image.height * 4)
        return fail(QObject::tr("The picture is empty."));
    const int bw = elementWidth(format), bh = elementHeight(format), bpe = bytesPerElement(format);
    const int columns = elementColumns(format, image.width), rows = elementRows(format, image.height);

    EncodedPixels result;
    result.linear = QByteArray(qsizetype(columns) * rows * bpe, '\0');
    auto *out = reinterpret_cast<uchar *>(result.linear.data());

    if (bw == 1 && bh == 1) {
        if (!canEncodePixels(format))
            return fail(QObject::tr("Saving in %1 is not supported yet.").arg(pixelFormatName(format)));
        const auto *rgba = reinterpret_cast<const uchar *>(image.rgba.constData());
        for (qsizetype i = 0; i < qsizetype(columns) * rows; ++i)
            encodePixel(format, rgba + i * 4, out + i * bpe);
        result.encodedBlocks = qsizetype(columns) * rows;
        return result;
    }
    if (!canEncodePixels(format))
        return fail(QObject::tr("Saving changed pixels in %1 is not supported yet.").arg(pixelFormatName(format)));

    QHash<QByteArray, QByteArray> known;
    std::optional<Image> original;
    if (originalLinear.size() >= result.linear.size()) {
        if (const auto decoded = decodePixels(format, originalLinear, image.width, image.height)) {
            original = Image{image.width, image.height, *decoded};
            for (int by = 0; by < rows; ++by)
                for (int bx = 0; bx < columns; ++bx) {
                    QByteArray key = blockKey(*original, bx, by, bw, bh);
                    if (!known.contains(key))
                        known.insert(std::move(key), QByteArray(originalLinear.data() + (qsizetype(by) * columns + bx) * bpe, bpe));
                }
        }
    }

    struct Pending
    {
        qsizetype index;
        QByteArray pixels;
    };
    std::vector<Pending> pending;
    if (!isAstc(format))
        initEncoders();
    for (int by = 0; by < rows; ++by)
        for (int bx = 0; bx < columns; ++bx) {
            const qsizetype index = qsizetype(by) * columns + bx;
            uchar *dst = out + index * bpe;
            QByteArray pixels = blockKey(image, bx, by, bw, bh);
            if (original && blockKey(*original, bx, by, bw, bh) == pixels) {
                std::memcpy(dst, originalLinear.data() + index * bpe, size_t(bpe));
                ++result.reusedBlocks;
                continue;
            }
            const auto found = known.constFind(pixels);
            if (found != known.constEnd()) {
                std::memcpy(dst, found->constData(), size_t(bpe));
                ++result.reusedBlocks;
            } else if (isAstc(format)) {
                pending.push_back({index, std::move(pixels)});
            } else {
                std::array<uchar, 64> block;
                std::memcpy(block.data(), pixels.constData(), 64);
                if (!encodeBlock(format, block, dst))
                    return fail(QObject::tr("Saving changed pixels in %1 is not supported yet.").arg(pixelFormatName(format)));
                ++result.encodedBlocks;
            }
        }

    if (!pending.empty()) {
        const int perRow = int(std::min<size_t>(pending.size(), 64));
        const int gridRows = int((pending.size() + size_t(perRow) - 1) / size_t(perRow));
        const int width = perRow * bw, height = gridRows * bh;
        QByteArray grid(qsizetype(width) * height * 4, '\0');
        for (size_t i = 0; i < pending.size(); ++i) {
            const int gx = int(i) % perRow, gy = int(i) / perRow;
            for (int y = 0; y < bh; ++y)
                std::memcpy(grid.data() + ((qsizetype(gy) * bh + y) * width + qsizetype(gx) * bw) * 4,
                            pending[i].pixels.constData() + qsizetype(y) * bw * 4, size_t(bw) * 4);
        }
        const std::optional<QByteArray> blocks = encodeAstc(grid, width, height, bw, bh, isSrgb(format), error);
        if (!blocks)
            return std::nullopt;
        for (size_t i = 0; i < pending.size(); ++i)
            std::memcpy(out + pending[i].index * bpe, blocks->constData() + qsizetype(i) * 16, 16);
        result.encodedBlocks += qsizetype(pending.size());
    }
    return result;
}
