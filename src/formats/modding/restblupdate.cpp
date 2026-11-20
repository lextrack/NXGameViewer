#include "modding/restblupdate.h"

#include "archives/compress.h"
#include "archives/sarcwriter.h"
#include "data/restblreader.h"

#include <QHash>
#include <QObject>
#include <QRegularExpression>

#include <algorithm>

namespace {

constexpr quint32 kGrowthAlignment = 0x20;

std::optional<quint32> valueOf(const Restbl &table, const QString &path)
{
    for (const auto &[name, size] : table.named)
        if (name == path)
            return size;
    const quint32 hash = restblPathHash(path);
    for (const auto &[h, size] : table.hashed)
        if (h == hash)
            return size;
    return std::nullopt;
}

bool setValue(Restbl &table, const QString &path, quint32 value)
{
    for (auto &[name, size] : table.named)
        if (name == path) {
            size = value;
            return true;
        }
    const quint32 hash = restblPathHash(path);
    for (auto &[h, size] : table.hashed)
        if (h == hash) {
            size = value;
            return true;
        }
    return false;
}

}

QString restblKey(const QString &romfsPath)
{
    QString key = QString(romfsPath).replace(u'\\', u'/');
    while (key.startsWith(u'/'))
        key.remove(0, 1);
    if (key.endsWith(QLatin1String(".zs"), Qt::CaseInsensitive))
        key.chop(3);
    return key;
}

bool isRestblPath(const QString &romfsPath)
{
    const QString key = restblKey(romfsPath);
    return key.contains(QLatin1String(".rsizetable"), Qt::CaseInsensitive)
        || key.contains(QLatin1String(".srsizetable"), Qt::CaseInsensitive);
}

std::optional<std::vector<UnpackedSize>> unpackedSizes(const QByteArray &file, const QString &gamePath,
                                                       const QStringList &entries, QString *error)
{
    std::vector<UnpackedSize> sizes;
    QByteArray level = file;
    QString path = gamePath;
    for (qsizetype i = 0;; ++i) {
        std::optional<QByteArray> plain = unpackAll(level, nullptr, error);
        if (!plain)
            return std::nullopt;
        sizes.push_back({restblKey(path), plain->size()});
        if (i == entries.size())
            return sizes;
        const std::optional<SarcContents> contents = readSarc(*plain, error);
        if (!contents)
            return std::nullopt;
        const SarcEntry *entry = nullptr;
        for (const SarcEntry &e : contents->entries)
            if (!entry && sarcEntryName(e) == entries[i])
                entry = &e;
        if (!entry) {
            if (error)
                *error = QObject::tr("%1 is not in %2.").arg(entries[i], path);
            return std::nullopt;
        }
        level = entry->data;
        path = entries[i];
    }
}

std::optional<RestblUpdate> raiseRestbl(const QByteArray &gameTable, const QByteArray &baseTable,
                                        const std::vector<UnpackedSize> &before, const std::vector<UnpackedSize> &after,
                                        QString *error)
{
    const std::optional<QByteArray> gamePlain = unpackAll(gameTable, nullptr, error);
    if (!gamePlain)
        return std::nullopt;
    const std::optional<Restbl> game = parseRestbl(*gamePlain, error);
    if (!game)
        return std::nullopt;
    std::vector<CompressionLayer> layers;
    const std::optional<QByteArray> basePlain = unpackAll(baseTable, &layers, error);
    if (!basePlain)
        return std::nullopt;
    std::optional<Restbl> base = parseRestbl(*basePlain, error);
    if (!base)
        return std::nullopt;

    QHash<QString, qint64> oldSizes;
    for (const UnpackedSize &s : before)
        oldSizes.insert(s.path, s.size);
    RestblUpdate update;
    for (const UnpackedSize &s : after) {
        const auto old = oldSizes.constFind(s.path);
        if (old == oldSizes.cend() || s.size <= *old)
            continue;
        const std::optional<quint32> listed = valueOf(*game, s.path);
        if (!listed) {
            update.unlisted << s.path;
            continue;
        }
        const qint64 growth = (s.size - *old + kGrowthAlignment - 1) / kGrowthAlignment * kGrowthAlignment;
        const qint64 wanted = qint64(*listed) + growth;
        if (wanted > qint64(0xFFFFFFFF)) {
            if (error)
                *error = QObject::tr("%1 would need more memory than the resource size table can record.").arg(s.path);
            return std::nullopt;
        }
        const quint32 current = valueOf(*base, s.path).value_or(*listed);
        if (current >= quint32(wanted))
            continue;
        if (!setValue(*base, s.path, quint32(wanted))) {
            update.unlisted << s.path;
            continue;
        }
        update.raised.push_back({s.path, current, quint32(wanted)});
    }
    if (update.raised.empty())
        return update;
    std::optional<QByteArray> packed = packAll(writeRestbl(*base), layers, error);
    if (!packed)
        return std::nullopt;
    update.file = std::move(*packed);
    return update;
}

QString newestRestblName(const QStringList &fileNames)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^ResourceSizeTable\.Product\.(\d+)\..*rsizetable(\.zs)?$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    QString best;
    qint64 bestVersion = -1;
    for (const QString &name : fileNames) {
        const QRegularExpressionMatch m = pattern.match(name);
        if (!m.hasMatch())
            continue;
        const qint64 version = m.captured(1).toLongLong();
        if (version > bestVersion || (version == bestVersion && name.endsWith(QLatin1String(".zs")))) {
            best = name;
            bestVersion = version;
        }
    }
    return best;
}
