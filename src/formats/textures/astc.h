#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

std::optional<QByteArray> decodeAstc(QByteArrayView blocks, int width, int height, int blockWidth, int blockHeight,
                                     bool srgb);
std::optional<QByteArray> encodeAstc(const QByteArray &rgba, int width, int height, int blockWidth, int blockHeight,
                                     bool srgb, QString *error);
