#include "textures/texturemips.h"

#include "textures/textureencode.h"

#include <QObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

const std::array<float, 256> &srgbToLinear()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

uchar linearToSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    const float c = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return uchar(std::lround(c * 255.0f));
}

struct Footprint
{
    int x[2];
    int y[2];
};

Footprint footprint(int x, int y, int sourceWidth, int sourceHeight)
{
    return {{std::min(2 * x, sourceWidth - 1), std::min(2 * x + 1, sourceWidth - 1)},
            {std::min(2 * y, sourceHeight - 1), std::min(2 * y + 1, sourceHeight - 1)}};
}

void averageInto(const Image &source, const Footprint &f, bool srgb, uchar *out)
{
    const auto *p = reinterpret_cast<const uchar *>(source.rgba.constData());
    const auto &lin = srgbToLinear();
    for (int c = 0; c < 4; ++c) {
        const bool gamma = srgb && c < 3;
        float sum = 0;
        int whole = 0;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const uchar v = p[(qsizetype(f.y[j]) * source.width + f.x[i]) * 4 + c];
                if (gamma)
                    sum += lin[v];
                else
                    whole += v;
            }
        out[c] = gamma ? linearToSrgb(sum / 4.0f) : uchar((whole + 2) / 4);
    }
}

}

std::optional<std::vector<MipLevel>> mipLayout(PixelFormat format, int width, int height, int baseBlockHeight,
                                               const std::vector<qsizetype> &offsets, qsizetype available,
                                               QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<MipLevel>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (width <= 0 || height <= 0 || offsets.empty())
        return fail(QObject::tr("The texture has no pixels."));
    const int bpe = bytesPerElement(format);
    std::vector<MipLevel> levels;
    for (size_t n = 0; n < offsets.size(); ++n) {
        MipLevel level;
        level.width = std::max(1, width >> n);
        level.height = std::max(1, height >> n);
        level.columns = elementColumns(format, level.width);
        level.rows = elementRows(format, level.height);
        level.blockHeight = n == 0 ? std::max(1, baseBlockHeight) : mipBlockHeight(baseBlockHeight, level.rows);
        level.offset = offsets[n];
        level.size = blockLinearSize(level.columns, level.rows, bpe, level.blockHeight);
        if (level.offset < 0 || level.offset > available - level.size)
            return fail(QObject::tr("Mip level %1 lies outside the texture's pixel data.").arg(n));
        if (!levels.empty() && level.offset < levels.back().offset + levels.back().size)
            return fail(QObject::tr("Mip level %1 overlaps the level before it.").arg(n));
        levels.push_back(level);
    }
    return levels;
}

static std::optional<QByteArray> deswizzleLevel(const MipLevel &level, QByteArrayView data, int bpe)
{
    if (!level.swappedSectors)
        return deswizzleBlockLinear(data.sliced(level.offset, level.size), level.columns, level.rows, bpe, level.blockHeight);
    QByteArray swizzled = data.sliced(level.offset, level.size).toByteArray();
    swapGobSectors(swizzled);
    return deswizzleBlockLinear(swizzled, level.columns, level.rows, bpe, level.blockHeight);
}

Image downsampleBox(const Image &source, int width, int height, bool srgb)
{
    Image out{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    auto *o = reinterpret_cast<uchar *>(out.rgba.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            averageInto(source, footprint(x, y, source.width, source.height), srgb, o + (qsizetype(y) * width + x) * 4);
    return out;
}

std::optional<std::vector<Image>> decodeMips(PixelFormat format, const std::vector<MipLevel> &levels, QByteArrayView data)
{
    std::vector<Image> images;
    for (const MipLevel &level : levels) {
        if (level.offset < 0 || level.offset > data.size() - level.size)
            return std::nullopt;
        const std::optional<QByteArray> linear = deswizzleLevel(level, data, bytesPerElement(format));
        if (!linear)
            return std::nullopt;
        std::optional<QByteArray> rgba = decodePixels(format, *linear, level.width, level.height);
        if (!rgba)
            return std::nullopt;
        images.push_back({level.width, level.height, std::move(*rgba)});
    }
    return images;
}

std::vector<Image> rebuildMips(const Image &top, const std::vector<Image> &original, const std::vector<MipLevel> &levels,
                               bool srgb)
{
    std::vector<Image> out{top};
    auto matches = [&](size_t n, const Image &image) {
        return n < original.size() && original[n].width == image.width && original[n].height == image.height
            && original[n].rgba.size() == image.rgba.size();
    };
    std::vector<char> dirty(size_t(top.width) * size_t(top.height), 1);
    if (matches(0, top)) {
        const auto *a = reinterpret_cast<const quint32 *>(top.rgba.constData());
        const auto *b = reinterpret_cast<const quint32 *>(original[0].rgba.constData());
        for (size_t i = 0; i < dirty.size(); ++i)
            dirty[i] = a[i] != b[i];
    }
    for (size_t n = 1; n < levels.size(); ++n) {
        const Image &previous = out.back();
        const int width = levels[n].width, height = levels[n].height;
        Image level{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
        std::vector<char> levelDirty(size_t(width) * size_t(height), 1);
        const bool keep = n < original.size() && original[n].width == width && original[n].height == height;
        auto *o = reinterpret_cast<uchar *>(level.rgba.data());
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const Footprint f = footprint(x, y, previous.width, previous.height);
                bool changed = false;
                for (int j = 0; j < 2 && !changed; ++j)
                    for (int i = 0; i < 2 && !changed; ++i)
                        changed = dirty[size_t(f.y[j]) * size_t(previous.width) + size_t(f.x[i])];
                const size_t at = size_t(y) * size_t(width) + size_t(x);
                uchar *px = o + at * 4;
                if (changed || !keep) {
                    averageInto(previous, f, srgb, px);
                    levelDirty[at] = 1;
                } else {
                    std::memcpy(px, original[n].rgba.constData() + at * 4, 4);
                    levelDirty[at] = 0;
                }
            }
        out.push_back(std::move(level));
        dirty = std::move(levelDirty);
    }
    return out;
}

std::optional<EncodedMips> encodeMips(PixelFormat format, const std::vector<MipLevel> &levels, QByteArrayView data,
                                      const StoredImageMaker &makeTop, bool srgb, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<EncodedMips> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const std::optional<std::vector<Image>> original = decodeMips(format, levels, data);
    if (!original)
        return fail(QObject::tr("The texture's pixel data could not be read."));
    const std::optional<Image> top = makeTop(original->front());
    if (!top)
        return std::nullopt;
    const std::vector<Image> images = rebuildMips(*top, *original, levels, srgb);

    EncodedMips result;
    const int bpe = bytesPerElement(format);
    for (size_t n = 0; n < levels.size(); ++n) {
        const MipLevel &level = levels[n];
        const std::optional<QByteArray> linear = deswizzleLevel(level, data, bpe);
        const std::optional<EncodedPixels> encoded =
            encodePixels(format, images[n], linear ? QByteArrayView(*linear) : QByteArrayView(), error);
        if (!encoded)
            return std::nullopt;
        std::optional<QByteArray> swizzled = swizzleBlockLinear(encoded->linear, level.columns, level.rows, bpe, level.blockHeight);
        if (!swizzled || swizzled->size() != level.size)
            return fail(QObject::tr("Mip level %1 could not be laid out for the GPU.").arg(n));
        if (level.swappedSectors)
            swapGobSectors(*swizzled);
        result.levels.push_back(std::move(*swizzled));
        result.encodedPerLevel.push_back(encoded->encodedBlocks);
        result.reusedBlocks += encoded->reusedBlocks;
        result.encodedBlocks += encoded->encodedBlocks;
    }
    return result;
}
