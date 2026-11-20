#include "archives/zstddictionaries.h"

#include "archives/decompress.h"
#include "archives/sarcreader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSet>

#include <zstd.h>

#include <map>
#include <memory>

namespace {

constexpr int kMaxLevels = 12;
constexpr qint64 kMaxPack = qint64{64} * 1024 * 1024;

struct FreeDDict
{
    void operator()(ZSTD_DDict *d) const { ZSTD_freeDDict(d); }
};

QMutex mutex;
struct Dictionary
{
    QByteArray bytes;
    std::unique_ptr<ZSTD_DDict, FreeDDict> decoder;
};

std::map<unsigned, Dictionary> dictionaries;
QSet<QString> loadedSources;
QStringList roots;

const QString kPackPath = QStringLiteral("Pack/ZsDic.pack.zs");

}

int addZstdDictionaries(const QByteArray &zsDicPack, const QString &source)
{
    {
        const QMutexLocker lock(&mutex);
        if (loadedSources.contains(source))
            return 0;
    }
    const std::optional<QByteArray> pack = decompress(zsDicPack, nullptr);
    if (!pack)
        return 0;
    QString error;
    const std::unique_ptr<Document> doc = SarcReader().readBytes(*pack, QStringLiteral("ZsDic.pack"), &error);
    if (!doc || doc->tables.empty() || !doc->tables.front()->archive())
        return 0;
    const std::shared_ptr<const Archive> archive = doc->tables.front()->archive();
    int added = 0;
    const QMutexLocker lock(&mutex);
    loadedSources.insert(source);
    for (size_t i = 0; i < archive->entries.size(); ++i) {
        if (!archive->entries[i].name.endsWith(QLatin1String(".zsdic"), Qt::CaseInsensitive))
            continue;
        const QByteArray bytes = archive->entryData(i);
        const unsigned id = ZSTD_getDictID_fromDict(bytes.constData(), size_t(bytes.size()));
        if (id == 0 || dictionaries.count(id))
            continue;
        ZSTD_DDict *dict = ZSTD_createDDict(bytes.constData(), size_t(bytes.size()));
        if (!dict)
            continue;
        dictionaries.emplace(id, Dictionary{bytes, std::unique_ptr<ZSTD_DDict, FreeDDict>(dict)});
        ++added;
    }
    return added;
}

bool loadZstdDictionariesNear(const QString &path)
{
    QDir dir = QFileInfo(path).absoluteDir();
    for (int level = 0; level < kMaxLevels; ++level) {
        for (const QString &candidate : {dir.filePath(kPackPath), dir.filePath(QStringLiteral("romfs/") + kPackPath)}) {
            const QFileInfo info(candidate);
            if (!info.isFile() || info.size() > kMaxPack)
                continue;
            const QString key = info.absoluteFilePath();
            {
                const QMutexLocker lock(&mutex);
                const QString root = QFileInfo(info.absolutePath()).absolutePath();
                if (!roots.contains(root))
                    roots << root;
                if (loadedSources.contains(key))
                    return true;
            }
            QFile file(key);
            if (file.open(QIODevice::ReadOnly)) {
                addZstdDictionaries(file.readAll(), key);
                return true;
            }
        }
        if (!dir.cdUp())
            break;
    }
    return false;
}

bool loadZstdDictionariesFrom(const Archive &archive)
{
    bool found = false;
    for (size_t i = 0; i < archive.entries.size(); ++i) {
        const ArchiveEntry &e = archive.entries[i];
        if (!e.name.endsWith(kPackPath, Qt::CaseInsensitive) || e.size > kMaxPack)
            continue;
        const QString key = QStringLiteral("archive:%1:%2:%3")
                                .arg(quintptr(&archive))
                                .arg(archive.entries.size())
                                .arg(e.name);
        addZstdDictionaries(archive.entryData(i), key);
        found = true;
    }
    return found;
}

unsigned zstdDictionaryId(QByteArrayView data)
{
    if (data.size() < 4 || detectCompression(data.left(4)) != Compression::Zstd)
        return 0;
    return ZSTD_getDictID_fromFrame(data.data(), size_t(data.size()));
}

const ZSTD_DDict_s *zstdDictionary(unsigned id)
{
    const QMutexLocker lock(&mutex);
    const auto it = dictionaries.find(id);
    return it == dictionaries.end() ? nullptr : it->second.decoder.get();
}

QByteArray zstdDictionaryBytes(unsigned id)
{
    const QMutexLocker lock(&mutex);
    const auto it = dictionaries.find(id);
    return it == dictionaries.end() ? QByteArray() : it->second.bytes;
}

bool needsUnknownZstdDictionary(QByteArrayView data)
{
    const unsigned id = zstdDictionaryId(data);
    return id != 0 && !zstdDictionary(id);
}

QStringList zstdDictionaryRoots()
{
    const QMutexLocker lock(&mutex);
    return roots;
}
