#include "textures/xtxwriter.h"

#include "textures/textureencode.h"
#include "textures/texturedecode.h"
#include "textures/texturemips.h"
#include "textures/xtxreader.h"

#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr qsizetype kBlockHeaderSize = 0x24;
constexpr qsizetype kTextureInfoSize = 0x78;

quint32 u32(const QByteArray &d, qsizetype pos) { return qFromLittleEndian<quint32>(d.constData() + pos); }
quint64 u64(const QByteArray &d, qsizetype pos) { return qFromLittleEndian<quint64>(d.constData() + pos); }

}

std::optional<XtxReplacement> replaceXtxImage(const QByteArray &xtx, const Image &image, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<XtxReplacement> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (xtx.size() < 0x10 || !xtx.startsWith("DFvN"))
        return fail(QObject::tr("Not an XTX texture (no \"DFvN\" at the start)."));

    qsizetype infoStart = -1, dataStart = -1, dataSize = 0;
    int textures = 0;
    qsizetype pos = u32(xtx, 4);
    for (int guard = 0; pos >= 0x10 && pos + kBlockHeaderSize <= xtx.size() && guard < 100000; ++guard) {
        if (xtx.mid(pos, 4) != "HBvN")
            return fail(QObject::tr("Broken block at 0x%1.").arg(pos, 0, 16));
        const quint64 size = u64(xtx, pos + 8), offset = u64(xtx, pos + 16);
        const quint32 type = u32(xtx, pos + 24);
        if (offset < quint64(kBlockHeaderSize) || offset > quint64(xtx.size() - pos) || size > quint64(xtx.size() - pos) - offset)
            return fail(QObject::tr("Block at 0x%1 goes past the end of the file.").arg(pos, 0, 16));
        const qsizetype start = pos + qsizetype(offset);
        if (type == 2) {
            if (size < quint64(kTextureInfoSize))
                return fail(QObject::tr("Texture info at 0x%1 is too short.").arg(start, 0, 16));
            ++textures;
            infoStart = start;
        } else if (type == 3 && infoStart >= 0 && dataStart < 0) {
            dataStart = start;
            dataSize = qsizetype(size);
        }
        pos = start + qsizetype(size);
    }
    if (textures != 1 || dataStart < 0)
        return fail(textures > 1 ? QObject::tr("Files with several textures cannot be replaced yet.")
                                 : QObject::tr("The file has no texture."));

    const quint32 width = u32(xtx, infoStart + 0x0C), height = u32(xtx, infoStart + 0x10);
    const quint32 target = u32(xtx, infoStart + 0x18), nvn = u32(xtx, infoStart + 0x1C);
    const quint32 mips = u32(xtx, infoStart + 0x20), sliceSize = u32(xtx, infoStart + 0x24);
    const quint32 blockHeightLog2 = u32(xtx, infoStart + 0x6C);
    const quint64 textureSize = u64(xtx, infoStart);
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
        offsets.push_back(qsizetype(u32(xtx, infoStart + 0x28 + qsizetype(m) * 4)));
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
