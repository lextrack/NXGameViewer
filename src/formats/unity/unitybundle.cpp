#include "unity/unitybundle.h"

#include "core/bytes.h"
#include "unity/lz4block.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

std::optional<QByteArray> unpack(QByteArrayView data, quint32 compression, qsizetype size, QString *error)
{
    switch (compression) {
    case 0:
        if (data.size() != size)
            break;
        return data.toByteArray();
    case 2:
    case 3:
        if (auto out = lz4BlockDecompress(data, size))
            return out;
        break;
    case 1:
        if (error)
            *error = QObject::tr("This bundle is compressed with LZMA, which is not supported yet.");
        return std::nullopt;
    default:
        if (error)
            *error = QObject::tr("Unknown bundle compression %1.").arg(compression);
        return std::nullopt;
    }
    if (error)
        *error = QObject::tr("A compressed block of the bundle is damaged.");
    return std::nullopt;
}

}

int UnityBundle::find(const QString &path) const
{
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].path.compare(path, Qt::CaseInsensitive) == 0)
            return int(i);
    return -1;
}

namespace {

constexpr qint64 kHeadSize = 4096;

std::optional<UnityBundle> parseBundle(UnityBundle::StoredReader readAt, qint64 fileSize, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<UnityBundle> {
        if (error && error->isEmpty())
            *error = message;
        return std::nullopt;
    };
    try {
        const QByteArray head = readAt(0, std::min(fileSize, kHeadSize));
        ByteCursor in(head);
        if (in.cstring() != QLatin1String("UnityFS"))
            return fail(QObject::tr("Not a Unity bundle (no \"UnityFS\" at the start)."));
        UnityBundle bundle;
        bundle.formatVersion = in.be<quint32>();
        bundle.unityVersion = in.cstring();
        bundle.unityRevision = in.cstring();
        in.be<qint64>();
        const quint32 compressedInfo = in.be<quint32>();
        const quint32 infoSize = in.be<quint32>();
        const quint32 flags = in.be<quint32>();
        if (bundle.formatVersion >= 7)
            in.align(16);
        qint64 infoAt = in.pos();
        if (flags & 0x80) {
            if (compressedInfo > fileSize)
                return fail(QObject::tr("The bundle's file list does not fit in the file."));
            infoAt = fileSize - compressedInfo;
        }
        if (infoAt > fileSize - compressedInfo)
            return fail(QObject::tr("The bundle ends too early or is damaged."));
        const QByteArray infoBytes = readAt(infoAt, compressedInfo);
        if (infoBytes.size() != qsizetype(compressedInfo))
            return fail(QObject::tr("The bundle ends too early or is damaged."));
        const std::optional<QByteArray> info = unpack(infoBytes, flags & 0x3F, infoSize, error);
        if (!info)
            return fail(QObject::tr("The bundle's file list could not be read."));
        qint64 dataAt = (flags & 0x80) ? in.pos() : infoAt + compressedInfo;
        if (flags & 0x200)
            dataAt = (dataAt + 15) / 16 * 16;

        ByteCursor list(*info);
        list.take(16);
        const qint32 blockCount = list.be<qint32>();
        if (blockCount < 0 || blockCount > 1'000'000)
            return fail(QObject::tr("The bundle's block list is damaged."));
        qint64 total = 0;
        qint64 stored = dataAt;
        for (qint32 i = 0; i < blockCount; ++i) {
            UnityBundleBlock b;
            b.size = list.be<quint32>();
            b.storedSize = list.be<quint32>();
            b.flags = list.be<quint16>();
            b.offset = total;
            b.storedOffset = stored;
            if (stored > fileSize - b.storedSize)
                return fail(QObject::tr("The bundle ends too early or is damaged."));
            total += b.size;
            stored += b.storedSize;
            bundle.blocks.push_back(b);
        }
        if (total > qint64(4) * 1024 * 1024 * 1024)
            return fail(QObject::tr("The bundle is too large to open."));
        const qint32 nodeCount = list.be<qint32>();
        if (nodeCount < 0 || nodeCount > 1'000'000)
            return fail(QObject::tr("The bundle's file list is damaged."));
        for (qint32 i = 0; i < nodeCount; ++i) {
            UnityBundleNode node;
            node.offset = list.be<qint64>();
            node.size = list.be<qint64>();
            node.flags = list.be<quint32>();
            node.path = list.cstring();
            if (node.offset < 0 || node.size < 0 || node.offset > total - node.size)
                return fail(QObject::tr("%1 points outside the bundle.").arg(node.path));
            bundle.nodes.push_back(std::move(node));
        }
        bundle.stored = std::move(readAt);
        return bundle;
    } catch (const std::runtime_error &) {
        return fail(QObject::tr("The bundle ends too early or is damaged."));
    }
}

}

std::optional<QByteArray> UnityBundle::read(size_t index, qint64 offset, qint64 size, QString *error) const
{
    auto fail = [error](const QString &message) -> std::optional<QByteArray> {
        if (error && error->isEmpty())
            *error = message;
        return std::nullopt;
    };
    if (index >= nodes.size())
        return fail(QObject::tr("There is no such file in the bundle."));
    const UnityBundleNode &node = nodes[index];
    if (offset < 0 || size < 0 || offset > node.size - size)
        return fail(QObject::tr("The requested data is outside %1.").arg(node.path));
    QByteArray out(qsizetype(size), Qt::Uninitialized);
    const qint64 from = node.offset + offset, to = from + size;
    auto it = std::upper_bound(blocks.begin(), blocks.end(), from,
                               [](qint64 at, const UnityBundleBlock &b) { return at < b.offset + b.size; });
    for (; it != blocks.end() && it->offset < to; ++it) {
        const QByteArray packed = stored ? stored(it->storedOffset, it->storedSize) : QByteArray();
        if (packed.size() != it->storedSize)
            return fail(QObject::tr("The bundle could not be read again; it may have changed on disk."));
        const std::optional<QByteArray> chunk = unpack(packed, it->flags & 0x3F, qsizetype(it->size), error);
        if (!chunk)
            return fail(QObject::tr("A block of the bundle could not be read."));
        const qint64 a = std::max(from, it->offset), b = std::min(to, it->offset + it->size);
        std::memcpy(out.data() + (a - from), chunk->constData() + (a - it->offset), size_t(b - a));
    }
    return out;
}

std::optional<QByteArray> UnityBundle::nodeData(size_t index, QString *error) const
{
    if (index >= nodes.size())
        return std::nullopt;
    return read(index, 0, nodes[index].size, error);
}

std::optional<UnityBundle> readUnityBundle(const QByteArray &file, QString *error)
{
    UnityBundle::StoredReader readAt = storedBytesReader(file);
    return parseBundle(std::move(readAt), file.size(), error);
}

std::optional<UnityBundle> readUnityBundleStored(const StoredReader &read, qint64 size, QString *error)
{
    return parseBundle(read, size, error);
}

std::optional<UnityBundle> readUnityBundleFile(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        if (error)
            *error = QObject::tr("The file does not exist.");
        return std::nullopt;
    }
    const qint64 fileSize = info.size();
    const QDateTime modified = info.lastModified();
    UnityBundle::StoredReader readAt = storedFileReader(path, fileSize, modified);
    return parseBundle(std::move(readAt), fileSize, error);
}
