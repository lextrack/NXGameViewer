#pragma once

#include "core/format.h"
#include "textures/texturedecode.h"

#include <QByteArray>
#include <QString>

#include <optional>

struct EncodedPixels
{
    QByteArray linear;
    qsizetype reusedBlocks = 0;
    qsizetype encodedBlocks = 0;
};

bool canEncodePixels(PixelFormat format);
std::optional<EncodedPixels> encodePixels(PixelFormat format, const Image &image, QByteArrayView originalLinear,
                                          QString *error);
