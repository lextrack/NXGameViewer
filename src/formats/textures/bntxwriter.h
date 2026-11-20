#pragma once

#include "core/format.h"
#include "textures/xtxwriter.h"

#include <QByteArray>
#include <QString>

#include <optional>

using TextureReplacement = XtxReplacement;

std::optional<TextureReplacement> replaceBntxImage(const QByteArray &file, qsizetype bntxOffset, int textureIndex,
                                                   const Image &image, QString *error);
std::optional<TextureReplacement> replaceTextureImage(const QByteArray &file, const TextureSlot &slot,
                                                      const Image &image, QString *error);
