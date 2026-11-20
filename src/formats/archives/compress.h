#pragma once

#include "archives/decompress.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

struct CompressionLayer
{
    Compression kind = Compression::None;
    bool zstdChecksum = false;
    unsigned zstdDictionary = 0;
    quint32 yaz0Alignment = 0;
};

constexpr int kZstdLevel = 12;

std::optional<CompressionLayer> describeCompression(const QByteArray &data, QString *error);
std::optional<QByteArray> compressZstd(const QByteArray &data, int level, bool checksum, QString *error,
                                       unsigned dictionary = 0);
QByteArray compressYaz0(const QByteArray &data, quint32 alignment = 0);
std::optional<QByteArray> compressLike(const QByteArray &data, const CompressionLayer &layer, QString *error);
std::optional<QByteArray> unpackAll(const QByteArray &data, std::vector<CompressionLayer> *layers, QString *error);
std::optional<QByteArray> packAll(const QByteArray &data, const std::vector<CompressionLayer> &layers, QString *error);
