#include "modding/modlibrary.h"

#include "core/paths.h"
#include "core/zipwriter.h"
#include "switchfs/switchkeysearch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace {

QString clean(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString child(const QString &parent, const QString &name)
{
    return QDir(parent).filePath(matchExistingCase(parent, name));
}

QStringList subfolders(const QString &folder)
{
    QStringList out;
    const QDir dir(folder);
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
        out << dir.filePath(name);
    return out;
}

void addFolder(QStringList &list, const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return;
    const QString c = clean(QFileInfo(path).absoluteFilePath());
    if (!list.contains(c))
        list << c;
}

bool removePath(const QString &path, bool isDir, bool toTrash, QString *error)
{
    if (toTrash) {
        if (QFile::moveToTrash(path))
            return true;
        if (error)
            *error = QObject::tr("%1 could not be moved to the trash.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const bool ok = isDir ? QDir(path).removeRecursively() : QFile::remove(path);
    if (!ok && error)
        *error = QObject::tr("%1 could not be deleted.").arg(QDir::toNativeSeparators(path));
    return ok;
}

}

bool ModRecord::exists() const
{
    if (folder.isEmpty())
        return false;
    const std::optional<ModLocation> mod = parseModFolder(folder);
    if (mod && layoutHasModName(mod->layout))
        return QFileInfo(modRoot(folder)).isDir();
    return QFileInfo(folder).isDir();
}

QString ModRecord::emulatorName() const
{
    switch (layout) {
    case ModLayout::YuzuFamily: return QObject::tr("yuzu family");
    case ModLayout::Ryujinx: return QObject::tr("Ryujinx");
    case ModLayout::Atmosphere: return QObject::tr("Atmosphère");
    }
    return {};
}

QString modId(const QString &romfs)
{
    return QString::fromLatin1(QCryptographicHash::hash(clean(romfs).toUtf8(), QCryptographicHash::Sha1).toHex().left(8))
        .toUpper();
}

ModRecord modRecord(const QString &romfs, const QString &game)
{
    ModRecord r;
    r.folder = clean(romfs);
    r.id = modId(r.folder);
    r.game = game;
    if (const std::optional<ModLocation> mod = parseModFolder(r.folder)) {
        r.layout = mod->layout;
        r.titleId = mod->titleId;
        r.name = layoutHasModName(mod->layout) ? mod->modName : QObject::tr("Atmosphère");
    } else {
        r.name = QFileInfo(modRoot(r.folder)).fileName();
        r.titleId = titleIdFromPath(r.folder).value_or(QString());
    }
    return r;
}

QString modDisplayName(const ModRecord &record)
{
    const QString game = record.game.isEmpty() ? record.titleId : QStringLiteral("%1 [%2]").arg(record.game, record.titleId);
    return game.isEmpty() ? record.name : QObject::tr("“%1” for %2").arg(record.name, game);
}

QVariantMap modRecordToVariant(const ModRecord &record)
{
    return {{QStringLiteral("id"), record.id},
            {QStringLiteral("name"), record.name},
            {QStringLiteral("titleId"), record.titleId},
            {QStringLiteral("game"), record.game},
            {QStringLiteral("layout"), int(record.layout)},
            {QStringLiteral("folder"), record.folder},
            {QStringLiteral("lastSavedFile"), record.lastSavedFile},
            {QStringLiteral("lastSaved"), record.lastSaved.toString(Qt::ISODate)}};
}

std::optional<ModRecord> modRecordFromVariant(const QVariantMap &map)
{
    ModRecord r;
    r.folder = clean(map.value(QStringLiteral("folder")).toString());
    if (r.folder.isEmpty() || r.folder == QLatin1String("."))
        return std::nullopt;
    r.id = map.value(QStringLiteral("id")).toString();
    if (r.id.isEmpty())
        r.id = modId(r.folder);
    r.name = map.value(QStringLiteral("name")).toString();
    r.titleId = map.value(QStringLiteral("titleId")).toString();
    r.game = map.value(QStringLiteral("game")).toString();
    const int layout = map.value(QStringLiteral("layout")).toInt();
    r.layout = layout >= 0 && layout <= int(ModLayout::Atmosphere) ? ModLayout(layout) : ModLayout::YuzuFamily;
    r.lastSavedFile = map.value(QStringLiteral("lastSavedFile")).toString();
    r.lastSaved = QDateTime::fromString(map.value(QStringLiteral("lastSaved")).toString(), Qt::ISODate);
    return r;
}

void rememberMod(std::vector<ModRecord> &history, ModRecord record)
{
    const auto it = std::find_if(history.begin(), history.end(), [&](const ModRecord &r) { return r.id == record.id; });
    if (it != history.end()) {
        if (record.game.isEmpty())
            record.game = it->game;
        if (record.lastSavedFile.isEmpty()) {
            record.lastSavedFile = it->lastSavedFile;
            record.lastSaved = it->lastSaved;
        }
        history.erase(it);
    }
    history.insert(history.begin(), std::move(record));
    if (history.size() > size_t(kMaxModHistory))
        history.resize(size_t(kMaxModHistory));
}

void forgetMod(std::vector<ModRecord> &history, const QString &id)
{
    std::erase_if(history, [&](const ModRecord &r) { return r.id == id; });
}

void forgetMissingMods(std::vector<ModRecord> &history)
{
    std::erase_if(history, [](const ModRecord &r) { return !r.exists(); });
}

QString gameNameFromPath(const QString &path, const QString &titleId)
{
    if (!isTitleId(titleId))
        return {};
    QStringList ids{titleId.toUpper(), applicationTitleId(titleId)};
    ids.removeDuplicates();
    const QStringList parts = clean(path).split(u'/');
    for (qsizetype i = parts.size() - 1; i >= 0; --i) {
        for (const QString &id : ids) {
            const QRegularExpression pattern(QStringLiteral(R"(^(.*?)\s*\[\s*%1\s*\])").arg(id),
                                             QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch m = pattern.match(parts[i]);
            if (m.hasMatch() && !m.captured(1).trimmed().isEmpty())
                return m.captured(1).trimmed();
        }
    }
    return {};
}

QStringList emulatorDataFolders(const KeySearchPlaces &places)
{
    QStringList out;
    const QStringList flatpaks = places.home.isEmpty() ? QStringList() : subfolders(QDir(places.home).filePath(QStringLiteral(".var/app")));
    for (const QString &family : yuzuFamilyNames())
        for (const QString &name : emulatorNameSpellings(family)) {
            for (const QString &base : {places.dataHome, places.appData})
                if (!base.isEmpty())
                    addFolder(out, QDir(base).filePath(name));
            for (const QString &app : flatpaks)
                addFolder(out, QDir(app).filePath(QStringLiteral("data/%1").arg(name)));
        }
    for (const QString &family : ryujinxFamilyNames())
        for (const QString &name : emulatorNameSpellings(family)) {
            for (const QString &base : {places.configHome, places.appData})
                if (!base.isEmpty())
                    addFolder(out, QDir(base).filePath(name));
            for (const QString &app : flatpaks)
                addFolder(out, QDir(app).filePath(QStringLiteral("config/%1").arg(name)));
        }
    return out;
}

std::vector<ModRecord> findInstalledMods(const QStringList &dataFolders)
{
    std::vector<ModRecord> out;
    QStringList seen;
    auto add = [&](const QString &romfs) {
        const QString c = clean(romfs);
        if (seen.contains(c))
            return;
        seen << c;
        out.push_back(modRecord(c));
    };
    for (const QString &data : dataFolders) {
        const QString load = child(data, QStringLiteral("load"));
        for (const QString &title : subfolders(load)) {
            if (!isTitleId(QFileInfo(title).fileName()))
                continue;
            for (const QString &mod : subfolders(title))
                add(child(mod, QStringLiteral("romfs")));
        }
        const QString contents = child(child(data, QStringLiteral("mods")), QStringLiteral("contents"));
        for (const QString &title : subfolders(contents)) {
            if (!isTitleId(QFileInfo(title).fileName()))
                continue;
            for (const QString &mod : subfolders(title))
                add(child(mod, QStringLiteral("romfs")));
        }
        const QString sd = child(child(child(data, QStringLiteral("sdmc")), QStringLiteral("atmosphere")),
                                 QStringLiteral("contents"));
        for (const QString &title : subfolders(sd)) {
            const QString romfs = child(title, QStringLiteral("romfs"));
            if (isTitleId(QFileInfo(title).fileName()) && QFileInfo(romfs).isDir())
                add(romfs);
        }
    }
    return out;
}

QString modRoot(const QString &romfs)
{
    return QFileInfo(clean(romfs)).path();
}

std::vector<ModFile> listModFiles(const QString &romfs)
{
    std::vector<ModFile> out;
    const QString root = modRoot(romfs);
    const QDir rootDir(root);
    for (const QString &part : {QStringLiteral("romfs"), QStringLiteral("exefs")}) {
        const QString folder = child(root, part);
        if (!QFileInfo(folder).isDir())
            continue;
        QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            out.push_back({rootDir.relativeFilePath(info.absoluteFilePath()), info.size(), info.lastModified()});
        }
    }
    std::sort(out.begin(), out.end(), [](const ModFile &a, const ModFile &b) { return a.path < b.path; });
    return out;
}

bool removeModFile(const QString &romfs, const QString &relative, bool toTrash, QString *error)
{
    const QString top = relative.section(u'/', 0, 0);
    if (!isSafeRelativePath(relative) || relative == top
        || (top.compare(QLatin1String("romfs"), Qt::CaseInsensitive) != 0
            && top.compare(QLatin1String("exefs"), Qt::CaseInsensitive) != 0)) {
        if (error)
            *error = QObject::tr("%1 is not a file of this mod.").arg(relative);
        return false;
    }
    const QString root = modRoot(romfs);
    const QString path = QDir(root).filePath(relative);
    if (!QFileInfo(path).isFile()) {
        if (error)
            *error = QObject::tr("%1 is no longer in the mod.").arg(relative);
        return false;
    }
    if (!removePath(path, false, toTrash, error))
        return false;
    const QString stop = clean(QDir(root).filePath(top));
    for (QString dir = clean(QFileInfo(path).path()); dir.startsWith(stop + u'/'); dir = clean(QFileInfo(dir).path()))
        if (!QDir().rmdir(dir))
            break;
    return true;
}

bool deleteMod(const QString &romfs, bool toTrash, QString *error)
{
    const QString folder = clean(romfs);
    const std::optional<ModLocation> mod = parseModFolder(folder);
    const QString target = mod && layoutHasModName(mod->layout) ? modRoot(folder) : folder;
    if (QFileInfo(folder).fileName().compare(QLatin1String("romfs"), Qt::CaseInsensitive) != 0
        || !QFileInfo(target).isDir()) {
        if (error)
            *error = QObject::tr("%1 is not a mod folder that can be deleted.").arg(QDir::toNativeSeparators(target));
        return false;
    }
    return removePath(target, true, toTrash, error);
}

QString zipPathFor(const ModRecord &record, const QString &relative)
{
    const std::optional<ModLocation> mod = parseModFolder(record.folder);
    if (!mod)
        return relative;
    if (!layoutHasModName(mod->layout))
        return QStringLiteral("atmosphere/contents/%1/%2").arg(mod->titleId, relative);
    return QStringLiteral("%1/%2").arg(safeFileName(mod->modName), relative);
}

bool packModZip(const ModRecord &record, const QString &zipPath, const std::function<bool(int done, int total)> &progress,
                QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const std::vector<ModFile> files = listModFiles(record.folder);
    if (files.empty())
        return fail(QObject::tr("The mod has no files yet."));
    QSaveFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly))
        return fail(QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(zipPath), out.errorString()));
    ZipWriter zip(&out);
    const QString root = modRoot(record.folder);
    bool canceled = false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (progress && !progress(int(i), int(files.size()))) {
            canceled = true;
            break;
        }
        QFile source(QDir(root).filePath(files[i].path));
        if (!source.open(QIODevice::ReadOnly))
            return fail(QObject::tr("Cannot read %1: %2").arg(files[i].path, source.errorString()));
        if (!zip.addFile(zipPathFor(record, files[i].path), &source))
            return fail(zip.error());
    }
    if (canceled) {
        out.cancelWriting();
        return fail(QObject::tr("Canceled."));
    }
    if (!zip.finish())
        return fail(zip.error());
    if (!out.commit())
        return fail(QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(zipPath), out.errorString()));
    if (progress)
        progress(int(files.size()), int(files.size()));
    return true;
}
