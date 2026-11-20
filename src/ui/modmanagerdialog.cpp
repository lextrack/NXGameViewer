#include "modmanagerdialog.h"
#include "modhistory.h"
#include "progressdialogs.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <numeric>

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;

}

ModManagerDialog::ModManagerDialog(const QStringList &dataFolders, const QString &activeMod, QWidget *parent)
    : QDialog(parent), m_dataFolders(dataFolders), m_active(QDir::cleanPath(activeMod))
{
    setWindowTitle(tr("Manage mods"));
    resize(1000, 620);

    auto *intro = new QLabel(tr("Mods you saved changes to, and mods found in your emulators' folders. The app keeps "
                                "only this list (ID, game and where each mod was last saved): the mod files themselves "
                                "stay in the emulator's folder."));
    intro->setWordWrap(true);

    m_list = new QTreeWidget;
    m_list->setHeaderLabels({tr("ID"), tr("Mod"), tr("Game"), tr("Emulator"), tr("Location"), tr("Last saved")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        showFiles();
        updateButtons();
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (m_use->isEnabled())
            m_use->click();
    });

    m_files = new QTreeWidget;
    m_files->setHeaderLabels({tr("Changed file"), tr("Size"), tr("Modified")});
    m_files->setRootIsDecorated(false);
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_files->header()->setStretchLastSection(false);
    connect(m_files, &QTreeWidget::itemSelectionChanged, this, &ModManagerDialog::updateButtons);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(m_list);
    splitter->addWidget(m_files);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    m_use = new QPushButton(tr("Use for saving"));
    m_use->setToolTip(tr("Save the files you change from now on in this mod"));
    connect(m_use, &QPushButton::clicked, this, [this] {
        if (const auto mod = selectedMod()) {
            m_chosen = mod->folder;
            accept();
        }
    });
    m_open = new QPushButton(tr("Open folder"));
    m_open->setToolTip(tr("Show the mod's folder in your file manager"));
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (const auto mod = selectedMod())
            QDesktopServices::openUrl(QUrl::fromLocalFile(modRoot(mod->folder)));
    });
    m_zip = new QPushButton(tr("Pack as zip…"));
    m_zip->setToolTip(tr("Make a zip of the mod to share it, laid out the way people install mods"));
    connect(m_zip, &QPushButton::clicked, this, &ModManagerDialog::chooseZip);
    m_delete = new QPushButton(tr("Delete mod…"));
    m_delete->setToolTip(tr("Move the whole mod folder to the trash"));
    connect(m_delete, &QPushButton::clicked, this, &ModManagerDialog::deleteSelectedMod);
    m_forget = new QPushButton(tr("Remove from list"));
    m_forget->setToolTip(tr("Forget this entry. The mod's files are not touched"));
    connect(m_forget, &QPushButton::clicked, this, &ModManagerDialog::forgetSelectedMod);
    auto *clearMissing = new QPushButton(tr("Clear missing"));
    clearMissing->setToolTip(tr("Forget the entries whose folder no longer exists"));
    connect(clearMissing, &QPushButton::clicked, this, &ModManagerDialog::forgetMissing);
    m_undo = new QPushButton(tr("Undo selected changes…"));
    m_undo->setToolTip(tr("Move the selected changed files to the trash, so the game uses its own files again"));
    connect(m_undo, &QPushButton::clicked, this, &ModManagerDialog::removeSelectedFiles);

    auto *modRow = new QHBoxLayout;
    for (QPushButton *b : {m_use, m_open, m_zip, m_delete, m_forget, clearMissing})
        modRow->addWidget(b);
    modRow->addStretch();
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_undo);
    fileRow->addStretch();

    m_status = new QLabel;
    m_status->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(modRow);
    layout->addWidget(splitter, 1);
    layout->addLayout(fileRow);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    QString select;
    for (const ModRecord &r : loadModHistory())
        if (QDir::cleanPath(r.folder) == m_active)
            select = r.id;
    reload(select.isEmpty() && !m_active.isEmpty() ? modId(m_active) : select);
}

void ModManagerDialog::setConfirmation(std::function<bool(const QString &, const QString &)> confirm)
{
    m_confirm = std::move(confirm);
}

bool ModManagerDialog::confirm(const QString &question, const QString &details)
{
    if (m_confirm)
        return m_confirm(question, details);
    QMessageBox box(QMessageBox::Question, windowTitle(), question, QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setInformativeText(details);
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Yes;
}

int ModManagerDialog::modCount() const
{
    return int(m_mods.size());
}

void ModManagerDialog::reload(const QString &selectId)
{
    const std::vector<ModRecord> history = loadModHistory();
    m_mods = history;
    m_inHistory.assign(history.size(), true);
    for (ModRecord &found : findInstalledMods(m_dataFolders)) {
        const bool known = std::any_of(m_mods.begin(), m_mods.end(), [&](const ModRecord &r) { return r.id == found.id; });
        if (!known) {
            m_mods.push_back(std::move(found));
            m_inHistory.push_back(false);
        }
    }
    m_list->clear();
    const QLocale locale;
    for (size_t i = 0; i < m_mods.size(); ++i) {
        const ModRecord &r = m_mods[i];
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, r.id);
        item->setText(1, r.name);
        item->setText(2, r.game.isEmpty() ? r.titleId : QStringLiteral("%1 [%2]").arg(r.game, r.titleId));
        item->setText(3, r.emulatorName());
        item->setText(4, QDir::toNativeSeparators(modRoot(r.folder)));
        item->setToolTip(4, QDir::toNativeSeparators(r.folder));
        item->setText(5, r.lastSaved.isValid() ? locale.toString(r.lastSaved, QLocale::ShortFormat)
                                               : (m_inHistory[i] ? tr("not yet") : tr("found in emulator folder")));
        if (!r.lastSavedFile.isEmpty())
            item->setToolTip(5, tr("Last saved: %1").arg(QDir::toNativeSeparators(r.lastSavedFile)));
        item->setData(0, kIndexRole, int(i));
        if (QDir::cleanPath(r.folder) == m_active) {
            QFont bold = item->font(1);
            bold.setBold(true);
            item->setFont(1, bold);
            item->setToolTip(1, tr("Files you change are saved in this mod now"));
        }
        if (!r.exists()) {
            for (int c = 0; c < m_list->columnCount(); ++c) {
                item->setForeground(c, palette().color(QPalette::Disabled, QPalette::Text));
                item->setToolTip(c, tr("This folder no longer exists: %1").arg(QDir::toNativeSeparators(modRoot(r.folder))));
            }
        }
        if (r.id == selectId)
            m_list->setCurrentItem(item);
    }
    for (int c = 0; c < m_list->columnCount() - 1; ++c)
        if (c != 4)
            m_list->resizeColumnToContents(c);
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    m_status->setText(m_mods.empty() ? tr("No mods yet. Use Mod → New mod… to make one.")
                                     : tr("%n mod(s).", nullptr, int(m_mods.size())));
    showFiles();
    updateButtons();
}

std::optional<ModRecord> ModManagerDialog::selectedMod() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    if (!item || !item->isSelected())
        return std::nullopt;
    const int index = item->data(0, kIndexRole).toInt();
    if (index < 0 || size_t(index) >= m_mods.size())
        return std::nullopt;
    return m_mods[size_t(index)];
}

bool ModManagerDialog::selectMod(const QString &id)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->text(0) == id) {
            m_list->setCurrentItem(m_list->topLevelItem(i));
            return true;
        }
    return false;
}

void ModManagerDialog::showFiles()
{
    m_files->clear();
    const std::optional<ModRecord> mod = selectedMod();
    if (!mod)
        return;
    const QLocale locale;
    qint64 total = 0;
    const std::vector<ModFile> files = listModFiles(mod->folder);
    for (const ModFile &f : files) {
        auto *item = new QTreeWidgetItem(m_files);
        item->setText(0, f.path);
        item->setText(1, locale.formattedDataSize(f.size));
        item->setText(2, locale.toString(f.modified, QLocale::ShortFormat));
        item->setData(0, kPathRole, f.path);
        total += f.size;
    }
    m_files->resizeColumnToContents(1);
    m_files->resizeColumnToContents(2);
    m_status->setText(mod->exists() ? tr("%1: %n changed file(s), %2.", nullptr, int(files.size()))
                                          .arg(modDisplayName(*mod), locale.formattedDataSize(total))
                                    : tr("%1: the folder no longer exists.").arg(modDisplayName(*mod)));
}

QStringList ModManagerDialog::fileList() const
{
    QStringList out;
    for (int i = 0; i < m_files->topLevelItemCount(); ++i)
        out << m_files->topLevelItem(i)->data(0, kPathRole).toString();
    return out;
}

void ModManagerDialog::selectFiles(const QStringList &paths)
{
    for (int i = 0; i < m_files->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_files->topLevelItem(i);
        item->setSelected(paths.contains(item->data(0, kPathRole).toString()));
    }
}

void ModManagerDialog::updateButtons()
{
    const std::optional<ModRecord> mod = selectedMod();
    const bool exists = mod && mod->exists();
    m_use->setEnabled(mod && (exists || parseModFolder(mod->folder)));
    m_open->setEnabled(exists);
    m_zip->setEnabled(exists && m_files->topLevelItemCount() > 0);
    m_delete->setEnabled(exists);
    const QTreeWidgetItem *item = m_list->currentItem();
    const int index = mod && item ? item->data(0, kIndexRole).toInt() : -1;
    m_forget->setEnabled(index >= 0 && size_t(index) < m_inHistory.size() && m_inHistory[size_t(index)]);
    m_undo->setEnabled(exists && !m_files->selectedItems().isEmpty());
}

bool ModManagerDialog::removeWithFallback(const std::vector<Removal> &removals)
{
    std::vector<size_t> failed;
    QStringList errors;
    auto attempt = [&](const std::vector<size_t> &which, bool toTrash) {
        failed.clear();
        errors.clear();
        for (size_t i : which) {
            QString error;
            if (!removals[i](toTrash, &error)) {
                failed.push_back(i);
                errors << error;
            }
        }
    };
    std::vector<size_t> all(removals.size());
    std::iota(all.begin(), all.end(), size_t(0));
    attempt(all, m_useTrash);
    if (!failed.empty() && m_useTrash) {
        if (!confirm(tr("Delete permanently?"),
                     tr("%1<br><br>The trash is not available here. Delete %n item(s) permanently instead? This cannot "
                        "be undone.", nullptr, int(failed.size()))
                         .arg(errors.first())))
            return false;
        attempt(std::vector<size_t>(failed), false);
    }
    if (failed.empty())
        return true;
    QMessageBox::warning(this, windowTitle(), errors.join(u'\n'));
    return false;
}

bool ModManagerDialog::removeSelectedFiles()
{
    const std::optional<ModRecord> mod = selectedMod();
    QStringList paths;
    for (QTreeWidgetItem *item : m_files->selectedItems())
        paths << item->data(0, kPathRole).toString();
    if (!mod || paths.isEmpty())
        return false;
    if (!confirm(tr("Undo %n change(s)?", nullptr, int(paths.size())),
                 tr("These files are moved to the trash, so the game uses its own files again:<br><tt>%1</tt>")
                     .arg(paths.mid(0, 12).join(QStringLiteral("<br>")).toHtmlEscaped()
                          + (paths.size() > 12 ? QStringLiteral("<br>…") : QString()))))
        return false;
    std::vector<Removal> removals;
    for (const QString &path : paths)
        removals.push_back([&mod, path](bool toTrash, QString *error) { return removeModFile(mod->folder, path, toTrash, error); });
    const bool all = removeWithFallback(removals);
    reload(mod->id);
    return all;
}

bool ModManagerDialog::deleteSelectedMod()
{
    const std::optional<ModRecord> mod = selectedMod();
    if (!mod || !mod->exists())
        return false;
    const QString target = modDeleteTarget(mod->folder);
    if (!confirm(tr("Delete the mod %1?").arg(modDisplayName(*mod)),
                 tr("This moves the folder to the trash:<br><tt>%1</tt><br>(%n file(s))", nullptr,
                    int(listModFiles(mod->folder).size()))
                     .arg(QDir::toNativeSeparators(target).toHtmlEscaped())))
        return false;
    if (!removeWithFallback({[&](bool toTrash, QString *error) { return deleteMod(mod->folder, toTrash, error); }}))
        return false;
    std::vector<ModRecord> history = loadModHistory();
    forgetMod(history, mod->id);
    saveModHistory(history);
    if (QDir::cleanPath(mod->folder) == m_active)
        m_active.clear();
    reload();
    m_status->setText(tr("Deleted %1.").arg(modDisplayName(*mod)));
    return true;
}

void ModManagerDialog::chooseZip()
{
    const std::optional<ModRecord> mod = selectedMod();
    if (!mod)
        return;
    const QString suggested = QDir::home().filePath(
        QStringLiteral("%1.zip").arg(QString(mod->name + (mod->titleId.isEmpty() ? QString() : u' ' + mod->titleId)).simplified()));
    const QString path = QFileDialog::getSaveFileName(this, tr("Pack mod as zip"), suggested, tr("Zip files (*.zip)"));
    if (!path.isEmpty())
        packSelectedMod(path);
}

bool ModManagerDialog::packSelectedMod(const QString &zipPath)
{
    const std::optional<ModRecord> mod = selectedMod();
    if (!mod)
        return false;
    QProgressDialog *progress = makeProgressDialog(this, tr("Packing %1…").arg(mod->name), tr("Pack mod"), 1);
    QString error;
    const bool ok = packModZip(*mod, zipPath,
                               [progress](int done, int total) {
                                   progress->setMaximum(std::max(1, total));
                                   progress->setValue(done);
                                   return !progress->wasCanceled();
                               },
                               &error);
    closeProgressDialog(progress);
    if (!ok) {
        if (error != tr("Canceled."))
            QMessageBox::warning(this, tr("Pack mod"), error);
        return false;
    }
    m_status->setText(tr("Saved %1").arg(QDir::toNativeSeparators(zipPath)));
    return true;
}

void ModManagerDialog::forgetSelectedMod()
{
    const std::optional<ModRecord> mod = selectedMod();
    if (!mod)
        return;
    std::vector<ModRecord> history = loadModHistory();
    forgetMod(history, mod->id);
    saveModHistory(history);
    reload();
}

void ModManagerDialog::forgetMissing()
{
    std::vector<ModRecord> history = loadModHistory();
    forgetMissingMods(history);
    saveModHistory(history);
    reload(selectedMod() ? selectedMod()->id : QString());
}
