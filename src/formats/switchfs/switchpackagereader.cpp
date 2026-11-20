#include "switchfs/switchpackagereader.h"

#include "core/simpletable.h"
#include "switchfs/switchgame.h"
#include "switchfs/switchkeysearch.h"
#include "switchfs/switchnup.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>
#include <QObject>

#include <cstring>

namespace {

struct CachedKeys
{
    QString path;
    QDateTime modified;
    qint64 size = -1;
    std::optional<SwitchKeys> keys;
    QString error;
};

QMutex keyMutex;
QString configuredKeyFile;
CachedKeys cachedKeys;

std::optional<SwitchKeys> currentKeys(QString *error)
{
    const QMutexLocker lock(&keyMutex);
    if (configuredKeyFile.isEmpty()) {
        if (error)
            *error = QObject::tr("No key file (prod.keys) is set.");
        return std::nullopt;
    }
    const QFileInfo info(configuredKeyFile);
    if (cachedKeys.path != configuredKeyFile || cachedKeys.modified != info.lastModified()
        || cachedKeys.size != info.size()) {
        const SwitchKeySearch search = useSwitchKeyFile(configuredKeyFile, {}, KeySearchPlaces::forSystem({}));
        cachedKeys.path = configuredKeyFile;
        cachedKeys.modified = info.lastModified();
        cachedKeys.size = info.size();
        cachedKeys.keys = search.found ? std::optional<SwitchKeys>(search.found->keys) : std::nullopt;
        cachedKeys.error = search.problem;
    }
    if (!cachedKeys.keys && error)
        *error = cachedKeys.error;
    return cachedKeys.keys;
}

class PackageTable : public SimpleTable
{
public:
    PackageTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Files"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("title")}, {QObject::tr("note")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }

    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

SwitchTitleFile note(const QString &path, const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {path, bytes.size(), [bytes](qint64 offset, char *out, qint64 length) {
                if (offset < 0 || length < 0 || offset > bytes.size() - length)
                    return false;
                std::memcpy(out, bytes.constData() + offset, size_t(length));
                return true;
            }};
}

}

void SwitchPackageReader::setKeyFile(const QString &prodKeys)
{
    const QMutexLocker lock(&keyMutex);
    configuredKeyFile = prodKeys;
}

QString SwitchPackageReader::keyFile()
{
    const QMutexLocker lock(&keyMutex);
    return configuredKeyFile;
}

bool SwitchPackageReader::isPackageName(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return suffix == QLatin1String("nsp") || suffix == QLatin1String("xci") || suffix == QLatin1String("nsz")
           || suffix == QLatin1String("xcz") || suffix == QLatin1String("nup");
}

bool SwitchPackageReader::needsKeys(const QString &path)
{
    return isPackageName(path) && !isNupFile(path) && !currentKeys(nullptr);
}

int SwitchPackageReader::score(const QString &fileName, QByteArrayView) const
{
    return isPackageName(fileName) ? 100 : 0;
}

std::unique_ptr<Document> SwitchPackageReader::read(const QString &path, QString *error) const
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    QString problem;
    const std::optional<SwitchKeys> keys = isNupFile(path) ? std::optional<SwitchKeys>(SwitchKeys()) : currentKeys(&problem);
    if (!keys)
        return fail(QObject::tr("This Switch game is encrypted and needs the keys of your own console. %1").arg(problem));
    const std::vector<SwitchTitle> titles = scanSwitchPackage(path, *keys, &problem);
    if (titles.empty())
        return fail(problem.isEmpty() ? QObject::tr("No game was found in the package.") : problem);

    auto files = std::make_shared<std::vector<SwitchTitleFile>>();
    auto archive = std::make_shared<Archive>();
    std::vector<SimpleTable::Row> rows;
    QStringList unreadable;
    for (const SwitchTitle &t : titles) {
        const QString folder = t.folderName();
        std::optional<std::vector<SwitchTitleFile>> titleFiles;
        QString titleProblem = t.problem;
        if (t.canExtract()) {
            titleFiles = switchTitleFiles(t, *keys, &titleProblem);
            if (titleFiles && titleFiles->empty())
                titleProblem = QObject::tr("The title has no files.");
        }
        if (!titleFiles || titleFiles->empty()) {
            unreadable << QStringLiteral("%1 (%2)").arg(folder, t.typeName());
            files->push_back(note(folder, titleProblem));
            rows.push_back({folder, qint64(files->back().size), t.typeName(), titleProblem});
            continue;
        }
        for (SwitchTitleFile &f : *titleFiles) {
            f.path = folder + u'/' + f.path;
            rows.push_back({f.path, f.size, t.typeName(), QString()});
            files->push_back(std::move(f));
        }
    }
    for (const SwitchTitleFile &f : *files)
        archive->entries.push_back({f.path, 0, f.size});
    archive->loader = [files](size_t index) {
        const SwitchTitleFile &f = files->at(index);
        QByteArray out(f.size, Qt::Uninitialized);
        return f.read(0, out.data(), f.size) ? out : QByteArray();
    };
    archive->stream = [files](size_t index) -> StoredReader {
        const SwitchTitleFile f = files->at(index);
        return [f](qint64 offset, qint64 length) {
            if (offset < 0 || length < 0 || offset > f.size - length)
                return QByteArray();
            QByteArray out(length, Qt::Uninitialized);
            return f.read(offset, out.data(), length) ? out : QByteArray();
        };
    };
    archive->headLoader = [files](size_t index, qsizetype length) {
        const SwitchTitleFile &f = files->at(index);
        if (!f.seeksFast)
            return QByteArray();
        const qint64 n = std::min<qint64>(f.size, length);
        QByteArray out(n, Qt::Uninitialized);
        return f.read(0, out.data(), n) ? out : QByteArray();
    };

    auto table = std::make_shared<PackageTable>(std::move(rows), archive);
    QString description = QObject::tr("%n title(s)", nullptr, int(titles.size())) + QStringLiteral(", ")
                          + QObject::tr("%n file(s)", nullptr, int(files->size() - size_t(unreadable.size())));
    if (!unreadable.isEmpty())
        description += QStringLiteral("; ") + QObject::tr("cannot be read yet: %1").arg(unreadable.join(QStringLiteral(", ")));
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
