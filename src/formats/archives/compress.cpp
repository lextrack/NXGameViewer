#include "archives/compress.h"
#include "archives/zstddictionaries.h"

#include <QObject>
#include <QtEndian>

#include <zstd.h>

#include <algorithm>
#include <memory>
#include <new>

namespace {

constexpr qsizetype kYaz0Window = 0x1000;
constexpr qsizetype kYaz0MaxMatch = 0x111;
constexpr qsizetype kYaz0MinMatch = 3;
constexpr int kYaz0ChainDepth = 128;
constexpr int kHashBits = 15;

template <typename T>
std::optional<T> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

quint32 hash3(const uchar *p)
{
    return ((quint32(p[0]) << 16 | quint32(p[1]) << 8 | p[2]) * 2654435761u) >> (32 - kHashBits);
}

}

std::optional<CompressionLayer> describeCompression(const QByteArray &data, QString *error)
{
    CompressionLayer layer;
    layer.kind = detectCompression(data);
    if (layer.kind == Compression::Zstd) {
        if (data.size() < 6)
            return fail<CompressionLayer>(error, QObject::tr("The zstd header is cut short."));
        layer.zstdDictionary = ZSTD_getDictID_fromFrame(data.constData(), size_t(data.size()));
        if (layer.zstdDictionary && zstdDictionaryBytes(layer.zstdDictionary).isEmpty())
            return fail<CompressionLayer>(
                error, QObject::tr("This file was compressed with a zstd dictionary (id %1) that was not found. Open it "
                                   "from the game's romfs folder, where Pack/ZsDic.pack.zs holds the dictionaries.")
                           .arg(layer.zstdDictionary));
        layer.zstdChecksum = (uchar(data[4]) & 0x04) != 0;
    } else if (layer.kind == Compression::Yaz0) {
        if (data.size() < 16)
            return fail<CompressionLayer>(error, QObject::tr("The Yaz0 header is cut short."));
        layer.yaz0Alignment = qFromBigEndian<quint32>(data.constData() + 8);
    }
    return layer;
}

std::optional<QByteArray> compressZstd(const QByteArray &data, int level, bool checksum, QString *error,
                                       unsigned dictionary)
{
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    if (!context)
        return fail<QByteArray>(error, QObject::tr("Not enough memory to compress this file."));
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, checksum ? 1 : 0);
    ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, 1);
    if (dictionary) {
        const QByteArray bytes = zstdDictionaryBytes(dictionary);
        if (bytes.isEmpty())
            return fail<QByteArray>(error, QObject::tr("The zstd dictionary (id %1) this file needs was not found.").arg(dictionary));
        if (ZSTD_isError(ZSTD_CCtx_loadDictionary(context.get(), bytes.constData(), size_t(bytes.size()))))
            return fail<QByteArray>(error, QObject::tr("The zstd dictionary could not be used."));
    }
    try {
        QByteArray out(qsizetype(ZSTD_compressBound(size_t(data.size()))), Qt::Uninitialized);
        const size_t written =
            ZSTD_compress2(context.get(), out.data(), size_t(out.size()), data.constData(), size_t(data.size()));
        if (ZSTD_isError(written))
            return fail<QByteArray>(error, QObject::tr("zstd compression failed: %1").arg(QString::fromUtf8(ZSTD_getErrorName(written))));
        out.truncate(qsizetype(written));
        return out;
    } catch (const std::bad_alloc &) {
        return fail<QByteArray>(error, QObject::tr("Not enough memory to compress this file."));
    }
}

QByteArray compressYaz0(const QByteArray &data, quint32 alignment)
{
    const auto *src = reinterpret_cast<const uchar *>(data.constData());
    const qsizetype size = data.size();
    QByteArray out;
    out.reserve(16 + size + size / 8 + 16);
    out.append("Yaz0", 4);
    char field[4];
    qToBigEndian(quint32(size), field);
    out.append(field, 4);
    qToBigEndian(alignment, field);
    out.append(field, 4);
    out.append(QByteArray(4, '\0'));

    std::vector<qint32> head(size_t(1) << kHashBits, -1);
    std::vector<qint32> previous(size_t(kYaz0Window), -1);
    auto insert = [&](qsizetype at) {
        if (at + kYaz0MinMatch > size)
            return;
        const quint32 h = hash3(src + at);
        previous[size_t(at % kYaz0Window)] = head[h];
        head[h] = qint32(at);
    };

    qsizetype pos = 0;
    while (pos < size) {
        const qsizetype codeAt = out.size();
        out.append('\0');
        uchar code = 0;
        for (int bit = 7; bit >= 0 && pos < size; --bit) {
            qsizetype bestLength = 0, bestDistance = 0;
            if (pos + kYaz0MinMatch <= size) {
                const qsizetype limit = std::min(kYaz0MaxMatch, size - pos);
                qint32 candidate = head[hash3(src + pos)];
                for (int depth = 0; candidate >= 0 && depth < kYaz0ChainDepth; ++depth) {
                    const qsizetype distance = pos - candidate;
                    if (distance > kYaz0Window || distance <= 0)
                        break;
                    qsizetype length = 0;
                    while (length < limit && src[candidate + length] == src[pos + length])
                        ++length;
                    if (length > bestLength) {
                        bestLength = length;
                        bestDistance = distance;
                        if (length == limit)
                            break;
                    }
                    const qint32 next = previous[size_t(candidate % kYaz0Window)];
                    if (next >= candidate)
                        break;
                    candidate = next;
                }
            }
            if (bestLength < kYaz0MinMatch) {
                code = uchar(code | (1 << bit));
                out.append(char(src[pos]));
                insert(pos);
                ++pos;
                continue;
            }
            const qsizetype d = bestDistance - 1;
            if (bestLength >= 0x12) {
                out.append(char(d >> 8));
                out.append(char(d & 0xFF));
                out.append(char(bestLength - 0x12));
            } else {
                out.append(char(((bestLength - 2) << 4) | (d >> 8)));
                out.append(char(d & 0xFF));
            }
            for (qsizetype k = 0; k < bestLength; ++k)
                insert(pos + k);
            pos += bestLength;
        }
        out[codeAt] = char(code);
    }
    return out;
}

std::optional<QByteArray> compressLike(const QByteArray &data, const CompressionLayer &layer, QString *error)
{
    switch (layer.kind) {
    case Compression::Zstd: return compressZstd(data, kZstdLevel, layer.zstdChecksum, error, layer.zstdDictionary);
    case Compression::Yaz0:
        if (data.size() > qsizetype(0xFFFFFFFFll))
            return fail<QByteArray>(error, QObject::tr("Yaz0 cannot hold more than 4 GB."));
        try {
            return compressYaz0(data, layer.yaz0Alignment);
        } catch (const std::bad_alloc &) {
            return fail<QByteArray>(error, QObject::tr("Not enough memory to compress this file."));
        }
    case Compression::None: break;
    }
    return data;
}

std::optional<QByteArray> unpackAll(const QByteArray &data, std::vector<CompressionLayer> *layers, QString *error)
{
    std::optional<UnwrappedData> unwrapped = unwrapCompression(data, QString(), error);
    if (!unwrapped)
        return std::nullopt;
    for (const QByteArray &head : unwrapped->layerHeads) {
        const std::optional<CompressionLayer> layer = describeCompression(head, error);
        if (!layer)
            return std::nullopt;
        if (layers)
            layers->push_back(*layer);
    }
    return std::move(unwrapped->bytes);
}

std::optional<QByteArray> packAll(const QByteArray &data, const std::vector<CompressionLayer> &layers, QString *error)
{
    QByteArray bytes = data;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        std::optional<QByteArray> packed = compressLike(bytes, *it, error);
        if (!packed)
            return std::nullopt;
        bytes = std::move(*packed);
    }
    return bytes;
}
