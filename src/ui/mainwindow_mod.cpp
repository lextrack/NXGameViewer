#include "mainwindow.h"
#include "formats/archives/compress.h"
#include "formats/archives/zstddictionaries.h"
#include "formats/modding/editedjson.h"
#include "formats/modding/restblupdate.h"
#include "formats/modding/modlibrary.h"
#include "formats/switchfs/switchkeysearch.h"
#include "formats/switchfs/switchpackagereader.h"
#include "formats/textures/bntxwriter.h"
#include "modhistory.h"
#include "modmanagerdialog.h"
#include "newmoddialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QMenu>
#include <QSettings>
#include <QToolButton>

#include <cstring>
#include <optional>

namespace {

QString modDescription(const QString &folder)
{
    const std::optional<ModLocation> mod = parseModFolder(folder);
    if (!mod)
        return QDir::toNativeSeparators(folder);
    if (!layoutHasModName(mod->layout))
        return QObject::tr("Atmosphère, %1").arg(mod->titleId);
    return QObject::tr("“%1” for %2").arg(mod->modName, mod->titleId);
}

QString turnOnHint(const QString &folder)
{
    const std::optional<ModLocation> mod = parseModFolder(folder);
    if (!mod)
        return QObject::tr("To see it in the game, copy or link this mod folder to where your emulator loads mods from.");
    switch (mod->layout) {
    case ModLayout::YuzuFamily:
        return QObject::tr("To see it in the game, make sure “%1” is ticked in the emulator: right-click the "
                           "game, then Properties → Add-ons.").arg(mod->modName);
    case ModLayout::Ryujinx:
        return QObject::tr("To see it in the game, make sure “%1” is enabled in Ryujinx: right-click the "
                           "game, then Manage Mods.").arg(mod->modName);
    case ModLayout::Atmosphere:
        return QObject::tr("If this folder is not on the SD card, copy its <tt>atmosphere</tt> folder to the root of "
                           "the SD card.");
    }
    return {};
}

QString rebuiltDetail(const std::vector<RebuiltLayer> &rebuilt, const QString &leafName, bool sizesHandled)
{
    if (rebuilt.empty())
        return {};
    QStringList layers;
    QStringList resized;
    for (const RebuiltLayer &layer : rebuilt) {
        const QString kind = layer.compression.isEmpty() ? QObject::tr("SARC")
                                                         : QObject::tr("SARC, %1").arg(layer.compression.join(", "));
        layers << QObject::tr("<tt>%1</tt> (%2)").arg(layer.name.toHtmlEscaped(), kind);
        if (layer.newUnpacked != layer.oldUnpacked)
            resized << QObject::tr("<tt>%1</tt>: %2 → %3 bytes")
                           .arg(layer.name.toHtmlEscaped(), QLocale().toString(qint64(layer.oldUnpacked)),
                                QLocale().toString(qint64(layer.newUnpacked)));
    }
    QString detail = QObject::tr("<br><br>Rebuilt %1 to hold %2.").arg(layers.join(QObject::tr(" and ")), leafName.toHtmlEscaped());
    if (resized.isEmpty())
        detail += QObject::tr(" The archives kept their unpacked size.");
    else if (sizesHandled)
        detail += QObject::tr("<br><br>The unpacked size changed: %1.").arg(resized.join(u"; "));
    else
        detail += QObject::tr("<br><br><b>The unpacked size changed</b>: %1.<br>Some games keep a table of file sizes (for "
                              "example a resource size table, <tt>.rsizetable</tt> or <tt>.srsizetable</tt>). If the game "
                              "does not load the change, that table may need the new size.")
                      .arg(resized.join(u"; "));
    return detail;
}

std::optional<QByteArray> readWhole(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    if (file.size() > kMaxEntryInMemory) {
        if (error)
            *error = QObject::tr("%1 is larger than %2.").arg(QFileInfo(path).fileName(),
                                                              QLocale().formattedDataSize(kMaxEntryInMemory));
        return std::nullopt;
    }
    return file.readAll();
}

}

QString MainWindow::modFolder() const
{
    return m_activeMod;
}

void MainWindow::setActiveMod(const QString &romfs)
{
    m_activeMod = romfs.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(romfs));
    if (!m_activeMod.isEmpty()) {
        ModRecord record = modRecord(m_activeMod);
        record.game = gameNameFromPath(m_currentPath + u'/' + m_currentEntries.join(u'/'), record.titleId);
        rememberModInHistory(std::move(record));
    }
    if (m_modFolderAction)
        updateModMenu();
}

bool MainWindow::chooseModFolder()
{
    QMessageBox intro(QMessageBox::Information, tr("Mod folder"),
                      tr("Choose which mod to save changed files in. They are saved at the same path they have in "
                         "the game, so the emulator loads them instead of the originals."),
                      QMessageBox::Ok | QMessageBox::Cancel, this);
    intro.setInformativeText(tr("Pick the mod's own folder (or the <b>romfs</b> folder inside it):<br>"
                                "yuzu and similar: <tt>…/load/&lt;title id&gt;/&lt;mod name&gt;</tt><br>"
                                "Ryujinx: <tt>…/mods/contents/&lt;title id&gt;/&lt;mod name&gt;</tt><br>"
                                "Atmosphère: <tt>…/atmosphere/contents/&lt;title id&gt;</tt><br><br>"
                                "To start a new mod, use <b>New mod…</b>: it makes these folders for you. "
                                "<b>My mods…</b> lists the mods you saved to before."));
    QPushButton *create = intro.addButton(tr("New mod…"), QMessageBox::ActionRole);
    create->setToolTip(tr("Make a new mod folder in the layout your emulator expects"));
    QPushButton *recent = intro.addButton(tr("My mods…"), QMessageBox::ActionRole);
    recent->setToolTip(tr("Pick one of your mods, or one found in your emulators' folders"));
    intro.button(QMessageBox::Ok)->setText(tr("Pick a folder…"));
    intro.button(QMessageBox::Ok)->setToolTip(tr("Pick an existing mod folder"));
    intro.exec();
    if (intro.clickedButton() == create)
        return newMod();
    if (intro.clickedButton() == recent) {
        manageMods();
        return !modFolder().isEmpty();
    }
    if (intro.clickedButton() != intro.button(QMessageBox::Ok))
        return false;
    QString start = modFolder().isEmpty() ? QString() : modRoot(modFolder());
    if (start.isEmpty()) {
        const std::vector<ModRecord> history = loadModHistory();
        start = history.empty() ? QDir::homePath() : QFileInfo(modRoot(history.front().folder)).path();
    }
    QString folder = QFileDialog::getExistingDirectory(this, tr("Mod folder"), start);
    if (folder.isEmpty())
        return false;
    if (QFileInfo(folder).fileName().compare(QLatin1String("romfs"), Qt::CaseInsensitive) != 0)
        folder = QDir(folder).filePath(QStringLiteral("romfs"));
    if (isOpenGameFolder(folder)) {
        QMessageBox::warning(this, tr("Mod folder"),
                             tr("%1 holds the game files you opened. Saving changes there would overwrite them. Use "
                                "<b>New mod…</b> to make a separate mod folder.")
                                 .arg(QDir::toNativeSeparators(folder)));
        return false;
    }
    if (!parseModFolder(folder)) {
        QMessageBox check(QMessageBox::Warning, tr("Mod folder"),
                          tr("Emulators will not find mods in this folder."), QMessageBox::Cancel, this);
        check.setInformativeText(
            tr("Files would be saved in <tt>%1</tt>, which is not inside <tt>load/&lt;title id&gt;/&lt;mod name&gt;</tt>, "
               "<tt>mods/contents/&lt;title id&gt;/&lt;mod name&gt;</tt> or <tt>atmosphere/contents/&lt;title id&gt;</tt>."
               "<br><br>If you picked a game's folder (named after its title ID), use <b>New mod…</b> to add a mod "
               "name. Use this folder only if you will copy the files somewhere else yourself.")
                .arg(QDir::toNativeSeparators(folder).toHtmlEscaped()));
        QPushButton *makeNew = check.addButton(tr("New mod…"), QMessageBox::AcceptRole);
        QPushButton *anyway = check.addButton(tr("Use it anyway"), QMessageBox::DestructiveRole);
        makeNew->setToolTip(tr("Make a mod folder in the layout your emulator expects"));
        anyway->setToolTip(tr("Save changed files in this folder even though emulators will not load them from here"));
        check.setDefaultButton(makeNew);
        check.exec();
        if (check.clickedButton() == makeNew)
            return newMod();
        if (check.clickedButton() != anyway)
            return false;
    }
    setActiveMod(folder);
    m_info->setText(tr("Saving changes to: %1").arg(QDir::toNativeSeparators(folder)));
    return true;
}

bool MainWindow::newMod()
{
    const QString context = m_currentPath.isEmpty() ? m_fsModel->rootPath() : m_currentPath;
    NewModDialog dialog(context + (m_currentEntries.isEmpty() ? QString() : u'/' + m_currentEntries.join(u'/')),
                        modFolder(), this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    setActiveMod(dialog.romfsPath());
    m_info->setText(tr("Saving changes to: %1").arg(QDir::toNativeSeparators(modFolder())));
    return true;
}

void MainWindow::manageMods()
{
    ModManagerDialog dialog(emulatorDataFolders(KeySearchPlaces::forSystem({})), modFolder(), this);
    if (dialog.exec() == QDialog::Accepted && !dialog.chosenMod().isEmpty()) {
        setActiveMod(dialog.chosenMod());
        m_info->setText(tr("Saving changes to: %1").arg(QDir::toNativeSeparators(modFolder())));
    } else if (!modFolder().isEmpty() && !QFileInfo(modRoot(modFolder())).isDir()) {
        m_activeMod.clear();
        updateModMenu();
    }
}

void MainWindow::updateModMenu()
{
    const QString folder = modFolder();
    m_modFolderAction->setText(folder.isEmpty() ? tr("Choose mod to save in…")
                                                : tr("Saving to: %1…").arg(modDescription(folder)));
    m_modFolderAction->setToolTip(folder.isEmpty() ? tr("Pick the mod that changed files are saved in")
                                                   : tr("Changed files are saved in %1. Click to use another mod")
                                                         .arg(QDir::toNativeSeparators(folder)));
    m_openModFolderAction->setEnabled(!folder.isEmpty());
    m_modMenu->menuAction()->setToolTip(folder.isEmpty()
                                ? tr("Make changed game files for an emulator mod folder. No mod is chosen yet")
                                : tr("Make changed game files for an emulator mod folder. Saving to: %1")
                                      .arg(modDescription(folder)));
}

bool MainWindow::isOpenGameFolder(const QString &folder) const
{
    const QString root = QFileInfo(m_fsModel->rootPath()).canonicalFilePath();
    const QString mod = QFileInfo(folder).canonicalFilePath();
    if (root.isEmpty() || mod.isEmpty() || parseModFolder(folder))
        return false;
    const QString romfs = QFileInfo(QDir(root).filePath(QStringLiteral("romfs"))).canonicalFilePath();
    return mod == root || mod == romfs || root.startsWith(mod + u'/');
}

std::optional<MainWindow::ModSource> MainWindow::modSource(const QString &path, const QStringList &entries,
                                                           QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<ModSource> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    ModSource source;
    if (SwitchPackageReader::isPackageName(path)) {
        if (entries.isEmpty())
            return fail(tr("Open a file inside the game to change it."));
        const QString first = entries.first();
        const qsizetype romfs = first.indexOf(QLatin1String("/romfs/"), 0, Qt::CaseInsensitive);
        if (romfs < 0)
            return fail(tr("Only files in the game's romfs can be changed with a mod."));
        const QString folder = first.left(romfs);
        static const QRegularExpression id(QStringLiteral(R"(\[([0-9A-Fa-f]{16})\]\s*$)"));
        const QRegularExpressionMatch m = id.match(folder);
        if (m.hasMatch())
            source.titleId = applicationTitleId(m.captured(1));
        source.game = folder.left(m.hasMatch() ? m.capturedStart() : folder.size()).trimmed();
        source.package = path;
        source.packageEntry = first;
        source.gamePath = first.mid(romfs + 7);
        source.outerName = QFileInfo(source.gamePath).fileName();
        source.nested = entries.mid(1);
    } else {
        source.diskPath = path;
        source.gamePath = gamePath(path);
        source.outerName = QFileInfo(path).fileName();
        source.nested = entries;
        if (const std::optional<QString> id = titleIdFromPath(path))
            source.titleId = applicationTitleId(*id);
        if (source.titleId)
            source.game = gameNameFromPath(path, *source.titleId);
    }
    source.target = QDir(modFolder()).filePath(source.gamePath);
    return source;
}

std::optional<QByteArray> MainWindow::modSourceBytes(const ModSource &source, QString *error)
{
    if (!source.diskPath.isEmpty()) {
        loadZstdDictionariesNear(source.diskPath);
        return readWhole(source.diskPath, error);
    }
    QString e;
    const std::unique_ptr<Document> doc = SwitchPackageReader().read(source.package, &e);
    const std::shared_ptr<const Archive> archive = doc && !doc->tables.empty() ? doc->tables.front()->archive() : nullptr;
    if (archive)
        loadZstdDictionariesFrom(*archive);
    const int index = archive ? archive->find(source.packageEntry) : -1;
    if (index < 0) {
        if (error)
            *error = e.isEmpty() ? tr("%1 is not in the game.").arg(source.packageEntry) : e;
        return std::nullopt;
    }
    if (archive->entries[size_t(index)].size > kMaxEntryInMemory) {
        if (error)
            *error = tr("%1 is larger than %2, too large to change here.")
                         .arg(source.outerName, QLocale().formattedDataSize(kMaxEntryInMemory));
        return std::nullopt;
    }
    return archive->entryData(size_t(index));
}

bool MainWindow::checkModTarget(const ModSource &source, const QString &title)
{
    if (!source.diskPath.isEmpty() && QFileInfo(source.target).canonicalFilePath() == QFileInfo(source.diskPath).canonicalFilePath()
        && isOpenGameFolder(modFolder())) {
        QMessageBox::warning(this, title,
                             tr("The mod folder is the game folder you opened (%1), so saving would overwrite the "
                                "game's own file. Use <b>Mod → New mod…</b> to make a separate mod folder.")
                                 .arg(QDir::toNativeSeparators(modFolder())));
        return false;
    }
    const std::optional<ModLocation> mod = parseModFolder(modFolder());
    if (!source.titleId || !mod || sameGame(*source.titleId, mod->titleId))
        return true;
    QMessageBox ask(QMessageBox::Warning, title,
                    tr("This file is from game %1, but the mod folder is for game %2.").arg(*source.titleId, mod->titleId),
                    QMessageBox::Cancel, this);
    ask.setInformativeText(tr("The emulator only loads a mod for the game whose title ID is in its folder, so this "
                              "change would not appear in game %1.<br><br>Mod folder: <tt>%2</tt>")
                               .arg(*source.titleId, QDir::toNativeSeparators(modFolder()).toHtmlEscaped()));
    QPushButton *makeNew = ask.addButton(tr("New mod for this game…"), QMessageBox::AcceptRole);
    QPushButton *anyway = ask.addButton(tr("Save there anyway"), QMessageBox::DestructiveRole);
    makeNew->setToolTip(tr("Make a mod folder for game %1 and save the change there").arg(*source.titleId));
    anyway->setToolTip(tr("Save in the current mod folder although it is for another game"));
    ask.setDefaultButton(makeNew);
    ask.exec();
    if (ask.clickedButton() == makeNew)
        return newMod();
    return ask.clickedButton() == anyway;
}

std::optional<MainWindow::ModSave> MainWindow::saveToMod(const ModSource &source, const QString &title,
                                                         const LeafEdit &edit, bool askToBuildOnEarlier,
                                                         bool needsOriginal)
{
    const QString target = QDir(modFolder()).filePath(source.gamePath);
    bool fromTarget = false;
    if (askToBuildOnEarlier && QFileInfo::exists(target)
        && (source.diskPath.isEmpty() || QFileInfo(target).canonicalFilePath() != QFileInfo(source.diskPath).canonicalFilePath())) {
        QMessageBox ask(QMessageBox::Question, title, tr("The mod folder already has a changed %1.").arg(source.gamePath),
                        QMessageBox::Cancel, this);
        ask.setInformativeText(tr("Build on it to keep the changes you already made to other parts of it, or "
                                  "start again from the game's file."));
        QPushButton *keep = ask.addButton(tr("Keep earlier changes"), QMessageBox::AcceptRole);
        QPushButton *fresh = ask.addButton(tr("Start from the game's file"), QMessageBox::DestructiveRole);
        keep->setToolTip(tr("Change the copy already in the mod folder, so your earlier changes to it are kept"));
        fresh->setToolTip(tr("Start again from the game's original file. Earlier changes to it in the mod folder are lost"));
        ask.setDefaultButton(keep);
        ask.exec();
        if (ask.clickedButton() == keep)
            fromTarget = true;
        else if (ask.clickedButton() != fresh)
            return std::nullopt;
    }

    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    QString error;
    std::vector<RebuiltLayer> rebuilt;
    std::optional<QByteArray> file;
    bool plainDiskFile = false;
    if (!needsOriginal && !fromTarget && source.nested.isEmpty() && !source.diskPath.isEmpty()) {
        QFile head(source.diskPath);
        plainDiskFile = head.open(QIODevice::ReadOnly) && detectCompression(head.read(4)) == Compression::None;
    }
    if (plainDiskFile) {
        file = edit(QByteArray(), &error);
    } else {
        const std::optional<QByteArray> original = fromTarget ? readWhole(target, &error) : modSourceBytes(source, &error);
        if (original)
            file = editNested(*original, source.outerName, source.nested, edit, &rebuilt, &error);
    }
    QGuiApplication::restoreOverrideCursor();
    if (!file) {
        QMessageBox::warning(this, title, error);
        return std::nullopt;
    }

    QSaveFile out(target);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !out.open(QIODevice::WriteOnly) || out.write(*file) < 0
        || !out.commit()) {
        QMessageBox::warning(this, title, tr("Could not save %1: %2").arg(target, out.errorString()));
        return std::nullopt;
    }
    ModRecord record = modRecord(modFolder(), source.game);
    record.lastSavedFile = QDir::cleanPath(target);
    record.lastSaved = QDateTime::currentDateTime();
    rememberModInHistory(std::move(record));
    m_info->setText(tr("Saved the mod file %1").arg(QDir::toNativeSeparators(target)));
    ModSave saved;
    saved.rebuilt = std::move(rebuilt);
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    saved.resourceSizes = updateResourceSizes(source, *file, &saved.resourceSizesHandled);
    QGuiApplication::restoreOverrideCursor();
    return saved;
}

std::optional<std::pair<QString, QByteArray>> MainWindow::gameResourceSizeTable(const ModSource &source)
{
    const QString folder = QStringLiteral("System/Resource/");
    if (!source.diskPath.isEmpty()) {
        const QString disk = QDir::fromNativeSeparators(QFileInfo(source.diskPath).absoluteFilePath());
        if (!disk.endsWith(u'/' + source.gamePath))
            return std::nullopt;
        const QDir resources(disk.left(disk.size() - source.gamePath.size()) + folder);
        const QString name = newestRestblName(resources.entryList(QDir::Files));
        if (name.isEmpty())
            return std::nullopt;
        QString error;
        const std::optional<QByteArray> bytes = readWhole(resources.filePath(name), &error);
        return bytes ? std::optional(std::make_pair(folder + name, *bytes)) : std::nullopt;
    }
    const qsizetype romfs = source.packageEntry.indexOf(QLatin1String("/romfs/"), 0, Qt::CaseInsensitive);
    if (romfs < 0)
        return std::nullopt;
    const QString prefix = source.packageEntry.left(romfs + 7) + folder;
    QString e;
    const std::unique_ptr<Document> doc = SwitchPackageReader().read(source.package, &e);
    const std::shared_ptr<const Archive> archive = doc && !doc->tables.empty() ? doc->tables.front()->archive() : nullptr;
    if (!archive)
        return std::nullopt;
    QStringList names;
    for (const ArchiveEntry &entry : archive->entries)
        if (entry.name.startsWith(prefix, Qt::CaseInsensitive) && !entry.name.mid(prefix.size()).contains(u'/'))
            names << entry.name.mid(prefix.size());
    const QString name = newestRestblName(names);
    const int index = name.isEmpty() ? -1 : archive->find(prefix + name);
    if (index < 0)
        return std::nullopt;
    const QByteArray bytes = archive->entryData(size_t(index));
    return bytes.isEmpty() ? std::nullopt : std::optional(std::make_pair(folder + name, bytes));
}

QString MainWindow::updateResourceSizes(const ModSource &source, const QByteArray &file, bool *handled)
{
    *handled = false;
    if (isRestblPath(source.gamePath))
        return {};
    const std::optional<std::pair<QString, QByteArray>> game = gameResourceSizeTable(source);
    if (!game)
        return {};
    *handled = true;
    QString error;
    const std::optional<QByteArray> original = modSourceBytes(source, &error);
    const auto before = original ? unpackedSizes(*original, source.gamePath, source.nested, &error) : std::nullopt;
    const auto after = before ? unpackedSizes(file, source.gamePath, source.nested, &error) : std::nullopt;
    const QString tablePath = QDir(modFolder()).filePath(game->first);
    std::optional<QByteArray> base = game->second;
    if (after && QFileInfo::exists(tablePath))
        base = readWhole(tablePath, &error);
    const std::optional<RestblUpdate> update =
        after && base ? raiseRestbl(game->second, *base, *before, *after, &error) : std::nullopt;
    if (!update)
        return tr("<br><br><b>The resource size table could not be updated</b>: %1 If the game does not load the "
                  "change, the table may need the new size.")
            .arg(error.toHtmlEscaped());
    QString note;
    if (!update->raised.empty()) {
        QSaveFile out(tablePath);
        if (!QDir().mkpath(QFileInfo(tablePath).absolutePath()) || !out.open(QIODevice::WriteOnly)
            || out.write(update->file) < 0 || !out.commit())
            return tr("<br><br><b>The resource size table could not be saved</b>: %1").arg(out.errorString().toHtmlEscaped());
        QStringList raised;
        for (const RestblChange &c : update->raised)
            raised << tr("<tt>%1</tt>: %2 → %3 bytes")
                          .arg(c.path.toHtmlEscaped(), QLocale().toString(c.oldValue), QLocale().toString(c.newValue));
        note += tr("<br><br>The game's resource size table was updated so it sets aside enough memory: %1. Saved as "
                   "<tt>%2</tt>.")
                    .arg(raised.join(u"; "), QDir::toNativeSeparators(tablePath).toHtmlEscaped());
    }
    if (update->unlisted.contains(restblKey(source.gamePath)))
        note += tr("<br><br>%1 grew but is not in the resource size table, so the table was left as it is.")
                    .arg(restblKey(source.gamePath).toHtmlEscaped());
    return note;
}

void MainWindow::showModHelp()
{
    const QString folder = modFolder();
    QMessageBox box(QMessageBox::Information, tr("How to make a mod"),
                    tr("Your changes are saved in a separate mod folder. The game's own files are never changed."),
                    QMessageBox::Ok, this);
    box.setInformativeText(
        tr("<ol>"
           "<li>Open the game: its <tt>romfs</tt> folder dumped from your emulator, or the NSP/XCI file itself.</li>"
           "<li><b>Mod → New mod…</b>: pick your emulator, its data folder and a name.</li>"
           "<li>To change a texture: open it, use <b>Extract → Picture as PNG…</b>, edit the PNG (same width, height "
           "and transparency) and use <b>Mod → Replace texture with PNG…</b></li>"
           "<li>To change values or messages in a BYML or MSBT file: open it, use <b>Extract → Plain JSON…</b>, edit "
           "the JSON and use <b>Mod → Replace with edited JSON…</b></li>"
           "<li>To change any other file: open it and use <b>Mod → Replace file…</b></li>"
           "<li>Turn the mod on in the emulator (yuzu and similar: Properties → Add-ons; Ryujinx: Manage Mods) "
           "and start the game.</li>"
           "</ol>"
           "<b>Mod → My mods…</b> lists your mods: undo a change, pack a mod as a zip to share it, or delete it. "
           "The app only remembers which mods you saved to, for which game and where; the files stay in the mod "
           "folder.<br><br>%1")
            .arg(folder.isEmpty() ? tr("No mod is chosen yet.")
                                  : tr("Saving to: %1<br><tt>%2</tt>")
                                        .arg(modDescription(folder).toHtmlEscaped(),
                                             QDir::toNativeSeparators(folder).toHtmlEscaped())));
    box.exec();
}

QString MainWindow::gamePath(const QString &path) const
{
    const QStringList parts = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()).split(u'/');
    for (qsizetype i = parts.size() - 2; i >= 0; --i)
        if (parts[i].compare(QLatin1String("romfs"), Qt::CaseInsensitive) == 0)
            return parts.mid(i + 1).join(u'/');
    const QString root = m_fsModel->rootPath();
    const QString relative = root.isEmpty() ? QString() : QDir(root).relativeFilePath(path);
    return relative.isEmpty() || relative.startsWith(QLatin1String("..")) ? QFileInfo(path).fileName() : relative;
}

bool MainWindow::isXtxFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == "DFvN";
}

void MainWindow::replaceFileWith(const QString &path, const QStringList &entries)
{
    if (path.isEmpty())
        return;
    const QString title = tr("Replace file");
    if (modFolder().isEmpty() && !chooseModFolder())
        return;
    QString error;
    std::optional<ModSource> source = modSource(path, entries, &error);
    if (!source) {
        QMessageBox::warning(this, title, error);
        return;
    }
    if (!checkModTarget(*source, title))
        return;
    source->target = QDir(modFolder()).filePath(source->gamePath);
    const QString leafName = decompressedName(QFileInfo(entries.isEmpty() ? path : entries.last()).fileName());
    const QString picked = QFileDialog::getOpenFileName(this, tr("Replace %1 with").arg(leafName),
                                                        QDir::home().filePath(leafName), tr("All files (*)"));
    if (picked.isEmpty())
        return;
    std::optional<QByteArray> replacement = readWhole(picked, &error);
    if (replacement && detectCompression(*replacement) != Compression::None)
        replacement = unpackAll(*replacement, nullptr, &error);
    if (!replacement) {
        QMessageBox::warning(this, title, error);
        return;
    }
    const QByteArray contents = *replacement;
    const std::optional<ModSave> saved =
        saveToMod(*source, title, [contents](const QByteArray &, QString *) { return std::optional<QByteArray>(contents); },
                  !source->nested.isEmpty(), false);
    if (!saved)
        return;
    QMessageBox box(QMessageBox::Information, title, tr("Saved %1 in the mod folder.").arg(source->gamePath),
                    QMessageBox::Ok, this);
    box.setInformativeText(tr("%1 now holds the contents of <tt>%2</tt>.")
                               .arg(leafName.toHtmlEscaped(), QDir::toNativeSeparators(picked).toHtmlEscaped())
                           + rebuiltDetail(saved->rebuilt, leafName, saved->resourceSizesHandled) + saved->resourceSizes
                           + tr("<br><br>Saved as <tt>%1</tt>").arg(QDir::toNativeSeparators(source->target).toHtmlEscaped())
                           + QStringLiteral("<br><br>") + turnOnHint(modFolder()));
    QPushButton *show = box.addButton(tr("Show file"), QMessageBox::ActionRole);
    show->setToolTip(tr("Show the saved mod file in your file manager"));
    box.exec();
    if (box.clickedButton() == show)
        showInFileManager(source->target);
}

void MainWindow::replaceTextureWithPng(const QString &path, const QStringList &entries, const TextureSlot &slot)
{
    if (path.isEmpty())
        return;
    const QString title = tr("Replace texture");
    if (modFolder().isEmpty() && !chooseModFolder())
        return;
    QString error;
    std::optional<ModSource> source = modSource(path, entries, &error);
    if (!source) {
        QMessageBox::warning(this, title, error);
        return;
    }
    if (!checkModTarget(*source, title))
        return;
    source->target = QDir(modFolder()).filePath(source->gamePath);
    const QString leafName = decompressedName(QFileInfo(entries.isEmpty() ? path : entries.last()).fileName());
    const QString textureName = slot.name.isEmpty() ? leafName : slot.name;
    const QString suggested = QDir::home().filePath(
        (slot.name.isEmpty() ? QFileInfo(leafName).completeBaseName() : slot.name) + ".png");
    const QString png = QFileDialog::getOpenFileName(this, tr("Replace %1 with").arg(textureName), suggested,
                                                     tr("Pictures (*.png *.webp *.bmp *.tga)"));
    if (png.isEmpty())
        return;
    QImage picture(png);
    if (picture.isNull()) {
        QMessageBox::warning(this, title, tr("%1 could not be read as a picture.").arg(png));
        return;
    }
    picture = picture.convertToFormat(QImage::Format_RGBA8888);
    Image image{picture.width(), picture.height(), QByteArray(qsizetype(picture.width()) * picture.height() * 4, '\0')};
    for (int y = 0; y < picture.height(); ++y)
        std::memcpy(image.rgba.data() + qsizetype(y) * picture.width() * 4, picture.constScanLine(y),
                    size_t(picture.width()) * 4);

    std::optional<TextureReplacement> texture;
    const bool askEarlier = !source->nested.isEmpty() || source->diskPath.isEmpty() || !isXtxFile(source->diskPath);
    const std::optional<ModSave> saved = saveToMod(
        *source, title,
        [&](const QByteArray &leaf, QString *e) -> std::optional<QByteArray> {
            texture = replaceTextureImage(leaf, slot, image, e);
            return texture ? std::optional<QByteArray>(texture->file) : std::nullopt;
        },
        askEarlier);
    if (!saved || !texture)
        return;

    const qsizetype total = texture->reusedBlocks + texture->encodedBlocks;
    QString detail = texture->encodedBlocks == 0
                         ? tr("All %n block(s) are the game's own data, so nothing lost quality.", nullptr, int(total))
                         : tr("%1 of %2 blocks were kept exactly as in the game; %3 changed block(s) were compressed "
                              "again as %4.")
                               .arg(texture->reusedBlocks)
                               .arg(total)
                               .arg(texture->encodedBlocks)
                               .arg(texture->formatName);
    if (texture->mipLevels > 1)
        detail += tr(" The %1 smaller mip levels were made again from the picture where it changed; everywhere else "
                     "they are the game's own.")
                      .arg(texture->mipLevels - 1);
    detail += rebuiltDetail(saved->rebuilt, textureName, saved->resourceSizesHandled) + saved->resourceSizes;
    QMessageBox box(QMessageBox::Information, title, tr("Saved %1 in the mod folder.").arg(source->gamePath),
                    QMessageBox::Ok, this);
    box.setInformativeText(detail + tr("<br><br>Saved as <tt>%1</tt>").arg(QDir::toNativeSeparators(source->target).toHtmlEscaped())
                           + QStringLiteral("<br><br>") + turnOnHint(modFolder()));
    QPushButton *show = box.addButton(tr("Show file"), QMessageBox::ActionRole);
    QPushButton *view = box.addButton(tr("Open it"), QMessageBox::ActionRole);
    show->setToolTip(tr("Show the saved mod file in your file manager"));
    view->setToolTip(tr("Open the saved mod file in the viewer to check the new texture"));
    box.exec();
    if (box.clickedButton() == show)
        showInFileManager(source->target);
    else if (box.clickedButton() == view)
        openLocation(source->target, source->nested, true);
}

void MainWindow::replaceWithEditedJson(const QString &path, const QStringList &entries)
{
    if (path.isEmpty())
        return;
    const QString title = tr("Replace with edited JSON");
    if (modFolder().isEmpty() && !chooseModFolder())
        return;
    QString error;
    std::optional<ModSource> source = modSource(path, entries, &error);
    if (!source) {
        QMessageBox::warning(this, title, error);
        return;
    }
    if (!checkModTarget(*source, title))
        return;
    source->target = QDir(modFolder()).filePath(source->gamePath);
    const QString leafName = decompressedName(QFileInfo(entries.isEmpty() ? path : entries.last()).fileName());
    const QString picked = QFileDialog::getOpenFileName(this, tr("Put edited JSON into %1").arg(leafName),
                                                        QDir::home().filePath(QFileInfo(leafName).completeBaseName() + ".json"),
                                                        tr("JSON files (*.json);;All files (*)"));
    if (picked.isEmpty())
        return;
    const std::optional<QByteArray> json = readWhole(picked, &error);
    if (!json) {
        QMessageBox::warning(this, title, error);
        return;
    }
    const QByteArray contents = *json;
    const std::optional<ModSave> saved = saveToMod(
        *source, title,
        [contents](const QByteArray &leaf, QString *e) -> std::optional<QByteArray> {
            if (!canApplyEditedJson(leaf)) {
                if (e)
                    *e = QObject::tr("Only BYML and MSBT files can be changed from JSON.");
                return std::nullopt;
            }
            return applyEditedJson(leaf, contents, e);
        },
        !source->nested.isEmpty(), true);
    if (!saved)
        return;
    QMessageBox box(QMessageBox::Information, title, tr("Saved %1 in the mod folder.").arg(source->gamePath),
                    QMessageBox::Ok, this);
    box.setInformativeText(tr("%1 now holds the values from <tt>%2</tt>. Values keep the types they have in the game's "
                              "file.")
                               .arg(leafName.toHtmlEscaped(), QDir::toNativeSeparators(picked).toHtmlEscaped())
                           + rebuiltDetail(saved->rebuilt, leafName, saved->resourceSizesHandled) + saved->resourceSizes
                           + tr("<br><br>Saved as <tt>%1</tt>").arg(QDir::toNativeSeparators(source->target).toHtmlEscaped())
                           + QStringLiteral("<br><br>") + turnOnHint(modFolder()));
    QPushButton *show = box.addButton(tr("Show file"), QMessageBox::ActionRole);
    QPushButton *view = box.addButton(tr("Open it"), QMessageBox::ActionRole);
    show->setToolTip(tr("Show the saved mod file in your file manager"));
    view->setToolTip(tr("Open the saved mod file in the viewer to check the change"));
    box.exec();
    if (box.clickedButton() == show)
        showInFileManager(source->target);
    else if (box.clickedButton() == view)
        openLocation(source->target, source->nested, true);
}
