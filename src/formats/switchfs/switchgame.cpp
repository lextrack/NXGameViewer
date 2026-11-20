#include "switchfs/switchgame.h"

#include "core/bytes.h"
#include "core/paths.h"
#include "switchfs/switchkeys.h"
#include "switchfs/switchnca.h"
#include "switchfs/switchncz.h"
#include "switchfs/switchnup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace {

constexpr qint64 kChunk = qint64{8} * 1024 * 1024;
constexpr int kWorkers = 4;
constexpr qint64 kMaxSmallFile = qint64{16} * 1024 * 1024;

const char *const kLanguages[16] = {"AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
                                    "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
                                    "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese",
                                    "BrazilianPortuguese"};

struct Package
{
    StoragePtr file;
    std::map<QString, StoragePtr> entries;
    SwitchKeys keys;
};

void readTicket(const QByteArray &ticket, SwitchKeys &keys)
{
    if (ticket.size() < 4)
        return;
    qsizetype signature = 0;
    switch (le32(ticket, 0)) {
    case 0x10000: case 0x10003: signature = 0x200 + 0x3C; break;
    case 0x10001: case 0x10004: signature = 0x100 + 0x3C; break;
    case 0x10002: case 0x10005: signature = 0x3C + 0x40; break;
    default: return;
    }
    const qsizetype base = 4 + signature;
    if (ticket.size() < base + 0x170 || uchar(ticket[base + 0x141]) != 0)
        return;
    keys.addTitleKey(ticket.mid(base + 0x160, 16), ticket.mid(base + 0x40, 16));
}

std::optional<Package> openPackage(const QString &path, const SwitchKeys &keys, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Package> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    Package p;
    p.keys = keys;
    p.file = fileStorage(path, error);
    if (!p.file)
        return std::nullopt;
    StoragePtr contents;
    const QByteArray start = p.file->read(0, std::min<qint64>(p.file->size(), 0x1200));
    if (start.startsWith("PFS0")) {
        contents = p.file;
    } else {
        qint64 base = -1;
        if (start.size() >= 0x140 && start.mid(0x100, 4) == "HEAD")
            base = 0;
        else if (start.size() >= 0x1140 && start.mid(0x1100, 4) == "HEAD")
            base = 0x1000;
        if (base < 0)
            return fail(QObject::tr("Not an NSP or XCI file."));
        const qint64 rootAt = base + qint64(le64(start, base + 0x130));
        StoragePtr root = subStorage(p.file, rootAt, p.file->size() - rootAt);
        const std::optional<std::vector<PartitionEntry>> partitions = readPartition(root, error);
        if (!partitions)
            return std::nullopt;
        for (const PartitionEntry &e : *partitions)
            if (e.name == QLatin1String("secure"))
                contents = subStorage(root, e.offset, e.size);
        if (!contents)
            return fail(QObject::tr("The XCI has no secure partition."));
    }
    const std::optional<std::vector<PartitionEntry>> files = readPartition(contents, error);
    if (!files)
        return std::nullopt;
    for (const PartitionEntry &e : *files) {
        StoragePtr entry = subStorage(contents, e.offset, e.size);
        if (!entry)
            continue;
        const QString name = e.name.toLower();
        if (name.endsWith(QLatin1String(".tik")) && e.size <= kMaxSmallFile)
            readTicket(entry->read(0, e.size), p.keys);
        if (name.endsWith(QLatin1String(".ncz"))) {
            if (StoragePtr nca = nczStorage(entry, nullptr))
                p.entries.emplace(name.chopped(1) + u'a', std::move(nca));
            continue;
        }
        p.entries.emplace(name, entry);
    }
    return p;
}

StoragePtr findFile(const std::vector<RomfsFile> &files, const StoragePtr &romfs, const QString &name)
{
    for (const RomfsFile &f : files)
        if (f.path.compare(name, Qt::CaseInsensitive) == 0)
            return subStorage(romfs, f.offset, f.size);
    return nullptr;
}

void readControl(const Package &p, const QString &ncaName, SwitchTitle &title)
{
    const auto it = p.entries.find(ncaName);
    if (it == p.entries.end())
        return;
    const std::optional<SwitchNca> nca = SwitchNca::open(it->second, p.keys, nullptr);
    if (!nca)
        return;
    for (int s = 0; s < 4; ++s) {
        if (!nca->isRomfsSection(s))
            continue;
        StoragePtr romfs = nca->romfs(s, nullptr);
        const std::optional<std::vector<RomfsFile>> files = romfs ? listRomfs(romfs, nullptr) : std::nullopt;
        if (!files)
            continue;
        StoragePtr nacp = findFile(*files, romfs, QStringLiteral("control.nacp"));
        if (!nacp || nacp->size() < 0x3070)
            continue;
        const QByteArray data = nacp->read(0, 0x3070);
        int language = -1;
        for (int i = 0; i < 16 && language < 0; ++i)
            if (data[0x300 * i] != '\0')
                language = i;
        auto text = [&data](qsizetype at, qsizetype size) {
            const QByteArray bytes = data.mid(at, size);
            const qsizetype end = bytes.indexOf('\0');
            return QString::fromUtf8(end < 0 ? bytes : bytes.left(end)).trimmed();
        };
        if (language >= 0) {
            title.name = text(0x300 * language, 0x200);
            title.publisher = text(0x300 * language + 0x200, 0x100);
        }
        title.displayVersion = text(0x3060, 0x10);
        StoragePtr icon;
        if (language >= 0)
            icon = findFile(*files, romfs, QStringLiteral("icon_%1.dat").arg(QLatin1String(kLanguages[language])));
        for (int i = 0; i < 16 && !icon; ++i)
            icon = findFile(*files, romfs, QStringLiteral("icon_%1.dat").arg(QLatin1String(kLanguages[i])));
        if (icon && icon->size() <= kMaxSmallFile)
            title.icon = icon->read(0, icon->size());
        return;
    }
}

QString checkReadable(const Package &p, const QString &ncaName, bool needRomfs)
{
    const auto it = p.entries.find(ncaName);
    if (it == p.entries.end())
        return QObject::tr("The package lacks %1.").arg(ncaName);
    QString error;
    const std::optional<SwitchNca> nca = SwitchNca::open(it->second, p.keys, &error);
    if (!nca)
        return error;
    bool found = false;
    for (int s = 0; s < 4; ++s) {
        if (nca->isPatchSection(s))
            return QObject::tr("Updates cannot be extracted yet: only base games and DLC.");
        if (!nca->isRomfsSection(s))
            continue;
        StoragePtr romfs = nca->romfs(s, &error);
        if (!romfs)
            return error;
        const QByteArray header = romfs->read(0, 8);
        if (header.size() != 8 || le64(header, 0) != 0x50)
            return QObject::tr("The game's files cannot be decrypted. Are the keys (prod.keys) from your own console "
                               "and up to date?");
        found = true;
    }
    if (needRomfs && !found)
        return QObject::tr("The content has no RomFS.");
    return {};
}

std::vector<SwitchTitle> titlesFromMeta(const Package &p, const QString &path)
{
    std::vector<SwitchTitle> titles;
    for (const auto &[name, storage] : p.entries) {
        if (!name.endsWith(QLatin1String(".cnmt.nca")))
            continue;
        const std::optional<SwitchNca> meta = SwitchNca::open(storage, p.keys, nullptr);
        if (!meta)
            continue;
        StoragePtr partition = meta->partition(0, nullptr);
        const std::optional<std::vector<PartitionEntry>> files = partition ? readPartition(partition, nullptr) : std::nullopt;
        if (!files)
            continue;
        for (const PartitionEntry &f : *files) {
            if (!f.name.endsWith(QLatin1String(".cnmt"), Qt::CaseInsensitive) || f.size < 0x20 || f.size > kMaxSmallFile)
                continue;
            const QByteArray cnmt = partition->read(f.offset, f.size);
            if (cnmt.size() < 0x20)
                continue;
            SwitchTitle t;
            t.package = path;
            t.titleId = le64(cnmt, 0);
            t.version = le32(cnmt, 8);
            switch (uchar(cnmt[0x0C])) {
            case 0x80: t.type = SwitchTitle::Application; break;
            case 0x81: t.type = SwitchTitle::Patch; break;
            case 0x82: t.type = SwitchTitle::AddOn; break;
            default: t.type = SwitchTitle::Other; break;
            }
            const qsizetype records = 0x20 + qFromLittleEndian<quint16>(cnmt.constData() + 0x0E);
            const int count = qFromLittleEndian<quint16>(cnmt.constData() + 0x10);
            QString control;
            for (int i = 0; i < count; ++i) {
                const qsizetype at = records + 0x38 * i;
                if (at + 0x38 > cnmt.size())
                    break;
                const QString nca = QString::fromLatin1(cnmt.mid(at + 0x20, 16).toHex());
                const int type = uchar(cnmt[at + 0x36]);
                const int offset = uchar(cnmt[at + 0x37]);
                const QString file = nca + QStringLiteral(".nca");
                if (type == 1)
                    offset == 0 ? t.programs.prepend(file) : t.programs.append(file);
                else if (type == 2)
                    t.data << file;
                else if (type == 3)
                    control = file;
            }
            if (!control.isEmpty())
                readControl(p, control, t);
            titles.push_back(std::move(t));
        }
    }
    return titles;
}

QString typeProblem(const SwitchTitle &t)
{
    switch (t.type) {
    case SwitchTitle::Application:
        return t.programs.isEmpty() ? QObject::tr("The game's program is missing from the package.") : QString();
    case SwitchTitle::AddOn:
        return t.data.isEmpty() ? QObject::tr("The DLC's data is missing from the package.") : QString();
    case SwitchTitle::Patch: return QObject::tr("Updates cannot be extracted yet: only base games and DLC.");
    case SwitchTitle::Other: return QObject::tr("This content is not a game or DLC.");
    }
    return {};
}

struct Job
{
    StoragePtr source;
    qint64 offset = 0;
    qint64 size = 0;
    QString path;
};

bool addNcaJobs(const Package &p, const QString &ncaName, const QString &prefix, bool exefs, std::vector<Job> &jobs,
                QString *error)
{
    const auto it = p.entries.find(ncaName);
    if (it == p.entries.end()) {
        if (error)
            *error = QObject::tr("The package lacks %1.").arg(ncaName);
        return false;
    }
    const std::optional<SwitchNca> nca = SwitchNca::open(it->second, p.keys, error);
    if (!nca)
        return false;
    for (int s = 0; s < 4; ++s) {
        if (exefs && s == 0 && nca->isPartitionSection(s)) {
            StoragePtr partition = nca->partition(s, error);
            const std::optional<std::vector<PartitionEntry>> files = partition ? readPartition(partition, error) : std::nullopt;
            if (!files)
                return false;
            for (const PartitionEntry &f : *files) {
                if (!isSafeRelativePath(f.name)) {
                    if (error)
                        *error = QObject::tr("The ExeFS has a file with an unsafe name: %1").arg(f.name);
                    return false;
                }
                jobs.push_back({partition, f.offset, f.size, prefix + QStringLiteral("exefs/") + f.name});
            }
        } else if (nca->isRomfsSection(s)) {
            StoragePtr romfs = nca->romfs(s, error);
            const std::optional<std::vector<RomfsFile>> files = romfs ? listRomfs(romfs, error) : std::nullopt;
            if (!files)
                return false;
            for (const RomfsFile &f : *files)
                jobs.push_back({romfs, f.offset, f.size, prefix + QStringLiteral("romfs/") + f.path});
        }
    }
    return true;
}

}

QString SwitchTitle::typeName() const
{
    switch (type) {
    case Application: return QObject::tr("Game");
    case Patch: return QObject::tr("Update");
    case AddOn: return QObject::tr("DLC");
    case Other: return QObject::tr("Other");
    }
    return {};
}

QString SwitchTitle::displayName() const
{
    return name.isEmpty() ? QFileInfo(package).completeBaseName() : name;
}

QString SwitchTitle::folderName() const
{
    QString folder = displayName();
    if (name.isEmpty()) {
        static const QRegularExpression tag(QStringLiteral(R"(\[\s*(v\d+|[0-9A-Fa-f]{16})\s*\])"));
        QRegularExpressionMatchIterator it = tag.globalMatch(folder);
        QString kept;
        qsizetype from = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString inside = m.captured(1);
            if (inside.size() == 16 && inside.compare(titleIdText(), Qt::CaseInsensitive) != 0)
                continue;
            kept += folder.mid(from, m.capturedStart() - from);
            from = m.capturedEnd();
        }
        folder = kept + folder.mid(from);
    }
    folder.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])")), QStringLiteral(" "));
    folder = folder.simplified();
    if (folder.isEmpty())
        folder = QObject::tr("Game");
    return QStringLiteral("%1 [%2]").arg(folder, titleIdText());
}

SwitchKeyCheck checkSwitchPackageKeys(const QString &package, const SwitchKeys &keys)
{
    SwitchKeyCheck check;
    if (isNupFile(package)) {
        check.opened = true;
        check.readsHeaders = true;
        check.hasGenerationKeys = true;
        return check;
    }
    const std::optional<Package> p = openPackage(package, keys, nullptr);
    if (!p)
        return check;
    check.opened = true;
    bool anyNca = false;
    bool allHeaders = true;
    bool allGenerations = true;
    for (const auto &[name, storage] : p->entries) {
        if (!name.endsWith(QLatin1String(".nca")))
            continue;
        anyNca = true;
        const std::optional<SwitchNca> nca = SwitchNca::open(storage, p->keys, nullptr);
        if (!nca) {
            allHeaders = false;
            continue;
        }
        check.neededGeneration = std::max(check.neededGeneration, nca->keyGeneration());
        allGenerations = allGenerations && nca->hasGenerationKeys();
    }
    check.readsHeaders = anyNca && allHeaders;
    check.hasGenerationKeys = check.readsHeaders && allGenerations;
    return check;
}

std::vector<SwitchTitle> scanSwitchPackage(const QString &path, const SwitchKeys &keys, QString *error)
{
    if (isNupFile(path))
        return scanNupPackage(path, error);
    const std::optional<Package> p = openPackage(path, keys, error);
    if (!p)
        return {};
    std::vector<SwitchTitle> titles = titlesFromMeta(*p, path);
    if (titles.empty() && error)
        *error = QObject::tr("No game was found in the package (are the keys right?).");
    for (SwitchTitle &t : titles) {
        if (t.problem.isEmpty())
            t.problem = typeProblem(t);
        if (t.problem.isEmpty())
            t.problem = checkReadable(*p, t.type == SwitchTitle::AddOn ? t.data.front() : t.programs.front(),
                                      t.type == SwitchTitle::AddOn);
    }
    return titles;
}

std::optional<std::vector<SwitchTitleFile>> switchTitleFiles(const SwitchTitle &title, const SwitchKeys &keys,
                                                             QString *error)
{
    if (!title.canExtract()) {
        if (error)
            *error = title.problem;
        return std::nullopt;
    }
    if (title.nupPackage >= 0)
        return nupTitleFiles(title, error);
    const std::optional<Package> p = openPackage(title.package, keys, error);
    if (!p)
        return std::nullopt;
    std::vector<Job> jobs;
    if (title.type == SwitchTitle::AddOn) {
        for (const QString &nca : title.data)
            if (!addNcaJobs(*p, nca, QString(), false, jobs, error))
                return std::nullopt;
    } else {
        for (qsizetype i = 0; i < title.programs.size(); ++i) {
            const QString prefix = i == 0 ? QString() : QStringLiteral("programs/%1/").arg(i);
            if (!addNcaJobs(*p, title.programs[i], prefix, true, jobs, error))
                return std::nullopt;
        }
    }
    for (auto first = jobs.begin(); first != jobs.end();) {
        const auto last = std::find_if(first, jobs.end(), [&](const Job &j) { return j.source != first->source; });
        std::stable_sort(first, last, [](const Job &a, const Job &b) { return a.offset < b.offset; });
        first = last;
    }
    std::vector<SwitchTitleFile> files;
    files.reserve(jobs.size());
    for (const Job &j : jobs)
        files.push_back({j.path, j.size, [source = j.source, base = j.offset, size = j.size](qint64 offset, char *out,
                                                                                         qint64 length) {
                             return offset >= 0 && length >= 0 && offset <= size - length
                                 && source->read(base + offset, out, length);
                         },
                         j.source->seeksFast(), !j.source->seeksFast()});
    return files;
}

bool verifySwitchTitle(const SwitchTitle &title, const SwitchKeys &keys,
                       const std::function<bool(qint64 done, qint64 total)> &progress, QString *error)
{
    if (!title.canExtract()) {
        if (error)
            *error = title.problem;
        return false;
    }
    if (title.nupPackage >= 0)
        return verifyNupTitle(title, progress, error);
    const std::optional<Package> p = openPackage(title.package, keys, error);
    if (!p)
        return false;
    std::vector<std::pair<SwitchNca, int>> sections;
    for (const QString &name : title.type == SwitchTitle::AddOn ? title.data : title.programs) {
        const auto it = p->entries.find(name);
        if (it == p->entries.end()) {
            if (error)
                *error = QObject::tr("The package lacks %1.").arg(name);
            return false;
        }
        std::optional<SwitchNca> nca = SwitchNca::open(it->second, p->keys, error);
        if (!nca)
            return false;
        for (int s = 0; s < 4; ++s)
            if (nca->isRomfsSection(s))
                sections.emplace_back(*nca, s);
    }
    qint64 total = 0;
    std::vector<qint64> sizes;
    for (const auto &[nca, s] : sections) {
        StoragePtr romfs = nca.romfs(s, error);
        if (!romfs)
            return false;
        sizes.push_back(romfs->size());
        total += romfs->size();
    }
    qint64 before = 0;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto &[nca, s] = sections[i];
        const bool ok = nca.verifyRomfs(s, [&](qint64 done, qint64) { return !progress || progress(before + std::min(done, sizes[i]), total); },
                                        error);
        if (!ok)
            return false;
        before += sizes[i];
    }
    return true;
}

bool extractSwitchTitle(const SwitchTitle &title, const SwitchKeys &keys, const QString &destination,
                        const std::function<bool(qint64 done, qint64 total)> &progress, SwitchExtractResult *result,
                        QString *error)
{
    if (result)
        *result = {};
    const std::optional<std::vector<SwitchTitleFile>> files = switchTitleFiles(title, keys, error);
    if (!files)
        return false;
    return extractSwitchFiles(*files, destination, progress, result, error);
}

bool extractSwitchFiles(const std::vector<SwitchTitleFile> &files, const QString &destination,
                        const std::function<bool(qint64 done, qint64 total)> &progress, SwitchExtractResult *result,
                        QString *error)
{
    SwitchExtractResult local;
    SwitchExtractResult &r = result ? *result : local;
    r = {};
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    qint64 total = 0;
    for (const SwitchTitleFile &f : files)
        total += f.size;
    const QDir root(destination);
    if (!QDir().mkpath(destination))
        return fail(QObject::tr("Cannot make the folder %1.").arg(QDir::toNativeSeparators(destination)));
    QSet<QString> folders;
    for (const SwitchTitleFile &f : files)
        folders.insert(QFileInfo(root.filePath(f.path)).absolutePath());
    for (const QString &folder : std::as_const(folders))
        if (!QDir().mkpath(folder))
            return fail(QObject::tr("Cannot make the folder %1.").arg(QDir::toNativeSeparators(folder)));
    if (progress && !progress(0, total)) {
        r.canceled = true;
        return true;
    }

    std::mutex mutex;
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    qint64 done = 0;
    QString problem;
    auto report = [&](qint64 step) {
        const std::lock_guard lock(mutex);
        done += step;
        if (!stop && progress && !progress(done, total)) {
            r.canceled = true;
            stop = true;
        }
        return !stop;
    };
    auto setProblem = [&](const QString &message) {
        const std::lock_guard lock(mutex);
        if (problem.isEmpty() && !r.canceled)
            problem = message;
        stop = true;
    };
    auto worker = [&] {
        QByteArray buffer(kChunk, Qt::Uninitialized);
        for (size_t i = next++; i < files.size() && !stop; i = next++) {
            const SwitchTitleFile &f = files[i];
            const QString target = root.filePath(f.path);
            const QString partial = target + QStringLiteral(".nxgv-partial");
            QFile out(partial);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                setProblem(QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
                return;
            }
            bool ok = true;
            for (qint64 at = 0; at < f.size && ok;) {
                const qint64 step = std::min(kChunk, f.size - at);
                if (!f.read(at, buffer.data(), step)) {
                    setProblem(QObject::tr("Cannot read %1 from the package.").arg(f.path));
                    ok = false;
                } else if (out.write(buffer.constData(), step) != step) {
                    setProblem(QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
                    ok = false;
                } else {
                    at += step;
                    ok = report(step);
                }
            }
            out.close();
            if (ok && out.error() != QFileDevice::NoError) {
                setProblem(QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
                ok = false;
            }
            if (ok) {
                QFile::remove(target);
                if (!QFile::rename(partial, target)) {
                    setProblem(QObject::tr("Cannot write %1.").arg(QDir::toNativeSeparators(target)));
                    ok = false;
                }
            }
            if (!ok) {
                QFile::remove(partial);
                return;
            }
            const std::lock_guard lock(mutex);
            ++r.files;
            r.bytes += f.size;
        }
    };
    const bool sequential = std::any_of(files.begin(), files.end(), [](const SwitchTitleFile &f) { return f.sequential; });
    const int workers = sequential ? 1 : std::clamp(QThread::idealThreadCount(), 1, kWorkers);
    const int count = int(std::clamp<size_t>(files.size(), 1, size_t(workers)));
    std::vector<std::thread> threads;
    for (int i = 1; i < count; ++i)
        threads.emplace_back(worker);
    worker();
    for (std::thread &thread : threads)
        thread.join();
    if (!problem.isEmpty())
        return fail(problem);
    return true;
}
