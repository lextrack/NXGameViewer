#include "textures/bntxwriter.h"

#include "textures/bntxreader.h"
#include "textures/textureencode.h"
#include "textures/texturedecode.h"
#include "textures/texturemips.h"

#include <QObject>

#include <cstring>
#include <new>

std::optional<TextureReplacement> replaceBntxImage(const QByteArray &file, qsizetype bntxOffset, int textureIndex,
                                                   const Image &image, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<TextureReplacement> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const std::optional<BntxFile> bntx = parseBntx(file, bntxOffset, error);
    if (!bntx)
        return std::nullopt;
    if (textureIndex < 0 || size_t(textureIndex) >= bntx->textures.size())
        return fail(QObject::tr("The BNTX file has no texture %1.").arg(textureIndex + 1));
    const BntxTexture &t = bntx->textures[size_t(textureIndex)];

    if (t.width != image.width || t.height != image.height)
        return fail(QObject::tr("The picture is %1 × %2, but %3 is %4 × %5. Use a picture of the same size.")
                        .arg(image.width).arg(image.height).arg(t.name).arg(t.width).arg(t.height));
    if (t.dimension != 2 || t.arrayLength != 1 || t.depth != 1)
        return fail(QObject::tr("Only plain 2D textures can be replaced yet (not arrays, cube maps or 3D textures)."));
    if (t.tileMode != 0)
        return fail(QObject::tr("Linear (not block-linear) textures cannot be replaced yet."));
    const std::optional<PixelFormat> format = bntxPixelFormat(t.format);
    if (!format || !canEncodePixels(*format))
        return fail(QObject::tr("Saving in %1 is not supported yet.").arg(bntxFormatName(t.format)));

    const qsizetype start = bntxOffset + t.mipOffsets[0];
    std::vector<qsizetype> offsets;
    for (qsizetype at : t.mipOffsets)
        offsets.push_back(at - t.mipOffsets[0]);
    std::optional<std::vector<MipLevel>> levels =
        mipLayout(*format, t.width, t.height, 1 << t.blockHeightLog2, offsets, t.imageSize, error);
    if (!levels)
        return std::nullopt;
    for (MipLevel &level : *levels)
        level.swappedSectors = t.swappedGobSectors;

    try {
        const quint32 channels = t.channels;
        const std::optional<EncodedMips> encoded = encodeMips(
            *format, *levels, QByteArrayView(file).sliced(start, t.imageSize),
            [&image, channels](const Image &stored) {
                return std::optional<Image>(Image{image.width, image.height, unapplyChannelSelectors(image.rgba, stored.rgba, channels)});
            },
            (t.format & 0xFF) == 0x06, error);
        if (!encoded)
            return std::nullopt;

        TextureReplacement result;
        result.file = file;
        for (size_t n = 0; n < levels->size(); ++n)
            std::memcpy(result.file.data() + start + (*levels)[n].offset, encoded->levels[n].constData(),
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

std::optional<TextureReplacement> replaceTextureImage(const QByteArray &file, const TextureSlot &slot,
                                                      const Image &image, QString *error)
{
    if (slot.container >= 0 && slot.container <= file.size() - 4 && file.mid(slot.container, 4) == "BNTX")
        return replaceBntxImage(file, slot.container, slot.index, image, error);
    if (slot.container == 0 && file.startsWith("DFvN"))
        return replaceXtxImage(file, image, error);
    if (error)
        *error = QObject::tr("No texture this app can replace was found here.");
    return std::nullopt;
}
