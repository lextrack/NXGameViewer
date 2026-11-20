#include "archives/rpf6reader.h"

#include "archives/decompress.h"
#include "core/bytes.h"
#include "core/crypto.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <zlib.h>

#include <vector>

namespace {

constexpr int kRounds = 16;
constexpr quint32 kMaxEntries = 10'000'000;
constexpr qint64 kMaxUnpacked = qint64{1024} * 1024 * 1024;

QMutex keyMutex;
QList<QByteArray> storedKeys;


struct Entry
{
    QString path;
    qint64 offset = 0;
    qint64 stored = 0;
    qint64 size = -1;
    bool compressed = false;
    bool resource = false;
    quint32 resourceFlags = 0;
};

struct Source
{
    StoredReader read;
    QByteArray key;
    std::vector<Entry> entries;
};

QByteArray rpf6Apply(const QByteArray &key, QByteArrayView data, bool encrypt)
{
    QByteArray out = data.toByteArray();
    if (key.size() != 32)
        return out;
    const qsizetype whole = out.size() / 16 * 16;
    if (whole > 0 && !crypto::ecbApplyInPlace(key, out.data(), whole, kRounds, encrypt))
        return data.toByteArray();
    return out;
}

std::optional<QByteArray> inflate(QByteArrayView data, bool raw, qint64 expected)
{
    struct Stream
    {
        z_stream z{};
        bool open = false;
        ~Stream()
        {
            if (open)
                inflateEnd(&z);
        }
    } stream;
    z_stream &z = stream.z;
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK)
        return std::nullopt;
    stream.open = true;
    QByteArray out;
    out.reserve(qsizetype(expected > 0 && expected <= kMaxUnpacked ? expected : data.size() * 4));
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    z.avail_in = uInt(data.size());
    char chunk[65536];
    int result = Z_OK;
    while (result == Z_OK) {
        z.next_out = reinterpret_cast<Bytef *>(chunk);
        z.avail_out = sizeof chunk;
        result = ::inflate(&z, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
            break;
        out.append(chunk, qsizetype(sizeof chunk - z.avail_out));
        if (out.size() > kMaxUnpacked) {
            result = Z_MEM_ERROR;
            break;
        }
        if (result == Z_OK && z.avail_in == 0 && z.avail_out != 0)
            break;
    }
    if (result != Z_STREAM_END)
        return std::nullopt;
    return out;
}

bool looksLikeZlib(QByteArrayView d)
{
    if (d.size() < 2)
        return false;
    const uchar a = uchar(d[0]), b = uchar(d[1]);
    return (a & 0x0F) == 8 && (a >> 4) <= 7 && ((a << 8) | b) % 31 == 0;
}

std::optional<QByteArray> unpack(const QByteArray &d)
{
    if (detectCompression(d) == Compression::Zstd)
        return decompress(d, nullptr);
    if (looksLikeZlib(d))
        return inflate(d, false, -1);
    return std::nullopt;
}

std::optional<QByteArray> readEntry(const Source &source, const Entry &e)
{
    QByteArray d = source.read(e.offset, e.stored);
    if (d.size() != e.stored)
        return std::nullopt;
    QByteArray header;
    if (e.resource) {
        const bool rsc5 = d.startsWith(QByteArrayView("RSC\x05", 4)), rsc85 = d.startsWith(QByteArrayView("RSC\x85", 4));
        if ((rsc5 || rsc85) && d.size() >= (rsc85 ? 16 : 12)
            && qFromLittleEndian<quint32>(d.constData() + 8) == e.resourceFlags) {
            header = d.left(rsc85 ? 16 : 12);
            d = d.mid(rsc85 ? 16 : 12);
        }
    }
    if (!e.compressed && !e.resource)
        return d;
    std::optional<QByteArray> u = unpack(d);
    if (!u && e.compressed && e.size >= 0) {
        u = inflate(d, true, e.size);
        if (u && u->size() != e.size)
            u.reset();
    }
    if (!u && !source.key.isEmpty()) {
        QByteArray plain = rpf6Decrypt(source.key, d);
        u = unpack(plain);
    }
    if (!u) {
        if (!e.compressed)
            return header + d;
        return std::nullopt;
    }
    if (e.size >= 0 && u->size() != e.size)
        return std::nullopt;
    return header + *u;
}

class ArchiveTable : public SimpleTable
{
public:
    ArchiveTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Files"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("stored")}, {QObject::tr("kind")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

std::unique_ptr<Document> parse(const StoredReader &readAt, qint64 fileSize, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    const QByteArray head = readAt(0, std::min<qint64>(fileSize, 16));
    if (head.size() < 16 || !head.startsWith("RPF6"))
        return fail(QObject::tr("Not an RPF6 archive (no \"RPF6\" at the start)."));
    const quint32 count = be32(head, 4);
    const qint64 namesAt = qint64(be32(head, 8)) * 8;
    const quint32 tag = be32(head, 12);
    if (count == 0 || count > kMaxEntries || 16 + qint64(count) * 20 > fileSize)
        return fail(QObject::tr("The RPF6 entry table is damaged."));
    const qint64 tableSize = (qint64(count) * 20 + 15) / 16 * 16;
    QByteArray table = readAt(16, std::min(tableSize, fileSize - 16));
    if (table.size() < qint64(count) * 20)
        return fail(QObject::tr("The RPF6 entry table is cut off."));

    auto source = std::make_shared<Source>();
    source->read = readAt;
    const bool encrypted = tag == Rpf6Reader::kEncrypted;
    if (encrypted) {
        for (const QByteArray &key : Rpf6Reader::keys())
            if (rpf6IsRootEntry(rpf6Decrypt(key, QByteArrayView(table).first(16)))) {
                source->key = key;
                break;
            }
        if (source->key.isEmpty())
            return fail(QObject::tr("This archive's file list is encrypted. Its key is in the game's executable "
                                    "(the \"main\" file of the game's ExeFS)."));
        table = rpf6Decrypt(source->key, table);
    } else if (tag != 0 || !rpf6IsRootEntry(table)) {
        return fail(QObject::tr("This RPF6 archive uses an encryption the app cannot read (0x%1).").arg(tag, 8, 16, QLatin1Char('0')));
    }

    struct Raw { quint32 hash, a, b, c; };
    std::vector<Raw> raw(count);
    for (quint32 i = 0; i < count; ++i)
        raw[i] = {be32(table, i * 20), be32(table, i * 20 + 4), be32(table, i * 20 + 8), be32(table, i * 20 + 12)};

    QSet<quint32> wanted;
    for (const Raw &r : raw)
        wanted.insert(r.hash);
    QHash<quint32, QString> names;
    if (namesAt >= 16 && namesAt < fileSize) {
        const qint64 length = std::min(fileSize - namesAt, Rpf6Reader::kMaxNameTable);
        QByteArray nameTable = readAt(namesAt, length);
        if (encrypted)
            nameTable = rpf6Decrypt(source->key, nameTable);
        qsizetype start = -1;
        const char *bytes = nameTable.constData();
        for (qsizetype i = 0; i < nameTable.size(); ++i) {
            const uchar c = uchar(bytes[i]);
            if (c >= 0x20 && c <= 0x7E) {
                if (start < 0)
                    start = i;
            } else {
                if (c == 0 && start >= 0 && i - start <= 255) {
                    const QByteArrayView name = QByteArrayView(nameTable).sliced(start, i - start);
                    for (qsizetype from = 0; from < name.size(); ++from)
                        if (const quint32 h = joaat(name.sliced(from)); wanted.contains(h) && !names.contains(h))
                            names.insert(h, QString::fromLatin1(name.sliced(from)));
                }
                start = -1;
            }
        }
    }

    std::vector<bool> seen(count, false);
    QStringList problems;
    struct Pending { quint32 index; QString prefix; int depth; };
    std::vector<Pending> stack{{0, QString(), 0}};
    quint64 pushed = 1;
    while (!stack.empty()) {
        const Pending p = std::move(stack.back());
        stack.pop_back();
        if (p.index >= count || seen[p.index] || p.depth > 64)
            return fail(QObject::tr("The RPF6 directory tree is damaged."));
        seen[p.index] = true;
        const Raw &r = raw[p.index];
        const QString name = names.value(r.hash, QStringLiteral("#%1").arg(r.hash, 8, 16, QLatin1Char('0')));
        if (r.b & 0x80000000u) {
            const quint32 first = r.b & 0x7FFFFFFFu;
            if (first > count || r.c > count - first)
                return fail(QObject::tr("The RPF6 directory tree is damaged."));
            pushed += r.c;
            if (pushed > count)
                return fail(QObject::tr("The RPF6 directory tree is damaged."));
            const QString prefix = p.index == 0 ? QString() : p.prefix + name + u'/';
            for (quint32 j = first + r.c; j > first; --j)
                stack.push_back({j - 1, prefix, p.depth + 1});
            continue;
        }
        Entry e;
        e.path = p.prefix + name;
        e.stored = r.a & 0x7FFFFFFFu;
        e.compressed = r.c & 0x40000000u;
        if (r.c & 0x80000000u) {
            e.resource = true;
            e.resourceFlags = r.c;
            e.offset = qint64(r.b & 0x7FFFFF00u) * 8;
        } else {
            e.offset = qint64(r.b & 0x7FFFFFFFu) * 8;
            e.size = r.c & 0x3FFFFFFFu;
        }
        if (e.offset > fileSize - e.stored) {
            problems << e.path;
            continue;
        }
        source->entries.push_back(std::move(e));
    }

    auto archive = std::make_shared<Archive>();
    std::vector<SimpleTable::Row> rows;
    rows.reserve(source->entries.size());
    for (const Entry &e : source->entries) {
        const qint64 shownSize = e.size >= 0 ? e.size : e.stored;
        archive->entries.push_back({e.path, e.offset, shownSize});
        rows.push_back({e.path, e.size >= 0 ? QVariant(e.size) : QVariant(), e.stored,
                        e.resource ? QObject::tr("resource") : e.compressed ? QObject::tr("compressed") : QObject::tr("stored")});
    }
    archive->loader = [source](size_t index) {
        if (index >= source->entries.size())
            return QByteArray();
        return readEntry(*source, source->entries[index]).value_or(QByteArray());
    };
    auto table_ = std::make_shared<ArchiveTable>(std::move(rows), archive);
    QString description = QObject::tr("%n file(s)", nullptr, int(source->entries.size()));
    if (encrypted)
        description += QObject::tr(", encrypted file list");
    if (!problems.isEmpty())
        description += QObject::tr("; %n entry(s) point outside the file", nullptr, int(problems.size()));
    table_->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table_));
    return doc;
}

}

std::unique_ptr<Document> Rpf6Reader::readStored(const StoredReader &read, qint64 size, const QString &,
                                                 QString *error) const
{
    return parse(read, size, error);
}

QByteArray rpf6Decrypt(const QByteArray &key, QByteArrayView data)
{
    return rpf6Apply(key, data, false);
}

QByteArray rpf6Encrypt(const QByteArray &key, QByteArrayView data)
{
    return rpf6Apply(key, data, true);
}

quint32 joaat(QByteArrayView name)
{
    quint32 h = 0;
    for (char ch : name) {
        uchar c = uchar(ch);
        if (c >= 'A' && c <= 'Z')
            c = uchar(c - 'A' + 'a');
        h += c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

bool rpf6IsRootEntry(QByteArrayView d)
{
    return d.size() >= 16 && be32(d, 0) == 0 && be32(d, 4) == 0 && be32(d, 8) == 0x80000001u && be32(d, 12) > 0;
}

void Rpf6Reader::setKeys(const QList<QByteArray> &keys)
{
    const QMutexLocker lock(&keyMutex);
    storedKeys.clear();
    for (const QByteArray &k : keys)
        if (k.size() == 32 && !storedKeys.contains(k))
            storedKeys << k;
}

QList<QByteArray> Rpf6Reader::keys()
{
    const QMutexLocker lock(&keyMutex);
    return storedKeys;
}

bool Rpf6Reader::needsKey(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(32);
    if (head.size() < 32 || !head.startsWith("RPF6") || be32(head, 12) != kEncrypted)
        return false;
    for (const QByteArray &key : keys())
        if (rpf6IsRootEntry(rpf6Decrypt(key, QByteArrayView(head).sliced(16, 16))))
            return false;
    return true;
}

int Rpf6Reader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("RPF6") ? 100 : 0;
}

std::unique_ptr<Document> Rpf6Reader::read(const QString &path, QString *error) const
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        if (error)
            *error = QObject::tr("The file does not exist.");
        return nullptr;
    }
    const qint64 fileSize = info.size();
    const QDateTime modified = info.lastModified();
    return parse(storedFileReader(path, fileSize, modified), fileSize, error);
}

std::unique_ptr<Document> Rpf6Reader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    return parse(storedBytesReader(data), data.size(), error);
}
