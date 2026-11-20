#include "archives/decompress.h"
#include "archives/zstddictionaries.h"

#include <QFileInfo>
#include <QObject>
#include <QtEndian>

#include <zstd.h>

#include <functional>
#include <memory>
#include <new>

namespace {

constexpr qint64 kMaxDecompressed = qint64{1024} * 1024 * 1024;

std::optional<QByteArray> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

using DStream = std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)>;

std::optional<QByteArray> decompressZstd(const QByteArray &data, QString *error)
{
    const unsigned dictionaryId = ZSTD_getDictID_fromFrame(data.constData(), size_t(data.size()));
    const ZSTD_DDict *dictionary = dictionaryId ? zstdDictionary(dictionaryId) : nullptr;
    if (dictionaryId && !dictionary)
        return fail(error, QObject::tr("This file was compressed with a zstd dictionary (id %1) that was not found. Open it "
                                       "from the game's romfs folder, where Pack/ZsDic.pack.zs holds the dictionaries.")
                               .arg(dictionaryId));
    const DStream stream(ZSTD_createDStream(), &ZSTD_freeDStream);
    if (!stream)
        return fail(error, QObject::tr("Not enough memory to decompress this file."));
    if (dictionary && ZSTD_isError(ZSTD_DCtx_refDDict(stream.get(), dictionary)))
        return fail(error, QObject::tr("The zstd dictionary could not be used."));
    QByteArray out;
    const unsigned long long declared = ZSTD_getFrameContentSize(data.constData(), size_t(data.size()));
    if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != ZSTD_CONTENTSIZE_ERROR && qint64(declared) <= kMaxDecompressed)
        out.reserve(qsizetype(declared));
    QByteArray chunk(qsizetype(ZSTD_DStreamOutSize()), Qt::Uninitialized);
    ZSTD_inBuffer in{data.constData(), size_t(data.size()), 0};
    size_t result = 0;
    for (;;) {
        ZSTD_outBuffer o{chunk.data(), size_t(chunk.size()), 0};
        result = ZSTD_decompressStream(stream.get(), &o, &in);
        if (ZSTD_isError(result))
            return fail(error, QObject::tr("The zstd data is damaged: %1").arg(QString::fromUtf8(ZSTD_getErrorName(result))));
        out.append(chunk.constData(), qsizetype(o.pos));
        if (out.size() > kMaxDecompressed)
            return fail(error, QObject::tr("The file decompresses to more than 1 GB."));
        if (in.pos == in.size && (result == 0 || o.pos < o.size))
            break;
    }
    if (result != 0)
        return fail(error, QObject::tr("The zstd data ends too early."));
    return out;
}

std::optional<QByteArray> decompressYaz0(const QByteArray &data, QString *error)
{
    if (data.size() < 16)
        return fail(error, QObject::tr("The Yaz0 header is cut short."));
    const quint32 size = qFromBigEndian<quint32>(data.constData() + 4);
    if (qint64(size) > kMaxDecompressed)
        return fail(error, QObject::tr("The file decompresses to more than 1 GB."));
    constexpr qint64 kMaxGroupInput = 1 + 8 * 3;
    constexpr qint64 kMaxGroupOutput = 8 * (0xFF + 0x12);
    if (qint64(size) > (qint64(data.size()) - 16 + kMaxGroupInput - 1) / kMaxGroupInput * kMaxGroupOutput)
        return fail(error, QObject::tr("The Yaz0 data ends too early."));
    QByteArray out(qsizetype(size), Qt::Uninitialized);
    const auto *src = reinterpret_cast<const uchar *>(data.constData());
    auto *dst = reinterpret_cast<uchar *>(out.data());
    qsizetype in = 16, pos = 0;
    const qsizetype end = data.size();
    while (pos < qsizetype(size)) {
        if (in >= end)
            return fail(error, QObject::tr("The Yaz0 data ends too early."));
        const uchar code = src[in++];
        for (int bit = 7; bit >= 0 && pos < qsizetype(size); --bit) {
            if (code & (1 << bit)) {
                if (in >= end)
                    return fail(error, QObject::tr("The Yaz0 data ends too early."));
                dst[pos++] = src[in++];
                continue;
            }
            if (in + 2 > end)
                return fail(error, QObject::tr("The Yaz0 data ends too early."));
            const uchar b1 = src[in], b2 = src[in + 1];
            in += 2;
            const qsizetype distance = ((qsizetype(b1 & 0x0F) << 8) | b2) + 1;
            qsizetype count = b1 >> 4;
            if (count == 0) {
                if (in >= end)
                    return fail(error, QObject::tr("The Yaz0 data ends too early."));
                count = qsizetype(src[in++]) + 0x12;
            } else {
                count += 2;
            }
            if (distance > pos)
                return fail(error, QObject::tr("The Yaz0 data is damaged."));
            for (qsizetype k = 0; k < count && pos < qsizetype(size); ++k, ++pos)
                dst[pos] = dst[pos - distance];
        }
    }
    return out;
}

}

Compression detectCompression(QByteArrayView head)
{
    if (head.size() >= 4 && uchar(head[0]) == 0x28 && uchar(head[1]) == 0xB5 && uchar(head[2]) == 0x2F
        && uchar(head[3]) == 0xFD)
        return Compression::Zstd;
    if (head.startsWith("Yaz0"))
        return Compression::Yaz0;
    return Compression::None;
}

QString compressionName(Compression compression)
{
    switch (compression) {
    case Compression::Zstd: return QStringLiteral("zstd");
    case Compression::Yaz0: return QStringLiteral("Yaz0");
    case Compression::None: break;
    }
    return {};
}

QString decompressedName(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("zs") || suffix == QLatin1String("zst") || suffix == QLatin1String("zstd"))
        return fileName.left(fileName.size() - suffix.size() - 1);
    if (suffix == QLatin1String("szs"))
        return fileName.left(fileName.size() - 4) + QStringLiteral(".sarc");
    return fileName;
}

std::optional<QByteArray> decompress(const QByteArray &data, QString *error)
{
    try {
        switch (detectCompression(data)) {
        case Compression::Zstd: return decompressZstd(data, error);
        case Compression::Yaz0: return decompressYaz0(data, error);
        case Compression::None: break;
        }
    } catch (const std::bad_alloc &) {
        return fail(error, QObject::tr("Not enough memory to decompress this file."));
    }
    return data;
}

std::optional<UnwrappedData> unwrapCompression(QByteArray data, QString name, QString *error, const UnwrapStop &stop)
{
    UnwrappedData out;
    out.bytes = std::move(data);
    out.name = std::move(name);
    for (int depth = 0; depth < kMaxCompressionLayers; ++depth) {
        if (detectCompression(out.bytes) == Compression::None || (stop && stop(out.bytes, out.name)))
            break;
        QByteArray head = out.bytes.left(kCompressionHeadSize);
        std::optional<QByteArray> unpacked = decompress(out.bytes, error);
        if (!unpacked)
            return std::nullopt;
        out.bytes = std::move(*unpacked);
        out.name = decompressedName(out.name);
        out.layerHeads.push_back(std::move(head));
    }
    return out;
}
