#include "mainwindow.h"
#include "progressdialogs.h"
#include "audioplayer.h"
#include "formats/archives/compress.h"
#include "formats/core/paths.h"
#include "tablemodel.h"

#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPromise>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrentRun>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr qsizetype kMaxSearchMatches = 2000;

}

void MainWindow::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir()) {
        openFolder(info.absoluteFilePath());
    } else if (info.isFile()) {
        openFolder(info.absolutePath());
        const QModelIndex index = m_fsModel->index(info.absoluteFilePath());
        if (index.isValid() && m_tree->currentIndex() != index)
            m_tree->setCurrentIndex(index);
        else
            openFile(info.absoluteFilePath());
        m_revealCurrent = true;
        revealCurrentFile();
    }
}

void MainWindow::openFileWith(const QString &path, int formatId)
{
    {
        const QSignalBlocker blocker(m_formatBox);
        selectFormat(formatId);
    }
    const QModelIndex index = m_fsModel->index(path);
    if (index.isValid() && m_tree->currentIndex() != index)
        m_tree->setCurrentIndex(index);
    else
        openFile(path);
}

int MainWindow::forcedFormat() const
{
    return m_formatBox ? std::max(0, m_formatBox->currentData().toInt()) : 0;
}

QString MainWindow::formatFamily(const QString &readerName)
{
    static const QStringList nintendo = {"SARC archive", "BYML data", "BCSV table", "MSBT messages",
                                         "Nintendo Opus sound"};
    if (readerName.startsWith(QLatin1String("Switch ")) || nintendo.contains(readerName))
        return tr("Nintendo Switch");
    if (readerName.startsWith(QLatin1String("RAGE ")))
        return tr("Rockstar (RAGE)");
    if (readerName.startsWith(QLatin1String("Falcom ")))
        return tr("Falcom");
    if (readerName.startsWith(QLatin1String("Unity ")))
        return tr("Unity");
    if (readerName.startsWith(QLatin1String("CRI ")))
        return tr("CRI");
    return tr("Common files");
}

void MainWindow::addGroupedFormats(QMenu *menu, const QList<int> &skip, int checked,
                                   const std::function<void(int formatId)> &pick)
{
    const auto &readers = m_registry->readers();
    QStringList families;
    QMap<QString, QList<int>> byFamily;
    for (size_t i = 0; i < readers.size(); ++i) {
        const int id = int(i) + 1;
        if (skip.contains(id))
            continue;
        const QString family = formatFamily(readers[i]->name());
        if (!families.contains(family))
            families << family;
        byFamily[family] << id;
    }
    std::stable_sort(families.begin(), families.end(), [this](const QString &a, const QString &b) {
        return (a == tr("Common files")) < (b == tr("Common files"));
    });
    for (const QString &family : families) {
        QMenu *sub = menu->addMenu(family);
        QList<int> ids = byFamily.value(family);
        std::sort(ids.begin(), ids.end(), [&readers](int a, int b) {
            return readers[size_t(a - 1)]->name().compare(readers[size_t(b - 1)]->name(), Qt::CaseInsensitive) < 0;
        });
        for (int id : ids) {
            QAction *action = sub->addAction(readers[size_t(id - 1)]->name(), this, [pick, id] { pick(id); });
            action->setCheckable(true);
            action->setChecked(id == checked);
        }
    }
}

void MainWindow::selectFormat(int formatId)
{
    int index = m_formatBox->findData(formatId);
    if (index < 0 && formatId > 0 && size_t(formatId) <= m_registry->readers().size()) {
        const int other = m_formatBox->findData(-1);
        const int at = other < 0 ? m_formatBox->count() : std::max(1, other - 1);
        m_formatBox->insertItem(at, m_registry->readers()[size_t(formatId - 1)]->name(), formatId);
        index = at;
    }
    m_formatBox->setCurrentIndex(std::max(0, index));
    m_shownFormatChoice = m_formatBox->currentIndex();
}

void MainWindow::refreshFormatChoices()
{
    const auto &readers = m_registry->readers();
    auto idOf = [&readers](const FormatReader *reader) {
        for (size_t i = 0; i < readers.size(); ++i)
            if (readers[i].get() == reader)
                return int(i) + 1;
        return 0;
    };
    QList<int> ids;
    auto add = [&ids](int id) {
        if (id > 0 && !ids.contains(id))
            ids << id;
    };
    std::vector<const FormatReader *> fitting;
    if (m_currentEntries.isEmpty() && !m_currentPath.isEmpty()) {
        fitting = m_registry->candidates(m_currentPath);
    } else if (m_parentArchive && !m_currentEntries.isEmpty()) {
        const int index = m_parentArchive->find(m_currentEntries.last());
        if (index >= 0)
            fitting = m_registry->candidates(m_currentEntries.last(), m_parentArchive->entryHead(size_t(index), 64), true);
    }
    for (const FormatReader *reader : fitting)
        if (reader->name() != QLatin1String("Hex"))
            add(idOf(reader));
    if (const FormatReader *used = m_registry->byName(m_currentFormat))
        if (used->name() != QLatin1String("Hex"))
            add(idOf(used));
    const int forced = forcedFormat();
    add(forced);
    add(idOf(m_registry->byName(QStringLiteral("Hex"))));

    const QSignalBlocker blocker(m_formatBox);
    m_formatBox->clear();
    m_formatBox->addItem(m_currentFormat.isEmpty() || forced != 0 ? tr("Auto-detect")
                                                                  : tr("Auto-detect (%1)").arg(m_currentFormat),
                         0);
    for (int id : ids)
        m_formatBox->addItem(readers[size_t(id - 1)]->name(), id);
    m_formatBox->insertSeparator(m_formatBox->count());
    m_formatBox->addItem(tr("Other formats…"), -1);
    m_formatBox->setItemData(m_formatBox->count() - 1,
                             tr("Pick from every format the app knows, grouped by game family"), Qt::ToolTipRole);
    selectFormat(forced);
}

void MainWindow::chooseOtherFormat()
{
    QList<int> listed;
    for (int i = 0; i < m_formatBox->count(); ++i)
        listed << m_formatBox->itemData(i).toInt();
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    int picked = 0;
    addGroupedFormats(&menu, listed, forcedFormat(), [&picked](int id) { picked = id; });
    menu.exec(m_formatBox->mapToGlobal(QPoint(0, m_formatBox->height())));
    if (picked <= 0)
        return;
    const int previous = forcedFormat();
    {
        const QSignalBlocker blocker(m_formatBox);
        selectFormat(picked);
        selectFormat(previous);
    }
    m_formatBox->setCurrentIndex(m_formatBox->findData(picked));
}

void MainWindow::showFileMenu(const QPoint &pos)
{
    const QModelIndex index = m_tree->indexAt(pos);
    if (!index.isValid())
        return;
    const QString path = m_fsModel->filePath(index);
    const QFileInfo info(path);
    QStringList exportPaths{path};
    if (m_tree->selectionModel()->isRowSelected(index.row(), index.parent()))
        exportPaths = selectedBrowserPaths();

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    if (info.isDir()) {
        menu.addAction(tr("Browse this folder"), this, [this, path] { openFolder(path); })
            ->setToolTip(tr("Show only this folder in the file list"));
        menu.addAction(tr("Open in file manager"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); })
            ->setToolTip(tr("Open this folder in your system's file manager"));
    } else {
        QMenu *openAs = menu.addMenu(tr("Open as"));
        openAs->setToolTipsVisible(true);
        openAs->menuAction()->setToolTip(tr("Open the file as a specific format, when auto-detect picks the wrong one"));
        auto *group = new QActionGroup(openAs);
        const bool shown = path == m_currentPath;
        const int forced = forcedFormat();
        auto choice = [&](QMenu *into, const QString &text, int formatIndex, bool checked) {
            QAction *action = into->addAction(text, this, [this, path, formatIndex] { openFileWith(path, formatIndex); });
            action->setCheckable(true);
            action->setChecked(checked);
            group->addAction(action);
        };
        choice(openAs,
               shown && forced == 0 && !m_currentFormat.isEmpty() ? tr("Auto-detect (%1)").arg(m_currentFormat)
                                                                   : tr("Auto-detect"),
               0, shown && forced == 0);
        openAs->addSeparator();
        const auto &readers = m_registry->readers();
        QList<int> fittingIds;
        for (const FormatReader *reader : m_registry->candidates(path))
            for (size_t i = 0; i < readers.size(); ++i)
                if (readers[i].get() == reader)
                    fittingIds << int(i) + 1;
        for (int id : fittingIds)
            choice(openAs, readers[size_t(id - 1)]->name(), id, shown && forced == id);
        auto *others = new QMenu(tr("Other formats"), openAs);
        others->menuAction()->setToolTip(tr("Formats that do not seem to match this file. Opening it with one may fail"));
        others->setToolTipsVisible(true);
        addGroupedFormats(others, fittingIds, shown ? forced : 0, [this, path](int id) { openFileWith(path, id); });
        if (!others->isEmpty())
            openAs->addMenu(others);
        if (isXtxFile(path))
            menu.addAction(tr("Replace with PNG…"), this, [this, path] { replaceTextureWithPng(path); })
                ->setToolTip(tr("Replace this texture with a PNG picture and save the result in the mod folder"));
        menu.addAction(tr("Open with system default app"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); })
            ->setToolTip(tr("Open the file in the app your system uses for this file type"));
        menu.addAction(tr("Show in file manager"), this, [this, path] { showInFileManager(path); })
            ->setToolTip(tr("Show the file in your system's file manager"));
    }
    menu.addSeparator();
    const QString exportText = exportPaths.size() > 1 ? tr("Extract %n items…", nullptr, int(exportPaths.size()))
                             : info.isDir()           ? tr("Extract folder…")
                                                      : tr("Extract…");
    QAction *exportAction = menu.addAction(exportText, this, [this, exportPaths] { extractPaths(exportPaths); });
    exportAction->setEnabled(!m_extractWatcher);
    exportAction->setToolTip(tr("Save to a folder, opening archives and converting files to common formats such as "
                                "PNG, WAV and glTF where possible"));
    menu.addSeparator();
    menu.addAction(tr("Copy path"), this,
                   [path] { QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path)); })
        ->setToolTip(tr("Copy the full path to the clipboard"));
    menu.addAction(tr("Copy name"), this, [info] { QGuiApplication::clipboard()->setText(info.fileName()); })
        ->setToolTip(tr("Copy only the name to the clipboard"));
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

QStringList MainWindow::selectedBrowserPaths() const
{
    QStringList paths;
    for (const QModelIndex &index : m_tree->selectionModel()->selectedRows(0))
        paths << m_fsModel->filePath(index);
    if (paths.isEmpty() && m_tree->currentIndex().isValid())
        paths << m_fsModel->filePath(m_tree->currentIndex());
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

void MainWindow::startFileSearch()
{
    if (m_searchCancel)
        m_searchCancel->store(true);
    m_matches.clear();
    m_matchIndex = -1;
    m_pendingMatch.clear();
    const QString text = m_fileFilter->text().trimmed();
    const QString root = m_fsModel->rootPath();
    if (text.isEmpty() || root.isEmpty() || !m_tree->isVisible()) {
        m_fsModel->setNameFilters({});
        m_searchExpanded.clear();
        m_searchStatus->hide();
        return;
    }
    m_searchStatus->setText(tr("Searching…"));
    m_searchStatus->show();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_searchCancel = cancel;
    const QRegularExpression pattern =
        QRegularExpression::fromWildcard(QLatin1Char('*') + text + QLatin1Char('*'), Qt::CaseInsensitive);
    m_searchWatcher->setFuture(QtConcurrent::run([root, pattern, cancel, text] {
        QStringList found;
        QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext() && !cancel->load()) {
            it.next();
            if (pattern.match(it.fileName()).hasMatch()) {
                found << it.filePath();
                if (found.size() > kMaxSearchMatches)
                    break;
            }
        }
        auto rank = [&text](const QString &path) {
            const QString name = path.section(u'/', -1);
            const int start = name.startsWith(text, Qt::CaseInsensitive) ? 0 : 1;
            return std::make_tuple(start, name.size(), name.toLower(), path);
        };
        std::sort(found.begin(), found.end(), [&rank](const QString &a, const QString &b) { return rank(a) < rank(b); });
        return found;
    }));
}

void MainWindow::revealMatch(const QString &path, bool open)
{
    const QString root = m_fsModel->rootPath();
    if (!path.startsWith(root))
        return;
    m_revealCurrent = false;
    const QStringList parts = path.mid(root.size()).split(u'/', Qt::SkipEmptyParts);
    QString folder = root;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        folder += (folder.endsWith(u'/') ? QString() : QStringLiteral("/")) + parts[i];
        const QModelIndex folderIndex = m_fsModel->index(folder);
        if (!m_tree->isExpanded(folderIndex)) {
            m_tree->expand(folderIndex);
            m_searchExpanded << folder;
        }
    }
    const QModelIndex index = m_fsModel->index(path);
    if (!index.isValid())
        return;
    if (open)
        m_tree->setCurrentIndex(index);
    else
        m_tree->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_pendingMatch = path;
    scrollIntoView(index);
}

void MainWindow::scrollIntoView(const QModelIndex &index)
{
    const QRect rect = m_tree->visualRect(index);
    if (rect.isEmpty() || !m_tree->viewport()->rect().contains(rect))
        m_tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void MainWindow::revealPendingMatch()
{
    if (m_pendingMatch.isEmpty() || m_matchIndex < 0 || m_matchIndex >= m_matches.size()
        || m_matches[m_matchIndex] != m_pendingMatch)
        return;
    const QModelIndex index = m_fsModel->index(m_pendingMatch);
    if (!index.isValid())
        return;
    if (!m_tree->selectionModel()->isSelected(index))
        m_tree->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    scrollIntoView(index);
}

void MainWindow::updateSearchStatus()
{
    if (m_matches.isEmpty()) {
        m_searchStatus->setText(tr("No matches"));
    } else {
        const QString total = m_searchTruncated ? tr("%1+").arg(m_matches.size()) : QString::number(m_matches.size());
        m_searchStatus->setText(tr("%1 of %2").arg(m_matchIndex + 1).arg(total));
    }
    m_searchStatus->show();
}

void MainWindow::nextMatch()
{
    if (m_searchTimer->isActive() || m_searchWatcher->isRunning())
        return;
    if (m_matches.isEmpty())
        return;
    m_matchIndex = (m_matchIndex + 1) % int(m_matches.size());
    revealMatch(m_matches[m_matchIndex], true);
    updateSearchStatus();
}

void MainWindow::searchFinished()
{
    if (m_searchWatcher->isCanceled())
        return;
    m_matches = m_searchWatcher->result();
    m_searchTruncated = m_matches.size() > kMaxSearchMatches;
    if (m_searchTruncated)
        m_matches.resize(kMaxSearchMatches);
    m_matchIndex = m_matches.isEmpty() ? -1 : 0;
    m_tree->setUpdatesEnabled(false);
    const QString text = m_fileFilter->text().trimmed();
    m_fsModel->setNameFilters(text.isEmpty() ? QStringList() : QStringList{'*' + text + '*'});
    for (auto it = m_searchExpanded.crbegin(); it != m_searchExpanded.crend(); ++it)
        m_tree->collapse(m_fsModel->index(*it));
    m_searchExpanded.clear();
    if (!m_matches.isEmpty())
        revealMatch(m_matches.front(), false);
    m_tree->setUpdatesEnabled(true);
    updateSearchStatus();
}

void MainWindow::revealCurrentFile()
{
    if (!m_revealCurrent)
        return;
    const QModelIndex current = m_tree->currentIndex();
    if (current.isValid() && m_tree->rootIndex().isValid() && m_fsModel->filePath(current).startsWith(m_fsModel->rootPath()))
        m_tree->scrollTo(current, QAbstractItemView::PositionAtCenter);
}

void MainWindow::showInFileManager(const QString &path)
{
    const QFileInfo info(path);
#if defined(Q_OS_WIN)
    if (QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"), QDir::toNativeSeparators(info.absoluteFilePath())}))
        return;
#elif defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), info.absoluteFilePath()}))
        return;
#elif defined(HAVE_QT_DBUS)
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call << QStringList{QUrl::fromLocalFile(info.absoluteFilePath()).toString()} << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ReplyMessage)
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

QString MainWindow::lastLocation() const
{
    const QString last = QSettings().value("lastLocation").toString();
    return QFileInfo::exists(last) ? last : QDir::homePath();
}

void MainWindow::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open folder"), lastLocation());
    if (!folder.isEmpty())
        openFolder(folder);
}

void MainWindow::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open file"), lastLocation());
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::openFolder(const QString &folder)
{
    QSettings().setValue("lastLocation", folder);
    m_fileFilter->clear();
    const QString previous = m_fsModel->filePath(m_tree->currentIndex());
    m_tree->collapseAll();
    m_tree->setRootIndex(m_fsModel->setRootPath(folder));
    const QString root = m_fsModel->rootPath();
    QString cameFrom;
    for (QString path = previous; !path.isEmpty();) {
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == root)
            cameFrom = path;
        if (parent == root || parent == path)
            break;
        path = parent;
    }
    m_folderEdit->setText(QDir::toNativeSeparators(folder));
    m_upAction->setEnabled(!QDir(folder).isRoot());
    m_tree->show();
    m_noFolder->hide();
    m_browserBar->show();
    setWindowTitle(tr("NX Game Viewer — %1").arg(folder));
    showFolder(folder);
    m_tree->setCurrentIndex(cameFrom.isEmpty() ? QModelIndex() : m_fsModel->index(cameFrom));
    m_revealCurrent = true;
    revealCurrentFile();
}

void MainWindow::showFolder(const QString &folder)
{
    m_nextPath.reset();
    m_discardLoad = isLoading();
    m_loadingTimer->stop();
    m_model->clear();
    m_shownTable.reset();
    {
        const QSignalBlocker blocker(m_tableBox);
        m_tableBox->clear();
    }
    m_document.reset();
    m_imageTable.reset();
    m_currentPath.clear();
    m_currentEntries.clear();
    m_currentFormat.clear();
    m_parentArchive.reset();
    m_autoEntries = 0;
    updateLocationBar();

    int files = 0, folders = 0;
    qint64 bytes = 0;
    QHash<QString, int> types;
    for (QDirIterator it(folder, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden); it.hasNext();) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            ++folders;
            continue;
        }
        ++files;
        bytes += info.size();
        const QString name = info.fileName();
        const qsizetype dot = name.lastIndexOf(u'.');
        ++types[dot > 0 ? name.mid(dot).toLower() : tr("(no extension)")];
    }
    QList<std::pair<QString, int>> sorted;
    for (auto it = types.cbegin(); it != types.cend(); ++it)
        sorted.append({it.key(), it.value()});
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QStringList common;
    for (qsizetype i = 0; i < std::min<qsizetype>(sorted.size(), 5); ++i)
        common << QStringLiteral("%1 (%2)").arg(sorted[i].first.toHtmlEscaped(), QLocale().toString(sorted[i].second));

    const QString name = QFileInfo(folder).fileName().isEmpty() ? folder : QFileInfo(folder).fileName();
    QString text = QStringLiteral("<h3>%1</h3><p style=\"color: gray\">%2</p>")
                       .arg(name.toHtmlEscaped(), QDir::toNativeSeparators(folder).toHtmlEscaped());
    if (files == 0 && folders == 0) {
        text += tr("<p>This folder is empty.</p>");
    } else {
        text += tr("<p>%1 · %2 · %3 in this folder</p>")
                    .arg(files == 1 ? tr("1 file") : tr("%1 files").arg(QLocale().toString(files)),
                         folders == 1 ? tr("1 folder") : tr("%1 folders").arg(QLocale().toString(folders)),
                         QLocale().formattedDataSize(bytes));
        if (!common.isEmpty())
            text += tr("<p>Most common: %1</p>").arg(common.join(QStringLiteral(" · ")));
        text += tr("<p style=\"color: gray\">Select a file to view it, or right-click and choose Extract to save this folder.</p>");
    }
    showMessage(text);
    m_info->setText(QDir::toNativeSeparators(folder));
}

void MainWindow::openFile(const QString &path)
{
    openLocation(path, {}, true);
}

void MainWindow::openLocation(const QString &path, const QStringList &entries, bool descend)
{
    m_audioPlayer->stop();
    m_nextPath = path;
    m_discardLoad = false;
    m_nextEntries = entries;
    m_nextDescend = descend;
    if (!isLoading())
        startNextLoad();
}

std::shared_ptr<const Archive> MainWindow::currentArchive() const
{
    const Table *table = currentTable();
    return table ? table->archive() : nullptr;
}

void MainWindow::openArchiveEntry(int row)
{
    const auto archive = currentArchive();
    if (!archive || row < 0 || size_t(row) >= archive->entries.size())
        return;
    openLocation(m_currentPath, m_currentEntries + QStringList{archive->entries[size_t(row)].name}, true);
}

QList<int> MainWindow::selectedArchiveRows() const
{
    QList<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
        rows << m_proxy->mapToSource(index).row();
    if (rows.isEmpty())
        for (const QModelIndex &index : m_table->selectionModel()->selectedIndexes())
            if (!rows.contains(m_proxy->mapToSource(index).row()))
                rows << m_proxy->mapToSource(index).row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

void MainWindow::showTableMenu(const QPoint &pos)
{
    const auto archive = currentArchive();
    const QModelIndex index = m_table->indexAt(pos);
    if (!archive || !index.isValid())
        return;
    const int row = m_proxy->mapToSource(index).row();
    const QList<int> selected = selectedArchiveRows();
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    menu.addAction(tr("Open"), this, [this, row] { openArchiveEntry(row); })
        ->setToolTip(tr("View this file from inside the archive"));
    QAction *extractSelected =
        menu.addAction(selected.size() > 1 ? tr("Extract %n files…", nullptr, int(selected.size())) : tr("Extract…"),
                       this, [this] { extractArchiveRows(true); });
    extractSelected->setToolTip(tr("Save the selected files to a folder, converted to common formats such as PNG, WAV "
                                   "and glTF where possible"));
    QAction *extractAll = menu.addAction(tr("Extract all…"), this, [this] { extractArchiveRows(false); });
    extractAll->setToolTip(tr("Save every file in this archive to a folder, converted where possible"));
    extractSelected->setEnabled(!m_extractWatcher);
    extractAll->setEnabled(!m_extractWatcher);
    if (selected.size() == 1) {
        const QStringList target = m_currentEntries + QStringList{archive->entries[size_t(row)].name};
        menu.addAction(tr("Replace with file…"), this, [this, target] { replaceFileWith(m_currentPath, target); })
            ->setToolTip(tr("Save a file you pick in the mod folder in place of this one"));
    }
    const QByteArray head = archive->entryHead(size_t(row), 4);
    if (head == "DFvN" || detectCompression(head) != Compression::None) {
        const std::optional<QByteArray> inner = detectCompression(head) == Compression::None
                                                    ? std::optional<QByteArray>(head)
                                                    : unpackAll(archive->entryData(size_t(row)), nullptr, nullptr);
        if (inner && inner->startsWith("DFvN")) {
            menu.addSeparator();
            const QStringList entries = m_currentEntries + QStringList{archive->entries[size_t(row)].name};
            menu.addAction(tr("Replace with PNG…"), this, [this, entries] { replaceTextureWithPng(m_currentPath, entries); })
                ->setToolTip(tr("Replace this texture with a PNG picture and save the result in the mod folder"));
        }
    }
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::extractArchiveRows(bool selectionOnly)
{
    const auto archive = currentArchive();
    if (!archive || m_extractWatcher)
        return;
    QList<int> rows = selectionOnly ? selectedArchiveRows() : QList<int>();
    if (rows.isEmpty())
        for (int i = 0; i < int(archive->entries.size()); ++i)
            rows << i;
    if (rows.isEmpty())
        return;
    const QString what = rows.size() == 1 ? archive->entries[size_t(rows.first())].name.section(u'/', -1)
                         : rows.size() == int(archive->entries.size())
                             ? tr("All %n file(s) in %1", nullptr, int(rows.size())).arg(currentName())
                             : tr("%n file(s) from %1", nullptr, int(rows.size())).arg(currentName());
    const std::optional<BatchOptions> options = chooseExtractOptions(what, {});
    if (!options)
        return;
    QStringList chain{QFileInfo(m_currentPath).fileName()};
    chain += m_currentEntries;
    startExtraction(archiveExtractItems(archive, rows, m_currentPath, chain.join(QStringLiteral(" › ")), *options),
                    *options);
}
