#include "switchfs/switchnca.h"
#include "unity/lz4block.h"

#include "core/crypto.h"
#include "core/paths.h"
#include "switchfs/switchkeys.h"

#include <QCryptographicHash>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace {

constexpr quint32 kMaxPartitionFiles = 1 << 16;
constexpr qint64 kMaxPartitionHeader = qint64{64} * 1024 * 1024;
constexpr qint64 kMaxRomfsTable = qint64{512} * 1024 * 1024;
constexpr quint32 kNone = 0xFFFFFFFFu;

quint32 u32(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<quint32>(b.constData() + at);
}

quint64 u64(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<quint64>(b.constData() + at);
}

class FileStorage : public SwitchStorage
{
public:
    explicit FileStorage(const QString &path) : m_file(path) {}
    bool open() { return m_file.open(QIODevice::ReadOnly); }
    QString errorString() const { return m_file.errorString(); }
    qint64 size() const override { return m_file.size(); }
    bool read(qint64 offset, char *out, qint64 length) const override
    {
        const QMutexLocker lock(&m_mutex);
        if (offset < 0 || length < 0 || offset > m_file.size() - length || !m_file.seek(offset))
            return false;
        return m_file.read(out, length) == length;
    }

private:
    mutable QFile m_file;
    mutable QMutex m_mutex;
};

class BytesStorage : public SwitchStorage
{
public:
    explicit BytesStorage(QByteArray data) : m_data(std::move(data)) {}
    qint64 size() const override { return m_data.size(); }
    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > m_data.size() - length)
            return false;
        std::memcpy(out, m_data.constData() + offset, size_t(length));
        return true;
    }

private:
    QByteArray m_data;
};

class SubStorage : public SwitchStorage
{
public:
    SubStorage(StoragePtr base, qint64 offset, qint64 size) : m_base(std::move(base)), m_offset(offset), m_size(size) {}
    qint64 size() const override { return m_size; }
    bool seeksFast() const override { return m_base->seeksFast(); }
    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > m_size - length)
            return false;
        return m_base->read(m_offset + offset, out, length);
    }

private:
    StoragePtr m_base;
    qint64 m_offset;
    qint64 m_size;
};

class CtrStorage : public SwitchStorage
{
public:
    CtrStorage(StoragePtr nca, qint64 offset, qint64 size, QByteArray key, QByteArray upper)
        : m_nca(std::move(nca)), m_offset(offset), m_size(size), m_key(std::move(key)), m_upper(std::move(upper))
    {
    }
    qint64 size() const override { return m_size; }
    bool seeksFast() const override { return m_nca->seeksFast(); }
    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > m_size - length)
            return false;
        if (length == 0)
            return true;
        const qint64 absolute = m_offset + offset;
        const qint64 aligned = absolute & ~qint64(15);
        const qint64 skip = absolute - aligned;
        const qint64 span = (skip + length + 15) & ~qint64(15);
        const qint64 available = std::min(span, m_nca->size() - aligned);
        if (available < skip + length)
            return false;
        QByteArray buffer(available, Qt::Uninitialized);
        if (!m_nca->read(aligned, buffer.data(), available)
            || !crypto::ctrApply(m_key, crypto::ctrCounter(m_upper, quint64(aligned)), buffer.data(), available))
            return false;
        std::memcpy(out, buffer.constData() + skip, size_t(length));
        return true;
    }

private:
    StoragePtr m_nca;
    qint64 m_offset;
    qint64 m_size;
    QByteArray m_key;
    QByteArray m_upper;
};

}

QByteArray SwitchStorage::read(qint64 offset, qint64 length) const
{
    if (length < 0 || length > kMaxRomfsTable)
        return {};
    QByteArray out(length, Qt::Uninitialized);
    if (!read(offset, out.data(), length))
        return {};
    return out;
}

StoragePtr fileStorage(const QString &path, QString *error)
{
    auto storage = std::make_shared<FileStorage>(path);
    if (!storage->open()) {
        if (error)
            *error = storage->errorString();
        return nullptr;
    }
    return storage;
}

StoragePtr bytesStorage(QByteArray data)
{
    return std::make_shared<BytesStorage>(std::move(data));
}

namespace {

class CompressedStorage : public SwitchStorage
{
public:
    enum Type : quint8 { Stored = 0, Zeros = 1, Lz4 = 3 };

    struct Entry
    {
        qint64 virtualOffset = 0;
        qint64 physicalOffset = 0;
        quint8 type = Stored;
        qint64 physicalSize = 0;
    };

    CompressedStorage(StoragePtr base, std::vector<Entry> entries, qint64 size)
        : m_base(std::move(base)), m_entries(std::move(entries)), m_size(size)
    {
    }
    qint64 size() const override { return m_size; }
    bool seeksFast() const override { return m_base->seeksFast(); }

    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > m_size - length)
            return false;
        if (length == 0)
            return true;
        auto it = std::upper_bound(m_entries.begin(), m_entries.end(), offset,
                                   [](qint64 o, const Entry &e) { return o < e.virtualOffset; });
        if (it == m_entries.begin())
            return false;
        for (size_t i = size_t(it - m_entries.begin() - 1); length > 0; ++i) {
            if (i >= m_entries.size())
                return false;
            const qint64 inside = offset - m_entries[i].virtualOffset;
            const qint64 count = std::min(length, virtualLength(i) - inside);
            if (!readEntry(i, inside, out, count))
                return false;
            offset += count;
            out += count;
            length -= count;
        }
        return true;
    }

private:
    qint64 virtualLength(size_t i) const
    {
        const qint64 next = i + 1 < m_entries.size() ? m_entries[i + 1].virtualOffset : m_size;
        return next - m_entries[i].virtualOffset;
    }

    bool readEntry(size_t i, qint64 inside, char *out, qint64 count) const
    {
        const Entry &e = m_entries[i];
        if (e.type == Stored)
            return m_base->read(e.physicalOffset + inside, out, count);
        if (e.type == Zeros) {
            std::memset(out, 0, size_t(count));
            return true;
        }
        QByteArray block;
        bool cached = false;
        {
            const QMutexLocker lock(&m_mutex);
            if (m_cachedEntry == i) {
                block = m_cachedBlock;
                cached = true;
            }
        }
        if (!cached) {
            const QByteArray packed = m_base->read(e.physicalOffset, e.physicalSize);
            if (packed.size() != e.physicalSize)
                return false;
            std::optional<QByteArray> unpacked = lz4BlockDecompress(packed, qsizetype(virtualLength(i)));
            if (!unpacked)
                return false;
            block = std::move(*unpacked);
            const QMutexLocker lock(&m_mutex);
            m_cachedBlock = block;
            m_cachedEntry = i;
        }
        std::memcpy(out, block.constData() + inside, size_t(count));
        return true;
    }

    StoragePtr m_base;
    std::vector<Entry> m_entries;
    qint64 m_size;
    mutable QMutex m_mutex;
    mutable size_t m_cachedEntry = size_t(-1);
    mutable QByteArray m_cachedBlock;
};

StoragePtr compressedStorage(const StoragePtr &section, const QByteArray &info, QString *error)
{
    auto fail = [error](const QString &message) -> StoragePtr {
        if (error)
            *error = message;
        return nullptr;
    };
    const QString damaged = QObject::tr("The compressed game data has a damaged table.");
    constexpr qint64 kNode = 0x4000;
    constexpr qint64 kEntrySize = 0x18;
    constexpr qint64 kEntriesPerNode = (kNode - 0x10) / kEntrySize;
    constexpr qint64 kOffsetsPerNode = (kNode - 0x10) / 8;
    constexpr qint64 kMaxBlock = qint64{16} * 1024 * 1024;
    const qint64 tableAt = qint64(u64(info, 0)), tableSize = qint64(u64(info, 8));
    const qint64 count = qint32(u32(info, 0x18));
    const qint64 sets = (count + kEntriesPerNode - 1) / kEntriesPerNode;
    if (info.mid(0x10, 4) != "BKTR")
        return fail(damaged);
    if (count <= 0 || tableAt < 0 || tableSize < kNode * (1 + sets) || tableAt > section->size() - tableSize)
        return fail(damaged);
    if (sets > kOffsetsPerNode)
        return fail(QObject::tr("This game's compressed data uses a table layout the app cannot read yet."));
    const QByteArray l1 = section->read(tableAt, kNode);
    if (l1.size() != kNode || qint32(u32(l1, 4)) != sets)
        return fail(damaged);
    const qint64 virtualSize = qint64(u64(l1, 8));
    std::vector<CompressedStorage::Entry> entries;
    entries.reserve(size_t(count));
    for (qint64 set = 0; set < sets; ++set) {
        const QByteArray node = section->read(tableAt + kNode * (1 + set), kNode);
        const qint64 inNode = node.size() == kNode ? qint32(u32(node, 4)) : -1;
        if (inNode <= 0 || inNode > kEntriesPerNode)
            return fail(damaged);
        for (qint64 e = 0; e < inNode; ++e) {
            const qsizetype at = qsizetype(0x10 + e * kEntrySize);
            CompressedStorage::Entry entry;
            entry.virtualOffset = qint64(u64(node, at));
            entry.physicalOffset = qint64(u64(node, at + 8));
            entry.type = quint8(node[at + 16]);
            entry.physicalSize = u32(node, at + 20);
            const bool known = entry.type == CompressedStorage::Stored || entry.type == CompressedStorage::Zeros
                            || entry.type == CompressedStorage::Lz4;
            if (!known)
                return fail(QObject::tr("This game's data uses a compression the app cannot read (%1).").arg(entry.type));
            if ((!entries.empty() && entry.virtualOffset <= entries.back().virtualOffset)
                || entry.virtualOffset >= virtualSize || entry.physicalOffset < 0)
                return fail(damaged);
            entries.push_back(entry);
        }
    }
    if (qint64(entries.size()) != count || entries.front().virtualOffset != 0)
        return fail(damaged);
    for (size_t i = 0; i < entries.size(); ++i) {
        const CompressedStorage::Entry &e = entries[i];
        const qint64 next = i + 1 < entries.size() ? entries[i + 1].virtualOffset : virtualSize;
        const qint64 length = next - e.virtualOffset;
        const qint64 stored = e.type == CompressedStorage::Stored ? length
                            : e.type == CompressedStorage::Lz4    ? e.physicalSize
                                                                  : 0;
        if ((e.type == CompressedStorage::Lz4 && length > kMaxBlock) || e.physicalOffset > section->size() - stored)
            return fail(damaged);
    }
    return std::make_shared<CompressedStorage>(section, std::move(entries), virtualSize);
}

}

StoragePtr subStorage(const StoragePtr &base, qint64 offset, qint64 size)
{
    if (!base || offset < 0 || size < 0 || offset > base->size() - size)
        return nullptr;
    return std::make_shared<SubStorage>(base, offset, size);
}

std::optional<std::vector<PartitionEntry>> readPartition(const StoragePtr &storage, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<PartitionEntry>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!storage || storage->size() < 16)
        return fail(QObject::tr("The partition is cut off."));
    const QByteArray head = storage->read(0, 16);
    const bool hfs = head.startsWith("HFS0");
    if (!hfs && !head.startsWith("PFS0"))
        return fail(QObject::tr("Not a PFS0 or HFS0 partition."));
    const quint32 count = u32(head, 4);
    const quint32 strings = u32(head, 8);
    const qint64 entrySize = hfs ? 0x40 : 0x18;
    const qint64 headerSize = 16 + qint64(count) * entrySize + strings;
    if (count > kMaxPartitionFiles || headerSize > kMaxPartitionHeader || headerSize > storage->size())
        return fail(QObject::tr("The partition header is damaged."));
    const QByteArray header = storage->read(0, headerSize);
    if (header.size() != headerSize)
        return fail(QObject::tr("The partition header is cut off."));
    const qint64 stringsAt = 16 + qint64(count) * entrySize;
    std::vector<PartitionEntry> out;
    for (quint32 i = 0; i < count; ++i) {
        const qint64 at = 16 + qint64(i) * entrySize;
        const quint64 offset = u64(header, at), size = u64(header, at + 8);
        const quint32 nameAt = u32(header, at + 16);
        if (nameAt >= strings)
            return fail(QObject::tr("The partition header is damaged."));
        const qsizetype end = header.indexOf('\0', stringsAt + nameAt);
        PartitionEntry e;
        e.name = QString::fromUtf8(header.constData() + stringsAt + nameAt,
                                   (end < 0 ? header.size() : end) - (stringsAt + nameAt));
        if (offset > quint64(storage->size()) || size > quint64(storage->size()) - quint64(headerSize)
            || offset > quint64(storage->size()) - quint64(headerSize) - size)
            return fail(QObject::tr("A file in the partition points outside it."));
        e.offset = headerSize + qint64(offset);
        e.size = qint64(size);
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<std::vector<RomfsFile>> listRomfs(const StoragePtr &romfs, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<RomfsFile>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!romfs || romfs->size() < 0x50)
        return fail(QObject::tr("The RomFS is cut off."));
    const QByteArray header = romfs->read(0, 0x50);
    if (header.size() != 0x50 || u64(header, 0) != 0x50)
        return fail(QObject::tr("The RomFS header is damaged (are the keys right?)."));
    const quint64 dirsAt = u64(header, 0x18), dirsSize = u64(header, 0x20);
    const quint64 filesAt = u64(header, 0x38), filesSize = u64(header, 0x40);
    const quint64 dataAt = u64(header, 0x48);
    const quint64 total = quint64(romfs->size());
    if (dirsSize > quint64(kMaxRomfsTable) || filesSize > quint64(kMaxRomfsTable) || dirsAt > total - dirsSize
        || filesAt > total - filesSize || dataAt > total)
        return fail(QObject::tr("The RomFS header is damaged."));
    const QByteArray dirs = romfs->read(qint64(dirsAt), qint64(dirsSize));
    const QByteArray files = romfs->read(qint64(filesAt), qint64(filesSize));
    if (qint64(dirs.size()) != qint64(dirsSize) || qint64(files.size()) != qint64(filesSize))
        return fail(QObject::tr("The RomFS tables are cut off."));

    auto name = [](const QByteArray &table, quint32 at, int fixed) -> std::optional<QString> {
        if (qint64(at) + fixed > table.size())
            return std::nullopt;
        const quint32 length = u32(table, at + fixed - 4);
        if (qint64(at) + fixed + length > table.size())
            return std::nullopt;
        return QString::fromUtf8(table.constData() + at + fixed, length);
    };

    std::vector<RomfsFile> out;
    struct Pending { quint32 dir; QString prefix; };
    std::vector<Pending> stack{{0, QString()}};
    QSet<quint32> seenDirs, seenFiles;
    while (!stack.empty()) {
        const Pending p = stack.back();
        stack.pop_back();
        if (seenDirs.contains(p.dir) || qint64(p.dir) + 0x18 > dirs.size())
            return fail(QObject::tr("The RomFS directory table is damaged."));
        seenDirs.insert(p.dir);
        for (quint32 f = u32(dirs, p.dir + 12); f != kNone; f = u32(files, f + 4)) {
            const std::optional<QString> fileName = name(files, f, 0x20);
            if (!fileName || seenFiles.contains(f))
                return fail(QObject::tr("The RomFS file table is damaged."));
            seenFiles.insert(f);
            const quint64 offset = u64(files, f + 8), size = u64(files, f + 16);
            if (offset > total - dataAt || size > total - dataAt - offset)
                return fail(QObject::tr("A RomFS file points outside the RomFS."));
            const QString path = p.prefix + *fileName;
            if (!isSafeRelativePath(path))
                return fail(QObject::tr("The RomFS has a file with an unsafe name: %1").arg(path));
            out.push_back({path, qint64(dataAt + offset), qint64(size)});
        }
        for (quint32 d = u32(dirs, p.dir + 8); d != kNone;) {
            const std::optional<QString> dirName = name(dirs, d, 0x18);
            if (!dirName)
                return fail(QObject::tr("The RomFS directory table is damaged."));
            stack.push_back({d, p.prefix + *dirName + u'/'});
            d = u32(dirs, d + 4);
            if (stack.size() > size_t(kMaxPartitionFiles) * 16)
                return fail(QObject::tr("The RomFS directory table is damaged."));
        }
    }
    return out;
}

std::optional<SwitchNca> SwitchNca::open(const StoragePtr &storage, const SwitchKeys &keys, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<SwitchNca> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!storage || storage->size() < 0xC00)
        return fail(QObject::tr("The NCA is cut off."));
    const QByteArray raw = storage->read(0, 0xC00);
    SwitchNca nca;
    nca.m_storage = storage;
    const QByteArray magic = raw.mid(0x200, 4);
    if (magic == "NCA3" || magic == "NCA2") {
        nca.m_header = raw;
    } else {
        QByteArray header = crypto::xtsDecrypt(keys.headerKey(), raw.left(0x400), 0, 0x200);
        const QByteArray decryptedMagic = header.mid(0x200, 4);
        if (decryptedMagic == "NCA3") {
            header = crypto::xtsDecrypt(keys.headerKey(), raw, 0, 0x200);
        } else if (decryptedMagic == "NCA2") {
            for (int i = 0; i < 4; ++i)
                header += crypto::xtsDecrypt(keys.headerKey(), raw.mid(0x400 + 0x200 * i, 0x200), 0, 0x200);
        } else if (decryptedMagic == "NCA0") {
            return fail(QObject::tr("This is an old NCA0 file, which the app cannot read."));
        } else {
            return fail(QObject::tr("The NCA header cannot be read. Are the keys (prod.keys) from your own console "
                                    "and up to date?"));
        }
        nca.m_header = header;
    }
    const QByteArray &h = nca.m_header;
    nca.m_titleId = u64(h, 0x210);
    int generation = std::max(int(uchar(h[0x206])), int(uchar(h[0x220])));
    if (generation > 0)
        --generation;
    nca.m_generation = generation;
    const QByteArray rightsId = h.mid(0x230, 16);
    if (storage->decryptedSections()) {
        nca.m_hasGenerationKeys = true;
    } else if (rightsId != QByteArray(16, '\0')) {
        const QByteArray titlekek = keys.titlekek(generation);
        const QByteArray encrypted = keys.titleKey(rightsId);
        nca.m_hasGenerationKeys = !titlekek.isEmpty();
        if (titlekek.isEmpty())
            nca.m_keyProblem = QObject::tr("The key file has no titlekek for key generation %1 (the game needs newer keys).")
                                   .arg(generation);
        else if (encrypted.isEmpty())
            nca.m_keyProblem = QObject::tr("No ticket or title key was found for this content.");
        else
            nca.m_key = crypto::ecbDecrypt(titlekek, encrypted);
    } else {
        const QByteArray areaKey = keys.keyAreaKey(generation, uchar(h[0x207]));
        nca.m_hasGenerationKeys = !areaKey.isEmpty();
        if (areaKey.isEmpty())
            nca.m_keyProblem = QObject::tr("The key file has no key area key for key generation %1 (the game needs newer "
                                           "keys).").arg(generation);
        else
            nca.m_key = crypto::ecbDecrypt(areaKey, h.mid(0x300, 0x40)).mid(0x20, 0x10);
    }
    return nca;
}

bool SwitchNca::hasSection(int section) const
{
    if (section < 0 || section > 3)
        return false;
    return u32(m_header, 0x240 + 0x10 * section) != 0;
}

bool SwitchNca::isPatchSection(int section) const
{
    const int crypt = hasSection(section) ? uchar(fsHeader(section)[4]) : 0;
    return crypt == 4 || crypt == 6;
}

bool SwitchNca::isRomfsSection(int section) const
{
    return hasSection(section) && uchar(fsHeader(section)[3]) == 3;
}

bool SwitchNca::isPartitionSection(int section) const
{
    return hasSection(section) && uchar(fsHeader(section)[3]) == 2;
}

StoragePtr SwitchNca::sectionData(int section, QString *error) const
{
    auto fail = [error](const QString &message) -> StoragePtr {
        if (error)
            *error = message;
        return nullptr;
    };
    if (!hasSection(section))
        return fail(QObject::tr("The NCA has no section %1.").arg(section));
    const qint64 start = qint64(u32(m_header, 0x240 + 0x10 * section)) * 0x200;
    const qint64 end = qint64(u32(m_header, 0x244 + 0x10 * section)) * 0x200;
    if (end <= start || end > m_storage->size())
        return fail(QObject::tr("The NCA section points outside the file."));
    const QByteArray fs = fsHeader(section);
    const int crypt = uchar(fs[4]);
    if (crypt == 1)
        return subStorage(m_storage, start, end - start);
    if (crypt == 4 || crypt == 6)
        return fail(QObject::tr("This section is an update patch; applying updates is not supported yet."));
    if (crypt != 3 && crypt != 5)
        return fail(QObject::tr("This NCA section uses an encryption the app cannot read (%1).").arg(crypt));
    if (m_storage->decryptedSections())
        return subStorage(m_storage, start, end - start);
    if (!m_keyProblem.isEmpty())
        return fail(m_keyProblem);
    QByteArray upper(8, '\0');
    for (int j = 0; j < 8; ++j)
        upper[j] = fs[0x140 + 7 - j];
    return std::make_shared<CtrStorage>(m_storage, start, end - start, m_key, upper);
}

StoragePtr SwitchNca::partition(int section, QString *error) const
{
    StoragePtr data = sectionData(section, error);
    if (!data)
        return nullptr;
    const QByteArray fs = fsHeader(section);
    const qint64 offset = qint64(u64(fs, 8 + 0x38)), size = qint64(u64(fs, 8 + 0x40));
    StoragePtr out = subStorage(data, offset, size);
    if (!out && error)
        *error = QObject::tr("The NCA partition points outside its section.");
    return out;
}

StoragePtr SwitchNca::romfs(int section, QString *error) const
{
    StoragePtr data = sectionData(section, error);
    if (!data)
        return nullptr;
    const QByteArray fs = fsHeader(section);
    if (fs.mid(8, 4) != "IVFC") {
        if (error)
            *error = QObject::tr("The RomFS section has no IVFC header.");
        return nullptr;
    }
    const qint64 offset = qint64(u64(fs, 8 + 0x10 + 5 * 0x18)), size = qint64(u64(fs, 8 + 0x18 + 5 * 0x18));
    StoragePtr out = subStorage(data, offset, size);
    if (!out) {
        if (error)
            *error = QObject::tr("The RomFS points outside its section.");
        return nullptr;
    }
    if (u64(fs, 0x178) != 0 && u64(fs, 0x180) != 0)
        return compressedStorage(out, fs.mid(0x178, 0x28), error);
    return out;
}

bool SwitchNca::verifyRomfs(int section, const std::function<bool(qint64 done, qint64 total)> &progress,
                            QString *error) const
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    StoragePtr data = sectionData(section, error);
    if (!data)
        return false;
    const QByteArray fs = fsHeader(section);
    if (fs.mid(8, 4) != "IVFC")
        return fail(QObject::tr("The RomFS section has no IVFC header."));
    const qsizetype level4 = 8 + 0x10 + 4 * 0x18, level5 = 8 + 0x10 + 5 * 0x18;
    const qint64 hashesAt = qint64(u64(fs, level4)), hashesSize = qint64(u64(fs, level4 + 8));
    const qint64 dataAt = qint64(u64(fs, level5)), dataSize = qint64(u64(fs, level5 + 8));
    const quint32 shift = u32(fs, level5 + 16);
    if (shift < 9 || shift > 24)
        return fail(QObject::tr("The RomFS hash tree is damaged."));
    const qint64 block = qint64(1) << shift;
    const qint64 blocks = (dataSize + block - 1) / block;
    if (blocks * 32 > hashesSize || hashesAt < 0 || dataAt < 0 || hashesAt > data->size() - hashesSize
        || dataAt > data->size() - dataSize)
        return fail(QObject::tr("The RomFS hash tree is damaged."));
    QByteArray buffer(block, Qt::Uninitialized);
    const qint64 kHashChunk = blocks * 32 <= kMaxRomfsTable ? std::max<qint64>(blocks, 1) : 4096;
    QByteArray hashes;
    for (qint64 i = 0; i < blocks; ++i) {
        if (i % kHashChunk == 0) {
            hashes = data->read(hashesAt + i * 32, std::min(kHashChunk, blocks - i) * 32);
            if (hashes.isEmpty())
                return fail(QObject::tr("Cannot read the RomFS hash tree."));
        }
        const qint64 length = std::min(block, dataSize - i * block);
        if (length < block)
            buffer.fill('\0');
        if (!data->read(dataAt + i * block, buffer.data(), length))
            return fail(QObject::tr("Cannot read the RomFS."));
        if (QCryptographicHash::hash(buffer, QCryptographicHash::Sha256) != hashes.mid((i % kHashChunk) * 32, 32))
            return fail(QObject::tr("RomFS block %1 does not match its hash: the file is damaged or the keys are wrong.")
                            .arg(i));
        if (progress && !progress((i + 1) * block, blocks * block))
            return fail(QObject::tr("Canceled."));
    }
    return true;
}
