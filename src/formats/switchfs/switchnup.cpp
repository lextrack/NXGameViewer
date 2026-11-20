#include "switchfs/switchnup.h"

#include "core/paths.h"
#include "switchfs/switchnca.h"
#include "switchfs/switchncz.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <map>

namespace {

constexpr qint64 kSuperblockSize = 0x80;
constexpr qint64 kTocHeaderSize = 0x30;
constexpr qint64 kPackageSize = 0x40;
constexpr qint64 kDirSize = 0x18;
constexpr qint64 kFileSize = 0x50;
constexpr qint64 kMaxToc = qint64{512} * 1024 * 1024;
constexpr qint64 kMaxMeta = qint64{16} * 1024 * 1024;
constexpr quint32 kNone = 0xFFFFFFFFu;
constexpr qint64 kChunk = qint64{8} * 1024 * 1024;
constexpr int kMaxDepth = 256;

quint16 u16(const QByteArray &b, qint64 at)
{
    return qFromLittleEndian<quint16>(b.constData() + at);
}

quint32 u32(const QByteArray &b, qint64 at)
{
    return qFromLittleEndian<quint32>(b.constData() + at);
}

quint64 u64(const QByteArray &b, qint64 at)
{
    return qFromLittleEndian<quint64>(b.constData() + at);
}

struct NupPackageRecord
{
    quint64 titleId = 0;
    quint32 rootDir = kNone;
    quint32 exefsDir = kNone;
    quint32 romfsDir = kNone;
    quint32 version = 0;
    quint8 kind = 0;
    qint64 metaOffset = 0;
    qint64 metaSize = 0;
};

struct NupEntry
{
    QString path;
    qint64 size = 0;
    qint64 extentOffset = 0;
    qint64 extentSize = 0;
    QByteArray sha256;
    quint8 codec = 0;
};

struct Nup
{
    StoragePtr file;
    QByteArray toc;
    qint64 blobOffset = 0;
    qint64 dirCount = 0;
    qint64 fileCount = 0;
    qint64 dirsAt = 0;
    qint64 filesAt = 0;
    qint64 childrenAt = 0;
    qint64 childrenEnd = 0;
    qint64 poolAt = 0;
    qint64 poolSize = 0;
    std::vector<NupPackageRecord> packages;

    std::optional<QString> name(quint32 offset, quint32 length) const
    {
        if (qint64(offset) > poolSize || qint64(length) > poolSize - qint64(offset))
            return std::nullopt;
        return QString::fromUtf8(toc.constData() + poolAt + offset, qsizetype(length));
    }
};

std::optional<Nup> openNup(const QString &path, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Nup> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const QString damaged = QObject::tr("The NUP file is damaged.");
    Nup n;
    n.file = fileStorage(path, error);
    if (!n.file)
        return std::nullopt;
    const qint64 fileSize = n.file->size();
    const QByteArray sb = n.file->read(0, std::min(fileSize, kSuperblockSize));
    if (sb.size() < kSuperblockSize || !sb.startsWith(QByteArrayLiteral("NUP\x01")))
        return fail(QObject::tr("Not a NUP file."));
    const qint64 headerSize = u16(sb, 4);
    const qint64 tocOffset = qint64(u64(sb, 8)), tocSize = qint64(u64(sb, 0x10));
    n.blobOffset = qint64(u64(sb, 0x38));
    const quint64 declaredSize = u64(sb, 0x40);
    const quint32 packageCount = u32(sb, 0x48);
    if (u32(sb, 0x4C) != 0)
        return fail(QObject::tr("This NUP file uses features the app cannot read yet."));
    if (headerSize < kSuperblockSize || tocOffset != headerSize || tocSize < kTocHeaderSize || tocSize > kMaxToc
        || tocOffset > fileSize - tocSize || n.blobOffset % 16 != 0 || n.blobOffset < tocOffset + tocSize
        || n.blobOffset > fileSize || declaredSize != quint64(fileSize) || packageCount == 0)
        return fail(damaged);
    n.toc = n.file->read(tocOffset, tocSize);
    if (n.toc.size() != tocSize)
        return fail(QObject::tr("The NUP file is cut off."));
    if (QCryptographicHash::hash(n.toc, QCryptographicHash::Sha256) != sb.mid(0x18, 32))
        return fail(QObject::tr("The NUP table of contents does not match its checksum: the file is damaged."));
    if (!n.toc.startsWith(QByteArrayLiteral("TOC\x01")) || u32(n.toc, 4) != packageCount)
        return fail(damaged);
    n.dirCount = qint64(u64(n.toc, 8));
    n.fileCount = qint64(u64(n.toc, 0x10));
    n.poolAt = qint64(u64(n.toc, 0x18));
    n.poolSize = qint64(u64(n.toc, 0x20));
    if (n.dirCount < 0 || n.fileCount < 0 || n.dirCount > tocSize / kDirSize || n.fileCount > tocSize / kFileSize
        || n.dirCount + n.fileCount > qint64(kNone) || n.poolAt < 0 || n.poolSize < 0 || n.poolAt > tocSize - n.poolSize)
        return fail(damaged);
    n.dirsAt = kTocHeaderSize + kPackageSize * qint64(packageCount);
    n.filesAt = n.dirsAt + kDirSize * n.dirCount;
    n.childrenAt = n.filesAt + kFileSize * n.fileCount;
    n.childrenEnd = n.poolAt >= n.childrenAt ? n.poolAt : tocSize;
    if (n.childrenAt > tocSize || n.childrenEnd > tocSize)
        return fail(damaged);
    for (quint32 i = 0; i < packageCount; ++i) {
        const qint64 at = kTocHeaderSize + kPackageSize * i;
        NupPackageRecord p;
        p.titleId = u64(n.toc, at);
        p.rootDir = u32(n.toc, at + 8);
        p.exefsDir = u32(n.toc, at + 0x0C);
        p.romfsDir = u32(n.toc, at + 0x10);
        p.version = u32(n.toc, at + 0x14);
        p.kind = quint8(n.toc[at + 0x18]);
        p.metaOffset = qint64(u64(n.toc, at + 0x20));
        p.metaSize = u32(n.toc, at + 0x28);
        for (quint32 dir : {p.rootDir, p.exefsDir, p.romfsDir})
            if (dir != kNone && qint64(dir) >= n.dirCount)
                return fail(damaged);
        if (p.metaSize > kMaxMeta || p.metaOffset < 0 || (p.metaSize > 0 && p.metaOffset > fileSize - p.metaSize))
            return fail(damaged);
        n.packages.push_back(p);
    }
    return n;
}

bool listTree(const Nup &n, quint32 dir, const QString &prefix, std::vector<NupEntry> &out, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString damaged = QObject::tr("The NUP file list is damaged.");
    struct Pending
    {
        quint32 dir;
        QString prefix;
        int depth;
    };
    std::vector<Pending> stack{{dir, prefix, 0}};
    QSet<quint32> seen;
    const qint64 fileEnd = n.file->size();
    while (!stack.empty()) {
        const Pending p = stack.back();
        stack.pop_back();
        if (seen.contains(p.dir) || p.depth > kMaxDepth)
            return fail(damaged);
        seen.insert(p.dir);
        const qint64 at = n.dirsAt + kDirSize * p.dir;
        const qint64 first = u32(n.toc, at + 8), count = u32(n.toc, at + 0x0C);
        if (n.childrenAt + 4 * (first + count) > n.childrenEnd)
            return fail(damaged);
        for (qint64 c = count - 1; c >= 0; --c) {
            const quint32 node = u32(n.toc, n.childrenAt + 4 * (first + c));
            if (qint64(node) < n.dirCount) {
                const qint64 childAt = n.dirsAt + kDirSize * node;
                const std::optional<QString> name = n.name(u32(n.toc, childAt), u32(n.toc, childAt + 4));
                if (!name || name->isEmpty())
                    return fail(damaged);
                stack.push_back({node, p.prefix + *name + u'/', p.depth + 1});
                continue;
            }
            const qint64 index = qint64(node) - n.dirCount;
            if (index >= n.fileCount)
                return fail(damaged);
            const qint64 fileAt = n.filesAt + kFileSize * index;
            const std::optional<QString> name = n.name(u32(n.toc, fileAt), u32(n.toc, fileAt + 4));
            if (!name || name->isEmpty())
                return fail(damaged);
            NupEntry e;
            e.path = p.prefix + *name;
            e.size = qint64(u64(n.toc, fileAt + 8));
            e.extentOffset = qint64(u64(n.toc, fileAt + 0x10));
            e.extentSize = qint64(u64(n.toc, fileAt + 0x18));
            e.sha256 = n.toc.mid(fileAt + 0x20, 32);
            e.codec = quint8(n.toc[fileAt + 0x40]);
            if (!isSafeRelativePath(e.path))
                return fail(QObject::tr("The NUP file has a file with an unsafe name: %1").arg(e.path));
            if (e.codec > 1)
                return fail(QObject::tr("The NUP file %1 uses a compression the app cannot read (%2).")
                                .arg(e.path)
                                .arg(e.codec));
            const bool raw = e.codec == 0;
            if (e.size < 0 || e.extentSize < 0 || (raw && e.extentSize != e.size)
                || (e.extentSize > 0 && (e.extentOffset < n.blobOffset || e.extentOffset > fileEnd - e.extentSize)))
                return fail(damaged);
            out.push_back(std::move(e));
        }
    }
    return true;
}

std::optional<std::vector<NupEntry>> packageEntries(const Nup &n, const NupPackageRecord &p, QString *error)
{
    std::vector<NupEntry> out;
    if (p.exefsDir != kNone && !listTree(n, p.exefsDir, QStringLiteral("exefs/"), out, error))
        return std::nullopt;
    if (p.romfsDir != kNone && !listTree(n, p.romfsDir, QStringLiteral("romfs/"), out, error))
        return std::nullopt;
    std::stable_sort(out.begin(), out.end(), [](const NupEntry &a, const NupEntry &b) { return a.extentOffset < b.extentOffset; });
    return out;
}

StoragePtr entryStorage(const Nup &n, const NupEntry &e)
{
    if (e.size == 0)
        return bytesStorage({});
    StoragePtr extent = subStorage(n.file, e.extentOffset, e.extentSize);
    return e.codec == 0 ? extent : zstdFrameStorage(extent, e.size);
}

std::map<quint16, QByteArray> readMeta(const Nup &n, const NupPackageRecord &p)
{
    std::map<quint16, QByteArray> out;
    if (p.metaSize <= 0)
        return out;
    const QByteArray meta = n.file->read(p.metaOffset, p.metaSize);
    for (qint64 at = 0; at + 8 <= meta.size();) {
        const quint16 type = u16(meta, at);
        const qint64 size = u32(meta, at + 4);
        if (size > meta.size() - at - 8)
            break;
        out.emplace(type, meta.mid(at + 8, size));
        at += 8 + size;
    }
    return out;
}

SwitchTitle::Type typeOf(quint8 kind)
{
    switch (kind) {
    case 0: return SwitchTitle::Application;
    case 1: return SwitchTitle::Patch;
    case 2: return SwitchTitle::AddOn;
    default: return SwitchTitle::Other;
    }
}

}

bool isNupFile(const QString &path)
{
    if (QFileInfo(path).suffix().compare(QLatin1String("nup"), Qt::CaseInsensitive) == 0)
        return true;
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(4) == QByteArrayLiteral("NUP\x01");
}

std::vector<SwitchTitle> scanNupPackage(const QString &path, QString *error)
{
    const std::optional<Nup> n = openNup(path, error);
    if (!n)
        return {};
    QString baseName, basePublisher;
    QByteArray baseIcon;
    for (const NupPackageRecord &p : n->packages)
        if (p.kind == 0) {
            const auto meta = readMeta(*n, p);
            baseName = meta.count(5) ? QString::fromUtf8(meta.at(5)) : QString();
            basePublisher = meta.count(6) ? QString::fromUtf8(meta.at(6)) : QString();
            baseIcon = meta.count(3) ? meta.at(3) : QByteArray();
        }
    std::vector<SwitchTitle> titles;
    for (size_t i = 0; i < n->packages.size(); ++i) {
        const NupPackageRecord &p = n->packages[i];
        const auto meta = readMeta(*n, p);
        SwitchTitle t;
        t.package = path;
        t.nupPackage = int(i);
        t.titleId = p.titleId;
        t.version = p.version;
        t.type = typeOf(p.kind);
        t.name = meta.count(5) ? QString::fromUtf8(meta.at(5)) : baseName;
        t.publisher = meta.count(6) ? QString::fromUtf8(meta.at(6)) : basePublisher;
        t.displayVersion = meta.count(4) ? QString::fromUtf8(meta.at(4)) : QString();
        t.icon = meta.count(3) ? meta.at(3) : baseIcon;
        if (t.type == SwitchTitle::Other)
            t.problem = QObject::tr("This content is not a game, update or DLC.");
        else if (p.exefsDir == kNone && p.romfsDir == kNone)
            t.problem = QObject::tr("This content has no ExeFS or RomFS.");
        titles.push_back(std::move(t));
    }
    return titles;
}

std::optional<std::vector<SwitchTitleFile>> nupTitleFiles(const SwitchTitle &title, QString *error)
{
    const std::optional<Nup> n = openNup(title.package, error);
    if (!n)
        return std::nullopt;
    if (title.nupPackage < 0 || size_t(title.nupPackage) >= n->packages.size()) {
        if (error)
            *error = QObject::tr("The NUP file has changed since it was read.");
        return std::nullopt;
    }
    const std::optional<std::vector<NupEntry>> entries = packageEntries(*n, n->packages[size_t(title.nupPackage)], error);
    if (!entries)
        return std::nullopt;
    std::vector<SwitchTitleFile> files;
    files.reserve(entries->size());
    for (const NupEntry &e : *entries) {
        StoragePtr storage = entryStorage(*n, e);
        if (!storage) {
            if (error)
                *error = QObject::tr("The NUP file list is damaged.");
            return std::nullopt;
        }
        const bool fast = storage->seeksFast();
        files.push_back({e.path, e.size,
                         [storage](qint64 offset, char *out, qint64 length) { return storage->read(offset, out, length); },
                         fast});
    }
    return files;
}

bool verifyNupTitle(const SwitchTitle &title, const std::function<bool(qint64 done, qint64 total)> &progress,
                    QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const std::optional<Nup> n = openNup(title.package, error);
    if (!n)
        return false;
    if (title.nupPackage < 0 || size_t(title.nupPackage) >= n->packages.size())
        return fail(QObject::tr("The NUP file has changed since it was read."));
    const std::optional<std::vector<NupEntry>> entries = packageEntries(*n, n->packages[size_t(title.nupPackage)], error);
    if (!entries)
        return false;
    qint64 total = 0;
    for (const NupEntry &e : *entries)
        total += e.size;
    QByteArray buffer(kChunk, Qt::Uninitialized);
    qint64 done = 0;
    for (const NupEntry &e : *entries) {
        StoragePtr storage = entryStorage(*n, e);
        if (!storage)
            return fail(QObject::tr("The NUP file list is damaged."));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (qint64 at = 0; at < e.size;) {
            const qint64 step = std::min(kChunk, e.size - at);
            if (!storage->read(at, buffer.data(), step))
                return fail(QObject::tr("Cannot read %1 from the NUP file.").arg(e.path));
            hash.addData(QByteArrayView(buffer.constData(), step));
            at += step;
            done += step;
            if (progress && !progress(done, total))
                return fail(QObject::tr("Canceled."));
        }
        if (hash.result() != e.sha256)
            return fail(QObject::tr("%1 does not match its checksum: the NUP file is damaged.").arg(e.path));
    }
    return true;
}
