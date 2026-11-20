#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

std::optional<QByteArray> lz4BlockDecompress(QByteArrayView source, qsizetype decompressedSize);
