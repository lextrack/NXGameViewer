#include "textures/xtxwriter.h"

#include "core/bytes.h"
#include "textures/textureencode.h"
#include "textures/texturedecode.h"
#include "textures/texturemips.h"
#include "textures/xtxreader.h"

#include <QObject>

#include <algorithm>
#include <cstring>
#include <new>

std::optional<XtxReplacement> replaceXtxImage(const QByteArray &xtx, const Image &image, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<XtxReplacement> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const std::optional<XtxLayout> layout = parseXtxLayout(xtx, error);
    if (!layout)
        return std::nullopt;
    if (layout->infoBlocks != 1 || layout->textures.empty())
        return fail(layout->infoBlocks > 1 ? QObject::tr("Files with several textures cannot be replaced yet.")
                                           : QObject::tr("The file has no texture."));
    const XtxTexture &texture = layout->textures.front();
    const XtxTextureInfo &info = texture.info;
    const qsizetype infoStart = info.at, dataStart = texture.start, dataSize = texture.size;
    const quint32 width = info.width, height = info.height, target = info.target, nvn = info.format;
    const quint32 mips = info.mipCount, sliceSize = info.sliceSize, blockHeightLog2 = info.blockHeightLog2;
    const quint64 textureSize = info.dataSize;
    if (int(width) != image.width || int(height) != image.height)
        return fail(QObject::tr("The picture is %1 × %2, but the texture is %3 × %4. Use a picture of the same size.")
                        .arg(image.width).arg(image.height).arg(width).arg(height));
    if (target != 1 || (sliceSize > 0 && textureSize / sliceSize > 1))
        return fail(QObject::tr("Only 2D textures with one slice can be replaced yet (not arrays or cube maps)."));
    if (mips < 1 || mips > 17)
        return fail(QObject::tr("The texture says it has %1 mip levels.").arg(mips));
    const std::optional<PixelFormat> format = xtxPixelFormat(nvn);
    if (!format)
        return fail(QObject::tr("Pixel format 0x%1 is not supported.").arg(nvn, 0, 16));
    if (blockHeightLog2 > 5)
        return fail(QObject::tr("The texture's block height is not valid."));

    std::vector<qsizetype> offsets;
    for (quint32 m = 0; m < mips; ++m)
        offsets.push_back(qsizetype(le32(xtx, infoStart + 0x28 + qsizetype(m) * 4)));
    const qsizetype available = sliceSize > 0 ? std::min<qsizetype>(qsizetype(sliceSize), dataSize) : dataSize;
    const std::optional<std::vector<MipLevel>> levels =
        mipLayout(*format, int(width), int(height), 1 << blockHeightLog2, offsets, available, error);
    if (!levels)
        return std::nullopt;

    try {
        const std::optional<EncodedMips> encoded = encodeMips(
            *format, *levels, QByteArrayView(xtx).sliced(dataStart, available),
            [&image](const Image &) { return std::optional<Image>(image); }, nvn == 0x38, error);
        if (!encoded)
            return std::nullopt;

        XtxReplacement result;
        result.file = xtx;
        for (size_t n = 0; n < levels->size(); ++n)
            std::memcpy(result.file.data() + dataStart + (*levels)[n].offset, encoded->levels[n].constData(),
                        size_t(encoded->levels[n].size()));
        result.formatName = pixelFormatName(*format);
        result.reusedBlocks = encoded->reusedBlocks;
        result.encodedBlocks = encoded->encodedBlocks;
        result.mipLevels = int(levels->size());
        return result;
    } catch (const std::bad_alloc &) {
        return fail(QObject::tr("Not enough memory to replace this texture."));
    }
}
