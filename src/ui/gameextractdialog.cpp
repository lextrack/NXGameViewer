#include "gameextractdialog.h"
#include "progressdialogs.h"
#include "switchfs/switchnup.h"
#include "switchfs/switchpackagereader.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <atomic>
#include <functional>
#include <memory>

namespace {

constexpr auto kKeysSetting = "switch/prodKeys";
constexpr auto kKeysChosenSetting = "switch/prodKeysChosen";
constexpr auto kPackagesSetting = "switch/packageFolder";
constexpr auto kDestinationSetting = "switch/extractFolder";
constexpr int kMaxFolderFiles = 4096;
constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kStatusMs = 8000;

QStringList packageFilters()
{
    return {QStringLiteral("*.nsp"), QStringLiteral("*.xci"), QStringLiteral("*.nsz"), QStringLiteral("*.xcz"),
            QStringLiteral("*.nup")};
}

QString displayName(const SwitchTitle &t)
{
    return t.displayName();
}

quint64 baseGameId(const SwitchTitle &t)
{
    const quint64 id = t.titleId & ~quint64(0xFFF);
    return t.type == SwitchTitle::AddOn ? id - 0x1000 : id;
}

}

GameExtractDialog::GameExtractDialog(QWidget *parent, std::optional<KeySearchPlaces> places)
    : QDialog(parent), m_places(std::move(places))
{
    setWindowTitle(tr("Extract game"));
    QSize size(1080, 680);
    if (parent)
        size = size.expandedTo(parent->window()->size() * 0.85);
    resize(size);

    auto *intro = new QLabel(tr("Pick your own game dumps (NSP, XCI, NSZ, XCZ or NUP). The app decrypts them with the keys of your own "
                                "console (NUP files need none) and saves the game's <b>exefs</b> and <b>romfs</b> folders, "
                                "ready to browse here."));
    intro->setWordWrap(true);

    m_keysLabel = new QLabel;
    m_keysLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_keysLabel->setWordWrap(true);
    auto *keysButton = new QPushButton(tr("Change…"));
    keysButton->setToolTip(tr("Pick the prod.keys file dumped from your own console. Title keys (title.keys) found "
                              "next to it and in the emulator folders are used too"));
    connect(keysButton, &QPushButton::clicked, this, &GameExtractDialog::chooseKeys);
    auto *keysRow = new QHBoxLayout;
    keysRow->addWidget(new QLabel(tr("Keys:")));
    keysRow->addWidget(m_keysLabel, 1);
    keysRow->addWidget(keysButton);

    auto *addFilesButton = new QPushButton(tr("Add files…"));
    addFilesButton->setToolTip(tr("Pick NSP, XCI, NSZ, XCZ or NUP files"));
    connect(addFilesButton, &QPushButton::clicked, this, &GameExtractDialog::addFiles);
    auto *addFolderButton = new QPushButton(tr("Add folder…"));
    addFolderButton->setToolTip(tr("Find every NSP, XCI, NSZ, XCZ and NUP file in a folder and its subfolders"));
    connect(addFolderButton, &QPushButton::clicked, this, &GameExtractDialog::addFolder);
    auto *addRow = new QHBoxLayout;
    addRow->addWidget(addFilesButton);
    addRow->addWidget(addFolderButton);
    addRow->addStretch();

    m_list = new QTreeWidget;
    m_list->setHeaderLabels({tr("Name"), tr("Title ID"), tr("Version"), tr("Type"), tr("File")});
    m_list->setRootIsDecorated(true);
    m_list->setIconSize(QSize(40, 40));
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_list->header()->resizeSection(0, 420);
    for (int c = 1; c < 4; ++c)
        m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &GameExtractDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (m_extract->isEnabled())
            extractSelected();
    });

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    m_statusTimer->setInterval(kStatusMs);
    connect(m_statusTimer, &QTimer::timeout, this, [this] { m_status->setText(m_baseStatus); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_extract = buttons->addButton(tr("Extract…"), QDialogButtonBox::AcceptRole);
    m_extract->setToolTip(tr("Choose where to save the selected game"));
    connect(m_extract, &QPushButton::clicked, this, &GameExtractDialog::extractSelected);
    m_verify = buttons->addButton(tr("Verify"), QDialogButtonBox::ActionRole);
    m_verify->setToolTip(tr("Check every block of the selected game against its hashes, to find a damaged dump"));
    connect(m_verify, &QPushButton::clicked, this, &GameExtractDialog::verifySelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(keysRow);
    layout->addLayout(addRow);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    m_scan = new QFutureWatcher<PackageScan>(this);
    connect(m_scan, &QFutureWatcher<PackageScan>::finished, this, [this] {
        m_scanPending = false;
        const PackageScan scan = m_scan->result();
        applyKeys(scan.keys, m_keysChosen);
        if (!scan.withoutKeys.isEmpty()) {
            m_pending << scan.withoutKeys;
            if (!scan.packages.empty())
                showScanned(scan.packages);
            setStatus(m_keyProblem);
            updateButtons();
            chooseKeys();
            return;
        }
        if (scan.replacesList) {
            m_titles.clear();
            m_list->clear();
        }
        showScanned(scan.packages);
        showKeyProblem();
        if (!m_pending.isEmpty()) {
            const QStringList next = std::exchange(m_pending, {});
            addPackages(next);
        }
    });

    const QSettings settings;
    const QString stored = settings.value(kKeysSetting).toString();
    const bool chosen = settings.value(kKeysChosenSetting, !stored.isEmpty()).toBool();
    SwitchKeySearch search;
    if (chosen && !stored.isEmpty())
        search = useSwitchKeyFile(stored, {}, placesFor({}));
    if (search.found)
        applyKeys(search, true);
    else
        applyKeys(findSwitchKeys({}, placesFor({})), false);
    showKeyProblem();
    updateButtons();
}

KeySearchPlaces GameExtractDialog::placesFor(const QStringList &packages) const
{
    KeySearchPlaces places = m_places ? *m_places : KeySearchPlaces::forSystem({});
    places.gameFolders = KeySearchPlaces::foldersOf(packages);
    return places;
}

QStringList GameExtractDialog::listedPackages() const
{
    QStringList packages;
    for (const SwitchTitle &t : m_titles)
        if (!packages.contains(t.package))
            packages << t.package;
    return packages;
}

void GameExtractDialog::applyKeys(const SwitchKeySearch &search, bool chosen)
{
    m_keyProblem = search.problem;
    if (!search.found) {
        if (!m_keys)
            m_keysLabel->setText(tr("<i>No key file found. Click Change… and pick the prod.keys dumped from your own "
                                    "console.</i>"));
        return;
    }
    const FoundSwitchKeys &found = *search.found;
    m_keys = found.keys;
    m_keyPath = found.prodKeys;
    SwitchPackageReader::setKeyFile(m_keyPath);
    m_keysChosen = chosen;
    QString text = QDir::toNativeSeparators(found.prodKeys).toHtmlEscaped();
    if (!chosen)
        text += tr(" <span style=\"color: gray\">(found automatically)</span>");
    if (m_keys->titleKeyCount() > 0)
        text += tr(" — %n title key(s)", nullptr, m_keys->titleKeyCount());
    if (!found.titleKeyFiles.isEmpty()) {
        QStringList files;
        for (const QString &f : found.titleKeyFiles)
            files << QDir::toNativeSeparators(f);
        m_keysLabel->setToolTip(tr("Title keys from: %1").arg(files.join(QStringLiteral(", "))));
    } else {
        m_keysLabel->setToolTip({});
    }
    m_keysLabel->setText(text);
}

void GameExtractDialog::showKeyProblem()
{
    if (m_keyProblem.isEmpty() || m_baseStatus.contains(m_keyProblem))
        return;
    setStatus(m_baseStatus.isEmpty() ? m_keyProblem : m_baseStatus + QLatin1Char(' ') + m_keyProblem);
}

void GameExtractDialog::setStatus(const QString &text)
{
    m_baseStatus = text;
    m_statusTimer->stop();
    m_status->setText(text);
}

void GameExtractDialog::flashStatus(const QString &text)
{
    m_status->setText(text);
    m_statusTimer->start();
}

bool GameExtractDialog::setKeyFile(const QString &path, QString *error)
{
    const QStringList packages = listedPackages() + m_pending;
    const SwitchKeySearch search = useSwitchKeyFile(path, packages, placesFor(packages));
    if (!search.found) {
        if (error)
            *error = search.problem;
        return false;
    }
    applyKeys(search, true);
    QSettings settings;
    settings.setValue(kKeysSetting, m_keyPath);
    settings.setValue(kKeysChosenSetting, true);
    setStatus({});
    showKeyProblem();
    const QStringList listed = listedPackages();
    if (!listed.isEmpty()) {
        m_titles.clear();
        m_list->clear();
    }
    const QStringList again = listed + std::exchange(m_pending, {});
    if (!again.isEmpty())
        addPackages(again);
    updateButtons();
    return true;
}

void GameExtractDialog::chooseKeys()
{
    const QString start = m_keyPath.isEmpty() ? QDir::homePath() : QFileInfo(m_keyPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose your console's keys"), start,
                                                      tr("Key files (*.keys *.txt *.ini);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!setKeyFile(path, &error))
        QMessageBox::warning(this, tr("Choose keys"), error);
}

void GameExtractDialog::addFiles()
{
    const QString start = QSettings().value(kPackagesSetting, QDir::homePath()).toString();
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Add games"), start,
                                                            tr("Switch games (*.nsp *.xci *.nsz *.xcz *.nup);;All files (*)"));
    if (paths.isEmpty())
        return;
    QSettings().setValue(kPackagesSetting, QFileInfo(paths.front()).absolutePath());
    addPackages(paths);
}

void GameExtractDialog::addFolder()
{
    const QString start = QSettings().value(kPackagesSetting, QDir::homePath()).toString();
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add a folder of games"), start);
    if (folder.isEmpty())
        return;
    QSettings().setValue(kPackagesSetting, folder);
    QStringList paths;
    QDirIterator it(folder, packageFilters(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && paths.size() < kMaxFolderFiles)
        paths << it.next();
    if (paths.isEmpty()) {
        flashStatus(tr("No NSP, XCI, NSZ, XCZ or NUP files were found in %1.").arg(QDir::toNativeSeparators(folder)));
        return;
    }
    addPackages(paths);
}

void GameExtractDialog::addPackages(const QStringList &paths)
{
    QStringList fresh;
    for (const QString &p : paths) {
        bool known = false;
        for (const SwitchTitle &t : m_titles)
            known = known || t.package == p;
        if (!known && !fresh.contains(p))
            fresh << p;
    }
    if (fresh.isEmpty())
        return;
    if (isScanning()) {
        m_pending << fresh;
        return;
    }
    setStatus(tr("Reading %n file(s)…", nullptr, int(fresh.size())));
    const QStringList listed = listedPackages();
    const QStringList all = listed + fresh;
    m_scanPending = true;
    m_scan->setFuture(QtConcurrent::run([fresh, listed, all, places = placesFor(all), chosen = m_keysChosen,
                                         keyPath = m_keyPath] {
        PackageScan scan;
        auto scanOne = [&scan](const QString &path, const SwitchKeys &keys) {
            ScannedPackage p;
            p.path = path;
            p.titles = scanSwitchPackage(path, keys, &p.error);
            scan.packages.push_back(std::move(p));
        };
        QStringList encrypted, plain;
        for (const QString &path : fresh)
            (isNupFile(path) ? plain : encrypted) << path;
        for (const QString &path : plain)
            scanOne(path, SwitchKeys());
        if (encrypted.isEmpty())
            return scan;
        scan.keys = chosen ? useSwitchKeyFile(keyPath, all, places) : findSwitchKeys(all, places);
        if (!scan.keys.found) {
            scan.withoutKeys = encrypted;
            return scan;
        }
        scan.replacesList = !listed.isEmpty() && scan.keys.found->prodKeys != keyPath;
        if (scan.replacesList) {
            scan.packages.clear();
            for (const QString &path : all)
                if (isNupFile(path))
                    scanOne(path, SwitchKeys());
        }
        const SwitchKeys &keys = scan.keys.found->keys;
        for (const QString &path : scan.replacesList ? all : encrypted)
            if (!isNupFile(path))
                scanOne(path, keys);
        return scan;
    }));
    updateButtons();
}

void GameExtractDialog::showScanned(const std::vector<ScannedPackage> &packages)
{
    QStringList failed;
    for (const ScannedPackage &p : packages) {
        if (p.titles.empty())
            failed << QStringLiteral("%1: %2").arg(QFileInfo(p.path).fileName(), p.error);
        for (const SwitchTitle &t : p.titles)
            m_titles.push_back(t);
    }
    rebuildList();
    QString status = tr("%n title(s) found.", nullptr, titleCount());
    if (!failed.isEmpty())
        status += QLatin1Char(' ') + tr("Not read: %1").arg(failed.join(QStringLiteral("; ")));
    setStatus(status);
    updateButtons();
}

void GameExtractDialog::rebuildList()
{
    const std::optional<SwitchTitle> before = selectedTitle();
    const int selectedIndex = before ? m_list->selectedItems().front()->data(0, kIndexRole).toInt() : -1;
    QSet<int> expanded;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->isExpanded())
            expanded.insert(m_list->topLevelItem(i)->data(0, kIndexRole).toInt());
    m_list->setSortingEnabled(false);
    m_list->clear();
    auto makeItem = [this](int index, QTreeWidgetItem *parent) {
        const SwitchTitle &t = m_titles[size_t(index)];
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_list);
        item->setText(0, displayName(t));
        item->setText(1, t.titleIdText());
        item->setText(2, t.displayVersion.isEmpty() ? QStringLiteral("v%1").arg(t.version)
                                                    : QStringLiteral("%1 (v%2)").arg(t.displayVersion).arg(t.version));
        item->setText(3, t.typeName());
        item->setText(4, QFileInfo(t.package).fileName());
        item->setToolTip(4, QDir::toNativeSeparators(t.package));
        item->setData(0, kIndexRole, index);
        QPixmap icon;
        if (!t.icon.isEmpty() && icon.loadFromData(t.icon))
            item->setIcon(0, icon.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        if (!t.canExtract()) {
            for (int c = 0; c < 5; ++c) {
                item->setForeground(c, palette().color(QPalette::Disabled, QPalette::Text));
                item->setToolTip(c, t.problem);
            }
        } else if (!t.publisher.isEmpty()) {
            item->setToolTip(0, t.publisher);
        }
        return item;
    };
    QHash<quint64, QTreeWidgetItem *> games;
    for (size_t i = 0; i < m_titles.size(); ++i) {
        const SwitchTitle &t = m_titles[i];
        if (t.type == SwitchTitle::Application && !games.contains(t.titleId))
            games.insert(t.titleId, makeItem(int(i), nullptr));
    }
    for (size_t i = 0; i < m_titles.size(); ++i) {
        const SwitchTitle &t = m_titles[i];
        if (t.type == SwitchTitle::Application && games.value(t.titleId)->data(0, kIndexRole).toInt() == int(i))
            continue;
        QTreeWidgetItem *parent = t.type == SwitchTitle::Application ? nullptr : games.value(baseGameId(t));
        makeItem(int(i), parent);
    }
    for (QTreeWidgetItem *game : std::as_const(games)) {
        int dlc = 0, updates = 0;
        for (int c = 0; c < game->childCount(); ++c)
            (m_titles[size_t(game->child(c)->data(0, kIndexRole).toInt())].type == SwitchTitle::Patch ? updates : dlc)++;
        QStringList extras;
        if (updates > 0)
            extras << tr("%n update(s)", nullptr, updates);
        if (dlc > 0)
            extras << tr("%n DLC", nullptr, dlc);
        if (!extras.isEmpty()) {
            game->setText(3, QStringLiteral("%1 (+%2)").arg(game->text(3), extras.join(QStringLiteral(", "))));
            game->setExpanded(expanded.contains(game->data(0, kIndexRole).toInt()));
        }
    }
    m_list->setSortingEnabled(true);
    QTreeWidgetItemIterator it(m_list);
    QTreeWidgetItem *select = m_list->topLevelItemCount() > 0 ? m_list->topLevelItem(0) : nullptr;
    for (; *it; ++it)
        if ((*it)->data(0, kIndexRole).toInt() == selectedIndex)
            select = *it;
    if (select)
        m_list->setCurrentItem(select);
}

int GameExtractDialog::titleCount() const
{
    return int(m_titles.size());
}

GameExtractDialog::~GameExtractDialog()
{
    if (m_extractionCancel)
        m_extractionCancel->store(true);
    if (m_extraction) {
        m_extraction->disconnect(this);
        m_extraction->waitForFinished();
    }
    if (m_scan) {
        m_scan->disconnect(this);
        m_scan->waitForFinished();
    }
}

bool GameExtractDialog::isScanning() const
{
    return m_scanPending || m_scan->isRunning();
}

std::optional<SwitchTitle> GameExtractDialog::selectedTitle() const
{
    const QList<QTreeWidgetItem *> items = m_list->selectedItems();
    if (items.isEmpty())
        return std::nullopt;
    const int index = items.front()->data(0, kIndexRole).toInt();
    if (index < 0 || size_t(index) >= m_titles.size())
        return std::nullopt;
    return m_titles[size_t(index)];
}

void GameExtractDialog::updateButtons()
{
    const std::optional<SwitchTitle> t = selectedTitle();
    const bool usable = t && t->canExtract() && (m_keys || t->nupPackage >= 0) && !m_extraction;
    m_extract->setEnabled(usable);
    m_verify->setEnabled(usable);
    if (t && !t->canExtract())
        flashStatus(tr("%1 cannot be extracted: %2").arg(displayName(*t), t->problem));
    else if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
        m_status->setText(m_baseStatus);
    }
}

QString GameExtractDialog::suggestedFolderName(const SwitchTitle &title)
{
    return title.folderName();
}

void GameExtractDialog::extractSelected()
{
    const std::optional<SwitchTitle> t = selectedTitle();
    if (!t || !t->canExtract() || (!m_keys && t->nupPackage < 0))
        return;
    const QString base = QSettings().value(kDestinationSetting, QDir::homePath()).toString();
    ExtractDestinationDialog destination(*t, QDir(base).filePath(suggestedFolderName(*t)), this);
    if (destination.exec() != QDialog::Accepted)
        return;
    const QString path = QDir::cleanPath(destination.destination());
    const QDir dir(path);
    if (dir.exists() && !dir.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Extract game"),
            tr("%1 is not empty. Files with the same names are replaced. Extract there anyway?")
                .arg(QDir::toNativeSeparators(path)));
        if (answer != QMessageBox::Yes)
            return;
    }
    QSettings().setValue(kDestinationSetting, QFileInfo(path).absolutePath());
    runExtraction(*t, path);
}

void GameExtractDialog::startTitleJob(const QString &label, const QString &title, const TitleJob &job,
                                      const std::function<void(const ExtractOutcome &, bool canceled)> &finished)
{
    auto done = std::make_shared<std::atomic<qint64>>(0);
    auto total = std::make_shared<std::atomic<qint64>>(0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QProgressDialog *progress = makeProgressDialog(this, label, title, 1000, 0);
    progress->setValue(0);
    auto *timer = new QTimer(progress);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, progress, [progress, done, total, label] {
        const qint64 t = total->load(), d = done->load();
        if (t > 0) {
            progress->setValue(int(d * 1000 / t));
            progress->setLabelText(tr("%1 %2 of %3").arg(label, QLocale().formattedDataSize(d), QLocale().formattedDataSize(t)));
        }
    });
    connect(progress, &QProgressDialog::canceled, progress, [cancel] { cancel->store(true); });
    timer->start();

    auto *watcher = new QFutureWatcher<ExtractOutcome>(this);
    m_extraction = watcher;
    m_extractionCancel = cancel;
    updateButtons();
    connect(watcher, &QFutureWatcher<ExtractOutcome>::finished, this, [this, watcher, progress, cancel, finished] {
        const ExtractOutcome outcome = watcher->result();
        m_extraction = nullptr;
        m_extractionCancel.reset();
        watcher->deleteLater();
        closeProgressDialog(progress);
        updateButtons();
        finished(outcome, cancel->load());
    });
    watcher->setFuture(QtConcurrent::run([job, done, total, cancel] {
        return job([&](qint64 d, qint64 t) {
            done->store(d);
            total->store(t);
            return !cancel->load();
        });
    }));
}

void GameExtractDialog::runExtraction(const SwitchTitle &title, const QString &destination)
{
    const SwitchKeys keys = m_keys.value_or(SwitchKeys());
    const TitleJob job = [title, keys, destination](const Progress &progress) {
        ExtractOutcome outcome;
        outcome.ok = extractSwitchTitle(title, keys, destination, progress, &outcome.result, &outcome.error);
        return outcome;
    };
    startTitleJob(tr("Extracting %1…").arg(displayName(title)), tr("Extract game"), job,
                  [this, title, destination](const ExtractOutcome &outcome, bool) {
        if (!outcome.ok) {
            QMessageBox::warning(this, tr("Extract game"), outcome.error);
            return;
        }
        if (outcome.result.canceled) {
            flashStatus(tr("Extraction canceled. The files saved so far stay in %1.").arg(QDir::toNativeSeparators(destination)));
            return;
        }
        flashStatus(tr("Extracted %1.").arg(displayName(title)));
        QMessageBox box(QMessageBox::Information, tr("Extract game"),
                        tr("Extracted %n file(s)", nullptr, outcome.result.files)
                            + tr(" (%1) to %2.").arg(QLocale().formattedDataSize(outcome.result.bytes),
                                                     QDir::toNativeSeparators(destination)),
                        QMessageBox::Close, this);
        QPushButton *open = box.addButton(tr("Open in the app"), QMessageBox::AcceptRole);
        QPushButton *show = box.addButton(tr("Show in file manager"), QMessageBox::ActionRole);
        box.setDefaultButton(open);
        box.exec();
        if (box.clickedButton() == open) {
            emit openFolderRequested(destination);
            accept();
        } else if (box.clickedButton() == show) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
        }
    });
}

void GameExtractDialog::verifySelected()
{
    const std::optional<SwitchTitle> t = selectedTitle();
    if (!t || !t->canExtract() || (!m_keys && t->nupPackage < 0) || m_extraction)
        return;
    const SwitchTitle title = *t;
    const SwitchKeys keys = m_keys.value_or(SwitchKeys());
    const TitleJob job = [title, keys](const Progress &progress) {
        ExtractOutcome outcome;
        outcome.ok = verifySwitchTitle(title, keys, progress, &outcome.error);
        return outcome;
    };
    startTitleJob(tr("Verifying %1…").arg(displayName(title)), tr("Verify game"), job,
                  [this, title](const ExtractOutcome &outcome, bool canceled) {
        if (canceled) {
            flashStatus(tr("Verification of %1 canceled.").arg(displayName(title)));
            return;
        }
        if (!outcome.ok) {
            QMessageBox::warning(this, tr("Verify game"),
                                 tr("%1 is damaged or could not be checked:\n%2").arg(displayName(title), outcome.error));
            return;
        }
        flashStatus(tr("%1 is intact: every block matches its hash.").arg(displayName(title)));
        QMessageBox::information(this, tr("Verify game"),
                                 tr("%1 is intact: every block matches its hash.").arg(displayName(title)));
    });
}

ExtractDestinationDialog::ExtractDestinationDialog(const SwitchTitle &title, const QString &suggested, QWidget *parent)
    : QDialog(parent), m_folderName(GameExtractDialog::suggestedFolderName(title))
{
    setWindowTitle(tr("Where to extract"));
    auto *label = new QLabel(tr("Extract <b>%1</b> to this folder. Its <b>exefs</b> and <b>romfs</b> folders are made inside it.")
                                 .arg((title.name.isEmpty() ? QFileInfo(title.package).completeBaseName() : title.name)
                                          .toHtmlEscaped()));
    label->setWordWrap(true);
    m_path = new QLineEdit(QDir::toNativeSeparators(suggested));
    m_path->setToolTip(tr("Type a folder, or pick one with Browse…; a missing folder is made"));
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setToolTip(tr("Pick a folder; the game gets its own folder inside it"));
    connect(browse, &QPushButton::clicked, this, &ExtractDestinationDialog::browse);
    auto *row = new QHBoxLayout;
    row->addWidget(m_path, 1);
    row->addWidget(browse);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *extract = buttons->addButton(tr("Extract"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_path, &QLineEdit::textChanged, this, [this, extract] {
        extract->setEnabled(QDir::isAbsolutePath(QDir::fromNativeSeparators(m_path->text().trimmed())));
    });
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(label);
    layout->addLayout(row);
    layout->addWidget(buttons);
    setMinimumWidth(520);
    constexpr int kWidth = 680;
    resize(kWidth, std::max(heightForWidth(kWidth), sizeHint().height()));
}

QString ExtractDestinationDialog::destination() const
{
    return QDir::fromNativeSeparators(m_path->text().trimmed());
}

void ExtractDestinationDialog::setDestination(const QString &path)
{
    m_path->setText(QDir::toNativeSeparators(path));
}

void ExtractDestinationDialog::browse()
{
    QString start = destination();
    while (!start.isEmpty() && !QFileInfo(start).isDir())
        start = QFileInfo(start).absolutePath() == start ? QString() : QFileInfo(start).absolutePath();
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Extract to"), start.isEmpty() ? QDir::homePath() : start);
    if (!folder.isEmpty())
        setDestination(QDir(folder).filePath(m_folderName));
}
