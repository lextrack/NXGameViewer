#include "switchfs/switchkeysearch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <tuple>

namespace {

constexpr qint64 kMaxKeyFile = qint64{4} * 1024 * 1024;

QStringList subfolders(const QString &folder)
{
    QStringList out;
    if (folder.isEmpty())
        return out;
    const QDir dir(folder);
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        out << dir.filePath(name);
    return out;
}

QString join(const QString &base, const QString &relative)
{
    return base.isEmpty() ? QString() : QDir(base).filePath(relative);
}

class FolderList
{
public:
    void add(const QString &path)
    {
        if (path.isEmpty())
            return;
        const QFileInfo info(path);
        if (!info.isDir())
            return;
        const QString clean = QDir::cleanPath(info.absoluteFilePath());
        if (!m_folders.contains(clean))
            m_folders << clean;
    }
    QStringList folders() const { return m_folders; }

private:
    QStringList m_folders;
};

struct KeyFile
{
    QString path;
    QByteArray text;
};

std::vector<KeyFile> keyFilesIn(const QString &folder)
{
    std::vector<KeyFile> out;
    const QFileInfoList files = QDir(folder).entryInfoList(
        {QStringLiteral("*.keys"), QStringLiteral("*.txt"), QStringLiteral("*.ini")}, QDir::Files | QDir::Readable,
        QDir::Name);
    for (const QFileInfo &info : files) {
        if (info.size() <= 0 || info.size() > kMaxKeyFile)
            continue;
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        out.push_back({info.absoluteFilePath(), file.readAll()});
    }
    return out;
}

struct Candidate
{
    QString path;
    SwitchKeys keys;
};

struct Scores
{
    bool tested = false;
    bool works = false;
    int reads = 0;
    int working = 0;
    bool readsAll = false;
    int needed = -1;
};

Scores score(const SwitchKeys &keys, const QStringList &packages)
{
    Scores s;
    bool worksAll = true;
    bool readsAll = true;
    for (const QString &package : packages) {
        const SwitchKeyCheck check = checkSwitchPackageKeys(package, keys);
        if (!check.opened)
            continue;
        s.tested = true;
        s.needed = std::max(s.needed, check.neededGeneration);
        s.reads += check.readsHeaders ? 1 : 0;
        s.working += check.works() ? 1 : 0;
        worksAll = worksAll && check.works();
        readsAll = readsAll && check.readsHeaders;
    }
    s.works = s.tested && worksAll;
    s.readsAll = s.tested && readsAll;
    return s;
}

QString problemFor(const FoundSwitchKeys &found)
{
    if (!found.testedOnGame || found.usable)
        return {};
    if (!found.readsGame)
        return QObject::tr("The key file %1 cannot decrypt this game: its header_key does not match. Dump the keys of "
                           "your own console again.")
            .arg(QDir::toNativeSeparators(found.prodKeys));
    return QObject::tr("This game needs keys up to %1, but the newest keys found go up to %2. Update your console, "
                       "then dump its keys again.")
        .arg(SwitchKeys::generationName(found.neededGeneration),
             found.newestGeneration < 0 ? QObject::tr("none") : SwitchKeys::generationName(found.newestGeneration));
}

FoundSwitchKeys describe(const Candidate &c, const Scores &s, const QStringList &titleFiles)
{
    FoundSwitchKeys found;
    found.prodKeys = c.path;
    found.titleKeyFiles = titleFiles;
    found.keys = c.keys;
    found.newestGeneration = c.keys.newestGeneration();
    found.neededGeneration = s.needed;
    found.testedOnGame = s.tested;
    found.readsGame = s.readsAll;
    found.usable = !s.tested || s.works;
    return found;
}

struct TitleKeys
{
    QStringList files;
    std::vector<QByteArray> texts;

    void addTo(SwitchKeys &keys) const
    {
        for (const QByteArray &text : texts)
            keys.addTitleKeys(text);
    }
};

TitleKeys titleKeysIn(const QStringList &folders, std::vector<Candidate> *candidates)
{
    TitleKeys titles;
    for (const QString &folder : folders) {
        for (KeyFile &file : keyFilesIn(folder)) {
            if (SwitchKeys::countTitleKeys(file.text) > 0) {
                titles.files << file.path;
                titles.texts.push_back(file.text);
            }
            if (!candidates)
                continue;
            std::optional<SwitchKeys> keys = SwitchKeys::fromText(file.text, {}, nullptr);
            if (keys)
                candidates->push_back({file.path, std::move(*keys)});
        }
    }
    return titles;
}

}

const QStringList &yuzuFamilyNames()
{
    static const QStringList names{QStringLiteral("yuzu"),    QStringLiteral("eden"),  QStringLiteral("lichii"),
                                   QStringLiteral("citron"),  QStringLiteral("sudachi"), QStringLiteral("suyu"),
                                   QStringLiteral("torzu")};
    return names;
}

const QStringList &ryujinxFamilyNames()
{
    static const QStringList names{QStringLiteral("Ryujinx"), QStringLiteral("Ryubing")};
    return names;
}

QStringList emulatorNameSpellings(const QString &name)
{
    QStringList out{name, name.toLower()};
    QString capital = name.toLower();
    if (!capital.isEmpty())
        capital[0] = capital[0].toUpper();
    out << capital;
    out.removeDuplicates();
    return out;
}

QStringList KeySearchPlaces::foldersOf(const QStringList &packages)
{
    QStringList out;
    for (const QString &package : packages) {
        const QFileInfo info(package);
        const QString folder = info.absolutePath();
        const QString parent = QFileInfo(folder).absolutePath();
        for (const QString &f : {folder, parent})
            if (!out.contains(f))
                out << f;
    }
    return out;
}

KeySearchPlaces KeySearchPlaces::forSystem(const QStringList &packages)
{
    KeySearchPlaces places;
    places.gameFolders = foldersOf(packages);
    places.home = QDir::homePath();
    const QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
#if defined(Q_OS_WIN)
    places.appData = qEnvironmentVariable("APPDATA");
    places.localAppData = qEnvironmentVariable("LOCALAPPDATA");
#elif defined(Q_OS_MACOS)
    places.appData = join(places.home, QStringLiteral("Library/Application Support"));
#else
    places.dataHome = xdgData.isEmpty() ? join(places.home, QStringLiteral(".local/share")) : xdgData;
    places.configHome = xdgConfig.isEmpty() ? join(places.home, QStringLiteral(".config")) : xdgConfig;
#endif
    return places;
}

QStringList switchKeyFolders(const KeySearchPlaces &places)
{
    FolderList list;
    for (const QString &folder : places.gameFolders)
        list.add(folder);

    QStringList portableBases = places.gameFolders;
    for (const QString &folder : places.gameFolders)
        portableBases << subfolders(folder);
    const QStringList flatpaks = subfolders(join(places.home, QStringLiteral(".var/app")));

    for (const QString &family : yuzuFamilyNames())
        for (const QString &name : emulatorNameSpellings(family)) {
            list.add(join(join(places.dataHome, name), QStringLiteral("keys")));
            list.add(join(join(places.appData, name), QStringLiteral("keys")));
            for (const QString &app : flatpaks)
                list.add(join(app, QStringLiteral("data/%1/keys").arg(name)));
            portableBases << subfolders(join(places.localAppData, name));
        }
    for (const QString &base : portableBases)
        list.add(join(base, QStringLiteral("user/keys")));
    for (const QString &base : {places.dataHome, places.appData})
        for (const QString &folder : subfolders(base))
            list.add(join(folder, QStringLiteral("keys")));
    for (const QString &app : flatpaks)
        for (const QString &folder : subfolders(join(app, QStringLiteral("data"))))
            list.add(join(folder, QStringLiteral("keys")));

    for (const QString &family : ryujinxFamilyNames())
        for (const QString &name : emulatorNameSpellings(family)) {
            list.add(join(join(places.configHome, name), QStringLiteral("system")));
            list.add(join(join(places.appData, name), QStringLiteral("system")));
            for (const QString &app : flatpaks)
                list.add(join(app, QStringLiteral("config/%1/system").arg(name)));
        }
    for (const QString &base : portableBases)
        list.add(join(base, QStringLiteral("portable/system")));
    for (const QString &base : {places.configHome, places.appData})
        for (const QString &folder : subfolders(base))
            list.add(join(folder, QStringLiteral("system")));
    for (const QString &app : flatpaks)
        for (const QString &folder : subfolders(join(app, QStringLiteral("config"))))
            list.add(join(folder, QStringLiteral("system")));

    list.add(join(places.home, QStringLiteral(".switch")));
    return list.folders();
}

SwitchKeySearch findSwitchKeys(const QStringList &packages, const KeySearchPlaces &places)
{
    SwitchKeySearch search;
    std::vector<Candidate> candidates;
    const TitleKeys titles = titleKeysIn(switchKeyFolders(places), &candidates);
    if (candidates.empty()) {
        search.problem = QObject::tr("No key file was found. Choose the prod.keys dumped from your own console.");
        return search;
    }

    size_t best = 0;
    Scores bestScores;
    auto rank = [](const Scores &s, const SwitchKeys &keys) {
        return std::make_tuple(s.works, s.reads, s.working, keys.newestGeneration());
    };
    for (size_t i = 0; i < candidates.size(); ++i) {
        titles.addTo(candidates[i].keys);
        const Scores s = score(candidates[i].keys, packages);
        if (i == 0 || rank(s, candidates[i].keys) > rank(bestScores, candidates[best].keys)) {
            best = i;
            bestScores = s;
        }
    }
    search.found = describe(candidates[best], bestScores, titles.files);
    search.problem = problemFor(*search.found);
    return search;
}

SwitchKeySearch useSwitchKeyFile(const QString &prodKeys, const QStringList &packages, const KeySearchPlaces &places)
{
    SwitchKeySearch search;
    QString error;
    std::optional<SwitchKeys> keys = SwitchKeys::load(prodKeys, &error);
    if (!keys) {
        search.problem = error;
        return search;
    }
    TitleKeys titles = titleKeysIn(switchKeyFolders(places), nullptr);
    const QString beside = SwitchKeys::titleKeysBeside(prodKeys);
    if (!beside.isEmpty() && !titles.files.contains(QFileInfo(beside).absoluteFilePath()))
        titles.files.prepend(QFileInfo(beside).absoluteFilePath());
    Candidate candidate{QFileInfo(prodKeys).absoluteFilePath(), std::move(*keys)};
    titles.addTo(candidate.keys);
    search.found = describe(candidate, score(candidate.keys, packages), titles.files);
    search.problem = problemFor(*search.found);
    return search;
}
