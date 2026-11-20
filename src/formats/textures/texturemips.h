#pragma once

#include "core/format.h"
#include "textures/texturedecode.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

struct MipLevel
{
    int width = 0;
    int height = 0;
    int columns = 0;
    int rows = 0;
    int blockHeight = 1;
    qsizetype offset = 0;
    qsizetype size = 0;
    bool swappedSectors = false;
};

struct EncodedMips
{
    std::vector<QByteArray> levels;
    std::vector<qsizetype> encodedPerLevel;
    qsizetype reusedBlocks = 0;
    qsizetype encodedBlocks = 0;
};

using StoredImageMaker = std::function<std::optional<Image>(const Image &originalTop)>;

std::optional<std::vector<MipLevel>> mipLayout(PixelFormat format, int width, int height, int baseBlockHeight,
                                               const std::vector<qsizetype> &offsets, qsizetype available,
                                               QString *error);
Image downsampleBox(const Image &source, int width, int height, bool srgb);
std::optional<std::vector<Image>> decodeMips(PixelFormat format, const std::vector<MipLevel> &levels,
                                             QByteArrayView data);
std::vector<Image> rebuildMips(const Image &top, const std::vector<Image> &original, const std::vector<MipLevel> &levels,
                               bool srgb);
std::optional<EncodedMips> encodeMips(PixelFormat format, const std::vector<MipLevel> &levels, QByteArrayView data,
                                      const StoredImageMaker &makeTop, bool srgb, QString *error);
