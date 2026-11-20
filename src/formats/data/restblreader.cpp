#include "data/restblreader.h"

#include "archives/zstddictionaries.h"
#include "core/simpletable.h"
#include "data/bcsvreader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QtEndian>

namespace {

constexpr qsizetype kHeaderSize = 0x16;
constexpr quint32 kMaxNameSize = 0x1000;
constexpr int kMaxScannedFiles = 2'000'000;

QMutex namesMutex;
QHash<QString, QHash<quint32, QString>> namesByRoot;

std::optional<Restbl> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

}

std::optional<Restbl> parseRestbl(const QByteArray &data, QString *error)
{
    if (data.size() < kHeaderSize || !data.startsWith("RESTBL"))
        return fail(error, QObject::tr("Not a resource size table (no \"RESTBL\" at the start)."));
    const char *p = data.constData();
    Restbl table;
    table.version = qFromLittleEndian<quint32>(p + 6);
    table.nameSize = qFromLittleEndian<quint32>(p + 10);
    const quint32 hashedCount = qFromLittleEndian<quint32>(p + 14);
    const quint32 namedCount = qFromLittleEndian<quint32>(p + 18);
    if (table.nameSize == 0 || table.nameSize > kMaxNameSize)
        return fail(error, QObject::tr("The resource size table has an unexpected name length (%1).").arg(table.nameSize));
    const qint64 needed = kHeaderSize + qint64(hashedCount) * 8 + qint64(namedCount) * (qint64(table.nameSize) + 4);
    if (needed > data.size())
        return fail(error, QObject::tr("The resource size table is cut short."));
    table.hashed.reserve(hashedCount);
    qsizetype at = kHeaderSize;
    for (quint32 i = 0; i < hashedCount; ++i, at += 8)
        table.hashed.emplace_back(qFromLittleEndian<quint32>(p + at), qFromLittleEndian<quint32>(p + at + 4));
    for (quint32 i = 0; i < namedCount; ++i, at += table.nameSize + 4) {
        const QByteArray raw(p + at, qsizetype(table.nameSize));
        const qsizetype end = raw.indexOf('\0');
        table.named.emplace_back(QString::fromUtf8(raw.left(end < 0 ? raw.size() : end)),
                                 qFromLittleEndian<quint32>(p + at + table.nameSize));
    }
    return table;
}

QByteArray writeRestbl(const Restbl &table)
{
    QByteArray out("RESTBL", 6);
    auto u32 = [&out](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    u32(table.version);
    u32(table.nameSize);
    u32(quint32(table.hashed.size()));
    u32(quint32(table.named.size()));
    for (const auto &[hash, size] : table.hashed) {
        u32(hash);
        u32(size);
    }
    for (const auto &[name, size] : table.named) {
        QByteArray raw = name.toUtf8().left(qsizetype(table.nameSize));
        raw.resize(qsizetype(table.nameSize), '\0');
        out += raw;
        u32(size);
    }
    return out;
}

quint32 restblPathHash(const QString &romfsPath)
{
    return BcsvReader::crc32(romfsPath.toUtf8());
}

QHash<quint32, QString> restblNamesUnder(const QString &romfsRoot)
{
    const QString root = QDir(romfsRoot).absolutePath();
    {
        const QMutexLocker lock(&namesMutex);
        const auto it = namesByRoot.constFind(root);
        if (it != namesByRoot.cend())
            return *it;
    }
    QHash<quint32, QString> names;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    for (int n = 0; n < kMaxScannedFiles && it.hasNext(); ++n) {
        QString path = QDir(root).relativeFilePath(it.next());
        if (path.endsWith(QLatin1String(".zs")))
            path.chop(3);
        names.insert(restblPathHash(path), path);
    }
    const QMutexLocker lock(&namesMutex);
    namesByRoot.insert(root, names);
    return names;
}

int RestblReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("RESTBL") ? 100 : 0;
}

std::unique_ptr<Document> RestblReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 64 MB.");
        return nullptr;
    }
    loadZstdDictionariesNear(path);
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> RestblReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    const std::optional<Restbl> table = parseRestbl(data, error);
    if (!table)
        return nullptr;
    QHash<quint32, QString> names;
    for (const QString &root : zstdDictionaryRoots())
        names.insert(restblNamesUnder(root));
    std::vector<SimpleTable::Row> rows;
    rows.reserve(table->hashed.size() + table->named.size());
    int known = 0;
    for (const auto &[hash, size] : table->hashed) {
        const auto it = names.constFind(hash);
        if (it != names.cend())
            ++known;
        rows.push_back({it != names.cend() ? *it : QString(), qint64(hash), qint64(size)});
    }
    for (const auto &[name, size] : table->named)
        rows.push_back({name, qint64(restblPathHash(name)), qint64(size)});
    const int hashedCount = int(table->hashed.size());
    auto result = std::make_shared<SimpleTable>(
        QObject::tr("Resource sizes"),
        std::vector<SimpleColumn>{{QObject::tr("file")},
                                  {QObject::tr("hash"), true, QObject::tr("CRC32 of the romfs path without .zs")},
                                  {QObject::tr("size"), false,
                                   QObject::tr("Memory the game sets aside for the file once it is decompressed")}},
        std::move(rows));
    QString description = QObject::tr("Version %1: %n file(s) by hash", nullptr, hashedCount).arg(table->version);
    description += QObject::tr(", %n by name", nullptr, int(table->named.size()));
    if (hashedCount > 0)
        description += QObject::tr("; %1 of the hashes match files in the game folder").arg(known);
    result->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(result));
    return doc;
}
