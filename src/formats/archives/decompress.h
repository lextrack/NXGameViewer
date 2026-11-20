#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

enum class Compression { None, Zstd, Yaz0 };

Compression detectCompression(QByteArrayView head);
QString compressionName(Compression compression);
QString decompressedName(const QString &fileName);
std::optional<QByteArray> decompress(const QByteArray &data, QString *error);

constexpr int kMaxCompressionLayers = 4;
constexpr qsizetype kCompressionHeadSize = 32;

struct UnwrappedData
{
    QByteArray bytes;
    QString name;
    std::vector<QByteArray> layerHeads;
};

using UnwrapStop = std::function<bool(const QByteArray &bytes, const QString &name)>;

std::optional<UnwrappedData> unwrapCompression(QByteArray data, QString name, QString *error, const UnwrapStop &stop = {});
