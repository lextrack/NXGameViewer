#pragma once

#include "core/format.h"
#include "unity/serializedfile.h"

#include <optional>

QString unityTextureFormatName(int format);
int switchGobsPerBlock(const UValue &texture, qint32 platform);
std::optional<Image> decodeUnityTexture(const UValue &texture, QByteArrayView imageData, qint32 platform,
                                        QString *error);
Image cropImage(const Image &image, int x, int y, int width, int height);
