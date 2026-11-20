#include "mainwindow.h"

#include "gameextractdialog.h"
#include "formats/archives/rpf6reader.h"
#include "formats/data/bymlreader.h"
#include "formats/data/msbtreader.h"
#include "formats/switchfs/switchpackagereader.h"
#include "modhistory.h"
#include "audioplayer.h"
#include "hoverdelegate.h"
#include "imageview.h"
#include "modelview.h"
#include "videoview.h"
#include "tablemodel.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace {

Qt::WindowFlags withoutMinMax(Qt::WindowFlags flags)
{
    return (flags & ~Qt::WindowMinMaxButtonsHint) | Qt::CustomizeWindowHint | Qt::WindowTitleHint
         | Qt::WindowCloseButtonHint;
}

class NoMinMaxFilter : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish)
            if (auto *box = qobject_cast<QMessageBox *>(watched))
                box->setWindowFlags(withoutMinMax(box->windowFlags()));
        return QObject::eventFilter(watched, event);
    }
};

QString droppedPath(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return {};
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile()))
            return url.toLocalFile();
    }
    return {};
}

QIcon whiteIcon(const QIcon &icon)
{
    QIcon result;
    for (const int size : {12, 16, 22, 32, 48}) {
        QPixmap white = icon.pixmap(QSize(size, size));
        QPainter painter(&white);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(white.rect(), Qt::white);
        painter.end();
        result.addPixmap(white, QIcon::Normal);
        result.addPixmap(icon.pixmap(QSize(size, size), QIcon::Disabled), QIcon::Disabled);
    }
    return result;
}

void styleNavigationButton(QToolButton *button, const QString &color, const QString &hover, const QString &dark)
{
    button->setIconSize(QSize(12, 12));
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 1px 3px; border: 1px solid transparent; border-radius: 3px; }"
        "QToolButton:enabled { background-color: %1; border-color: %3; }"
        "QToolButton:enabled:hover { background-color: %2; }"
        "QToolButton:enabled:pressed { background-color: %3; }").arg(color, hover, dark));
}

}

MainWindow::MainWindow(const QString &startPath, QWidget *parent) : QMainWindow(parent)
{
    m_audioPlayer = new AudioPlayer(this);
    setWindowTitle(tr("NX Game Viewer"));
    resize(1280, 800);

    setupToolbar();
    QWidget *browser = setupBrowser();
    setupViewer();

    auto *splitter = new QSplitter;
    splitter->addWidget(browser);
    splitter->addWidget(m_stack);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({360, 920});
    setCentralWidget(splitter);

    m_info = new QLabel;
    statusBar()->addWidget(m_info, 1);

    setAcceptDrops(true);
    m_fileFilter->setAcceptDrops(false);
    m_rowFilter->setAcceptDrops(false);

    setupBackgroundWork();
    connectSignals();
    loadArchiveKeys();
    loadSwitchKeys();
    loadModHistory();
    loadGameProfiles(QSettings().value("game").toString());
    showWelcome();

    if (!startPath.isEmpty())
        openPath(startPath);
}

MainWindow::~MainWindow()
{
    if (m_searchCancel)
        m_searchCancel->store(true);
    const std::initializer_list<QFutureWatcherBase *> cancellable{m_extractWatcher, m_keyWatcher, m_searchWatcher};
    for (QFutureWatcherBase *watcher : cancellable)
        if (watcher) {
            watcher->disconnect(this);
            watcher->cancel();
        }
    for (QFutureWatcherBase *watcher : cancellable)
        if (watcher)
            watcher->waitForFinished();
    if (m_exportWatcher) {
        m_exportWatcher->disconnect(this);
        m_exportWatcher->waitForFinished();
    }
}

void MainWindow::setupToolbar()
{
    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setIconSize(QSize(18, 18));
    QAction *openFolderAction =
        toolbar->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open folder…"), this, &MainWindow::chooseFolder);
    openFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    openFolderAction->setToolTip(tr("Browse a folder of game files (Ctrl+Shift+O)"));
    QAction *openFileAction =
        toolbar->addAction(style()->standardIcon(QStyle::SP_FileIcon), tr("Open file…"), this, &MainWindow::chooseFile);
    openFileAction->setShortcut(QKeySequence::Open);
    openFileAction->setToolTip(tr("Open a single file (Ctrl+O)"));
    m_reloadAction = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Reload"), this,
                                        [this] { reloadCurrent(false); });
    m_reloadAction->setShortcut(QKeySequence::Refresh);
    m_reloadAction->setToolTip(tr("Read the open file again from disk (F5). It also reloads by itself when another "
                                  "program changes it; the file list always follows changes on disk"));
    m_reloadAction->setEnabled(false);
    toolbar->addSeparator();

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setToolTipsVisible(true);
    fileMenu->addAction(openFolderAction);
    fileMenu->addAction(openFileAction);
    fileMenu->addSeparator();
    QAction *extractGameAction = fileMenu->addAction(tr("Extract game…"), this, &MainWindow::extractGame);
    extractGameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    extractGameAction->setToolTip(tr("Decrypt your own NSP, XCI, NSZ or XCZ game dumps into exefs and romfs folders (Ctrl+Shift+E)"));
    fileMenu->addSeparator();
    fileMenu->addAction(m_reloadAction);
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    m_fileWatcher = new QFileSystemWatcher(this);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(400);
    connect(m_reloadTimer, &QTimer::timeout, this, [this] { reloadCurrent(true); });
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_currentPath.isEmpty() && !m_fileWatcher->files().contains(m_currentPath) && QFileInfo::exists(m_currentPath))
            m_reloadTimer->start();
    });

    QMenu *exportMenu = menuBar()->addMenu(tr("&Extract"));
    exportMenu->setToolTipsVisible(true);
    m_extractAction = exportMenu->addAction(tr("Extract…"), this, &MainWindow::extractShown);
    m_extractAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_extractAction->setToolTip(tr("Save the selected files, or the open archive's files, converted to common formats "
                                   "such as PNG, WAV and glTF where possible (Ctrl+E). Also in the right-click menus"));
    m_extractAction->setEnabled(false);
    exportMenu->addSection(tr("Save what is shown"));
    exportMenu->addAction(tr("Table as CSV…"), this, &MainWindow::exportCsv);
    m_exportJsonAction = exportMenu->addAction(tr("Plain JSON…"), this, &MainWindow::exportJson);
    m_exportJsonAction->setToolTip(tr("Save as plain, readable JSON (formats that support it)"));
    m_exportJsonAction->setEnabled(false);
    m_exportImageAction = exportMenu->addAction(tr("Picture as PNG…"), this, &MainWindow::exportImage);
    m_exportImageAction->setToolTip(tr("Save the picture as a PNG file (textures, model previews)"));
    m_exportImageAction->setEnabled(false);
    m_exportModelAction = exportMenu->addAction(tr("Model as glTF (.glb)…"), this, &MainWindow::exportModel);
    m_exportModelAction->setToolTip(tr("Save the 3D model with its textures and skeleton, for Blender and other tools"));
    m_exportAnimatedAction =
        exportMenu->addAction(tr("Model with animations (.glb)…"), this, &MainWindow::exportModelWithAnimations);
    m_exportAnimatedAction->setToolTip(
        tr("Save the 3D model together with animation files that fit its skeleton, each as a named animation"));
    setModelExportsEnabled(false);
    m_exportWavAction = exportMenu->addAction(tr("Sound as WAV…"), this, &MainWindow::exportWav);
    m_exportWavAction->setToolTip(tr("Save the sound as a WAV file"));
    m_exportWavAction->setEnabled(false);

    m_modMenu = menuBar()->addMenu(tr("&Mod"));
    QMenu *modMenu = m_modMenu;
    modMenu->setToolTipsVisible(true);
    m_replaceTextureAction = modMenu->addAction(tr("Replace texture with PNG…"), this,
                                                [this] {
                                                    if (const Table *table = currentTable())
                                                        if (const auto slot = table->textureSlot())
                                                            replaceTextureWithPng(m_currentPath, m_currentEntries, *slot);
                                                });
    m_replaceTextureAction->setToolTip(
        tr("Put a PNG picture (same size) into the open texture and save it in the mod folder (inside its archives too)"));
    m_replaceTextureAction->setEnabled(false);
    m_replaceFileAction = modMenu->addAction(tr("Replace file…"), this, [this] { replaceFileWith(m_currentPath, m_currentEntries); });
    m_replaceFileAction->setToolTip(tr("Replace the open file with a file you pick and save it in the mod folder (inside its "
                                       "archives too; compressed files are compressed again like the game's)"));
    m_replaceFileAction->setEnabled(false);
    m_replaceJsonAction = modMenu->addAction(tr("Replace with edited JSON…"), this,
                                             [this] { replaceWithEditedJson(m_currentPath, m_currentEntries); });
    m_replaceJsonAction->setToolTip(tr("Put a JSON file saved with Extract → Plain JSON and edited back into the open BYML "
                                       "or MSBT file, keeping the game's value types, and save it in the mod folder"));
    m_replaceJsonAction->setEnabled(false);
    modMenu->addSeparator();
    QAction *newModAction = modMenu->addAction(tr("New mod…"), this, [this] { newMod(); });
    newModAction->setToolTip(tr("Make a mod folder for an emulator from the game's title ID and a name"));
    m_modFolderAction = modMenu->addAction(tr("Choose mod to save in…"), this, [this] { chooseModFolder(); });
    QAction *manageAction = modMenu->addAction(tr("My mods…"), this, [this] { manageMods(); });
    manageAction->setToolTip(tr("Your mods and the ones found in your emulators' folders: see and undo changed files, "
                                "pack a mod as a zip, or delete it"));
    m_openModFolderAction = modMenu->addAction(tr("Open mod folder"), this, [this] {
        QDir().mkpath(modFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(modFolder()));
    });
    m_openModFolderAction->setToolTip(tr("Show the mod folder in your file manager"));
    modMenu->addSeparator();
    QAction *modHelpAction = modMenu->addAction(tr("How to make a mod…"), this, [this] { showModHelp(); });
    modHelpAction->setToolTip(tr("The steps to change a texture and see it in the game"));
    connect(modMenu, &QMenu::aboutToShow, this, &MainWindow::updateModMenu);
    updateModMenu();

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setToolTipsVisible(true);
    helpMenu->addAction(tr("How to extract a game…"), this, [this] { showExtractHelp(); });
    helpMenu->addAction(modHelpAction);
    helpMenu->addSeparator();
    helpMenu->addAction(tr("About NX Game Viewer…"), this, [this] { showAbout(); });
    helpMenu->addAction(tr("About Qt…"), this, [this] {
        NoMinMaxFilter filter;
        qApp->installEventFilter(&filter);
        QMessageBox::aboutQt(this);
        qApp->removeEventFilter(&filter);
    });

    for (QAction *top : menuBar()->actions())
        if (QMenu *menu = top->menu())
            for (QAction *item : menu->actions())
                item->setIconVisibleInMenu(false);

    m_playAction = new QAction(this);
    m_playAction->setObjectName(QStringLiteral("playAction"));
    m_playAction->setCheckable(true);
    m_playAction->setEnabled(false);
    connect(m_playAction, &QAction::toggled, this, &MainWindow::playAudio);
    connect(m_audioPlayer, &AudioPlayer::playingChanged, this, &MainWindow::showPlaying);
    connect(m_audioPlayer, &AudioPlayer::decodeFailed, this,
            [this](const QString &error) { QMessageBox::warning(this, tr("Cannot decode the sound"), error); });
    auto *playButton = new QToolButton;
    playButton->setObjectName(QStringLiteral("playButton"));
    playButton->setDefaultAction(m_playAction);
    playButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    playButton->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 2px 10px; border: 1px solid palette(mid); border-radius: 4px; }"
        "QToolButton:enabled { background-color: #2e7d32; border-color: #1b5e20; color: white; }"
        "QToolButton:enabled:hover { background-color: #388e3c; }"
        "QToolButton:checked { background-color: #c62828; border-color: #8e0000; color: white; }"
        "QToolButton:checked:hover { background-color: #d32f2f; }"));
    m_playButtonAction = toolbar->addWidget(playButton);
    m_playButtonAction->setVisible(false);
    showPlaying(false);

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto compactBox = [](QComboBox *box, int characters) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(characters);
    };
    auto label = [](const QString &text) {
        auto *l = new QLabel(text);
        l->setContentsMargins(8, 0, 4, 0);
        return l;
    };

    toolbar->addWidget(label(tr("Format")));
    m_formatBox = new QComboBox;
    refreshFormatChoices();
    m_formatBox->setToolTip(tr("Read files as this format, e.g. view any file as raw bytes with “Hex”. Lists the "
                               "formats that fit the open file; the others are under “Other formats…”"));
    compactBox(m_formatBox, 11);
    toolbar->addWidget(m_formatBox);

    toolbar->addWidget(label(tr("Text")));
    m_encodingBox = new QComboBox;
    m_encodingBox->addItem(tr("Auto-detect"));
    for (TextEncoding encoding : allTextEncodings())
        m_encodingBox->addItem(encodingName(encoding));
    m_encodingBox->setToolTip(tr("How text is decoded in text files and in the hex view"));
    compactBox(m_encodingBox, 10);
    m_encodingBox->setEnabled(false);
    toolbar->addWidget(m_encodingBox);

    m_hexAction = toolbar->addAction(tr("Hex"));
    m_hexAction->setCheckable(true);
    m_hexAction->setToolTip(tr("Show numbers as raw hexadecimal values"));
    toolbar->addSeparator();

    toolbar->addWidget(label(tr("Game")));
    m_gameBox = new QComboBox;
    m_gameBox->setToolTip(tr("Column names and types are saved per game"));
    compactBox(m_gameBox, 11);
    toolbar->addWidget(m_gameBox);
    QAction *folderAction = toolbar->addAction(style()->standardIcon(QStyle::SP_DirIcon), tr("Columns folder"), this, [this] {
        QDir().mkpath(columnsFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(columnsFolder()));
    });
    folderAction->setToolTip(tr("Open the folder with the saved column files, to back them up or share them"));
    if (auto *folderButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(folderAction)))
        folderButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
}

QWidget *MainWindow::setupBrowser()
{
    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setIconProvider(&m_iconProvider);
    m_fsModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_fsModel->setNameFilterDisables(false);

    m_tree = new QTreeView;
    m_tree->setModel(m_fsModel);
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setColumnHidden(2, true);
    m_tree->setColumnHidden(3, true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(false);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->hide();

    m_upAction = new QAction(whiteIcon(style()->standardIcon(QStyle::SP_FileDialogToParent)), tr("Up"), this);
    m_upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    m_upAction->setToolTip(tr("Go to the parent folder (Alt+Up)"));
    m_upAction->setEnabled(false);
    connect(m_upAction, &QAction::triggered, this, [this] {
        QDir folder(m_fsModel->rootPath());
        if (folder.cdUp())
            openFolder(folder.absolutePath());
    });
    addAction(m_upAction);
    auto *upButton = new QToolButton;
    upButton->setObjectName(QStringLiteral("upButton"));
    upButton->setDefaultAction(m_upAction);
    styleNavigationButton(upButton, QStringLiteral("#1e88e5"), QStringLiteral("#2196f3"), QStringLiteral("#1565c0"));

    m_folderEdit = new QLineEdit;
    m_folderEdit->setPlaceholderText(tr("Type or paste a folder or file, then press Enter"));
    m_folderEdit->setToolTip(tr("The folder shown below. Type or paste another folder or file and press Enter to go there."));
    connect(m_folderEdit, &QLineEdit::returnPressed, this, [this] {
        const QString typed = QDir::fromNativeSeparators(m_folderEdit->text().trimmed());
        if (QFileInfo::exists(typed)) {
            openPath(typed);
        } else {
            showMessage(tr("No such folder or file: %1").arg(m_folderEdit->text().trimmed()));
            m_folderEdit->setText(QDir::toNativeSeparators(m_fsModel->rootPath()));
        }
    });

    m_noFolder = new QWidget;
    auto *noFolderLayout = new QVBoxLayout(m_noFolder);
    noFolderLayout->addStretch();
    auto *noFolderText = new QLabel(tr("No folder open"));
    noFolderText->setAlignment(Qt::AlignCenter);
    noFolderText->setEnabled(false);
    noFolderLayout->addWidget(noFolderText);
    noFolderLayout->addStretch();

    m_fileFilter = new QLineEdit;
    m_fileFilter->setPlaceholderText(tr("Filter files…"));
    m_fileFilter->setClearButtonEnabled(true);
    m_fileFilter->setToolTip(tr("Finds files and folders by name in every subfolder. Enter opens the highlighted "
                                "match and moves to the next one. Wildcards such as *.bars work too."));
    m_searchStatus = new QLabel;
    m_searchStatus->setEnabled(false);
    m_searchStatus->hide();
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);
    m_searchWatcher = new QFutureWatcher<QStringList>(this);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(6, 6, 0, 0);
    leftLayout->setSpacing(4);
    m_browserBar = new QWidget;
    auto *browserLayout = new QVBoxLayout(m_browserBar);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->setSpacing(4);
    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(2);
    pathRow->addWidget(upButton);
    pathRow->addWidget(m_folderEdit, 1);
    browserLayout->addLayout(pathRow);
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(4);
    filterRow->addWidget(m_fileFilter, 1);
    filterRow->addWidget(m_searchStatus);
    browserLayout->addLayout(filterRow);
    m_browserBar->hide();
    leftLayout->addWidget(m_browserBar);
    leftLayout->addWidget(m_tree, 1);
    leftLayout->addWidget(m_noFolder, 1);
    return left;
}

void MainWindow::setupViewer()
{
    m_message = new QLabel;
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    connect(m_message, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("nxgv:rpf6-key"))
            findArchiveKey();
        else if (link == QLatin1String("nxgv:switch-keys"))
            chooseSwitchKeys();
    });

    m_model = new TableModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(TableModel::SortRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);

    m_table = new QTableView;
    m_table->setModel(m_proxy);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->setDefaultSectionSize(24);
    m_table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_hoverDelegate = new HoverRowDelegate(m_table);
    m_table->setItemDelegate(m_hoverDelegate);
    m_table->setMouseTracking(true);
    m_table->viewport()->installEventFilter(this);

    m_tableBox = new QComboBox;
    m_tableBox->setToolTip(tr("Pick which table of this file to show, when it has more than one"));
    m_rowFilter = new QLineEdit;
    m_rowFilter->setPlaceholderText(tr("Search in this table…"));
    m_rowFilter->setToolTip(tr("Shows only the rows that contain this text in any column (not case-sensitive)"));
    m_rowFilter->setClearButtonEnabled(true);

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Table:")));
    topRow->addWidget(m_tableBox, 1);
    topRow->addWidget(m_rowFilter, 2);

    m_backAction = new QAction(whiteIcon(style()->standardIcon(QStyle::SP_ArrowBack)), tr("Back"), this);
    m_backAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    m_backAction->setToolTip(tr("Back to the archive this file is in (Alt+Left)"));
    m_backAction->setEnabled(false);
    connect(m_backAction, &QAction::triggered, this, [this] {
        if (m_currentEntries.isEmpty())
            return;
        QStringList entries = m_currentEntries;
        entries.resize(std::max<qsizetype>(0, entries.size() - 1 - m_autoEntries));
        openLocation(m_currentPath, entries, false);
    });
    addAction(m_backAction);
    auto *backButton = new QToolButton;
    backButton->setObjectName(QStringLiteral("backButton"));
    backButton->setDefaultAction(m_backAction);
    styleNavigationButton(backButton, QStringLiteral("#ef6c00"), QStringLiteral("#f57c00"), QStringLiteral("#e65100"));
    m_locationLabel = new QLabel;
    m_locationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_locationBar = new QWidget;
    auto *locationRow = new QHBoxLayout(m_locationBar);
    locationRow->setContentsMargins(0, 0, 0, 0);
    locationRow->addWidget(backButton);
    locationRow->addWidget(m_locationLabel, 1);
    m_locationBar->hide();

    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
        if (currentArchive() && index.isValid())
            openArchiveEntry(m_proxy->mapToSource(index).row());
    });
    connect(m_table, &QWidget::customContextMenuRequested, this, &MainWindow::showTableMenu);

    auto *tablePage = new QWidget;
    auto *tableLayout = new QVBoxLayout(tablePage);
    tableLayout->setContentsMargins(0, 6, 6, 0);
    tableLayout->setSpacing(4);
    tableLayout->addWidget(m_locationBar);
    tableLayout->addLayout(topRow);
    m_archiveHint = new QLabel(tr("Double-click an entry to open it. Select entries and right-click to extract them."));
    m_archiveHint->setEnabled(false);
    m_archiveHint->hide();
    tableLayout->addWidget(m_archiveHint);
    m_imageView = new ImageView;
    m_imageSplitter = new QSplitter(Qt::Vertical);
    m_imageSplitter->addWidget(m_imageView);
    if (ModelView::isSupported()) {
        m_modelView = new ModelView;
        m_modelView->hide();
        m_imageSplitter->addWidget(m_modelView);
    }
    m_videoView = new VideoView;
    m_videoView->hide();
    m_imageSplitter->addWidget(m_videoView);
    m_imageSplitter->addWidget(m_table);
    for (int i = 0; i < m_imageSplitter->count(); ++i)
        m_imageSplitter->setStretchFactor(i, m_imageSplitter->widget(i) == m_table ? 1 : 3);
    m_imageSplitter->setChildrenCollapsible(false);
    m_imageView->hide();
    tableLayout->addWidget(m_imageSplitter);

    m_loadingPage = new QWidget;
    auto *loadingLayout = new QVBoxLayout(m_loadingPage);
    loadingLayout->addStretch();
    m_loadingLabel = new QLabel;
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLayout->addWidget(m_loadingLabel);
    auto *busy = new QProgressBar;
    busy->setRange(0, 0);
    busy->setMaximumWidth(320);
    busy->setTextVisible(false);
    loadingLayout->addWidget(busy, 0, Qt::AlignHCenter);
    loadingLayout->addStretch();

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_message);
    m_stack->addWidget(tablePage);
    m_stack->addWidget(m_loadingPage);
}

void MainWindow::setupBackgroundWork()
{
    m_loadWatcher = new QFutureWatcher<LoadResult>(this);
    connect(m_loadWatcher, &QFutureWatcherBase::finished, this, &MainWindow::loadFinished);
    m_imageWatcher = new QFutureWatcher<ImageResult>(this);
    connect(m_imageWatcher, &QFutureWatcherBase::finished, this, &MainWindow::imageFinished);
    m_modelWatcher = new QFutureWatcher<ModelResult>(this);
    connect(m_modelWatcher, &QFutureWatcherBase::finished, this, &MainWindow::modelFinished);
    m_exportWatcher = new QFutureWatcher<ExportOutcome>(this);
    connect(m_exportWatcher, &QFutureWatcherBase::finished, this, [this] {
        const ExportOutcome outcome = m_exportWatcher->result();
        setModelExportsEnabled(m_shownModel != nullptr);
        if (!outcome.ok)
            QMessageBox::warning(this, tr("Export failed"), outcome.message);
        else
            m_info->setText(outcome.message);
    });
    if (m_modelView) {
        connect(m_modelView, &ModelView::renderingFailed, this, [this] {
            m_imageTable.reset();
            rebuildShownTable(true);
        });
    }
    m_loadingTimer = new QTimer(this);
    m_loadingTimer->setSingleShot(true);
    m_loadingTimer->setInterval(150);
    connect(m_loadingTimer, &QTimer::timeout, this, [this] {
        if (!isLoading())
            return;
        m_stack->setCurrentIndex(2);
    });
}

void MainWindow::connectSignals()
{
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &index) {
                if (!index.isValid())
                    return;
                if (m_fsModel->isDir(index))
                    showFolder(m_fsModel->filePath(index));
                else
                    openFile(m_fsModel->filePath(index));
            });
    connect(m_tree, &QWidget::customContextMenuRequested, this, &MainWindow::showFileMenu);
    connect(m_fsModel, &QFileSystemModel::directoryLoaded, this, &MainWindow::revealCurrentFile);
    connect(m_fsModel, &QFileSystemModel::directoryLoaded, this, &MainWindow::revealPendingMatch);
    connect(m_fsModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::revealPendingMatch);
    connect(m_tree->verticalScrollBar(), &QAbstractSlider::actionTriggered, this, [this] { m_pendingMatch.clear(); });
    connect(m_tree, &QAbstractItemView::pressed, this, [this] { m_pendingMatch.clear(); });
    connect(m_fsModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::revealCurrentFile);
    connect(m_tree->verticalScrollBar(), &QAbstractSlider::actionTriggered, this, [this] { m_revealCurrent = false; });
    connect(m_tree, &QAbstractItemView::pressed, this, [this] { m_revealCurrent = false; });
    connect(m_fileFilter, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::startFileSearch);
    connect(m_fileFilter, &QLineEdit::returnPressed, this, &MainWindow::nextMatch);
    connect(m_searchWatcher, &QFutureWatcherBase::finished, this, &MainWindow::searchFinished);
    connect(m_formatBox, &QComboBox::currentIndexChanged, this, [this] {
        if (m_formatBox->currentData().toInt() < 0) {
            {
                const QSignalBlocker blocker(m_formatBox);
                m_formatBox->setCurrentIndex(std::min(m_shownFormatChoice, m_formatBox->count() - 1));
            }
            QTimer::singleShot(0, this, &MainWindow::chooseOtherFormat);
            return;
        }
        m_shownFormatChoice = m_formatBox->currentIndex();
        if (m_nextPath)
            openLocation(*m_nextPath, m_nextEntries, false);
        else if (isLoading())
            openLocation(m_loadingPath, m_nextEntries, false);
        else if (!m_currentPath.isEmpty())
            openLocation(m_currentPath, m_currentEntries, false);
    });
    connect(m_tableBox, &QComboBox::currentIndexChanged, this, [this] { rebuildShownTable(false); });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] {
        const Table *table = currentTable();
        if (table && !table->hasAudio() && table->hasRowAudio() && m_playAction->isChecked())
            m_audioPlayer->stop();
    });
    connect(m_encodingBox, &QComboBox::currentIndexChanged, this, [this] { rebuildShownTable(true); });
    connect(m_rowFilter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_proxy->setFilterFixedString(text);
        updateInfo();
    });
    connect(m_hexAction, &QAction::toggled, this, [this](bool hex) {
        m_model->setDisplayMode(hex ? DisplayMode::Hex : DisplayMode::Auto);
    });
    connect(m_table->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
            &MainWindow::showColumnMenu);
    connect(m_gameBox, &QComboBox::activated, this, &MainWindow::gameChosen);
}

void MainWindow::showWelcome()
{
    showMessage(tr("<h2>NX Game Viewer</h2>"
                   "<p>Browse and export the files of Switch games.</p>"
                   "<p>Open a folder or a file from the toolbar, or drag one onto this window.</p>"
                   "<p>To unpack your own game dump (NSP, XCI, NSZ or XCZ), choose File → Extract game….</p>"
                   "<p style=\"color: gray\">Textures, 3D models, sounds, videos, tables and archives from "
                   "Nintendo, Unity, CRI, Falcom and RAGE games.<br>"
                   "The Format menu lists every supported format.</p>"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QString path = droppedPath(event->mimeData());
    if (path.isEmpty())
        return;
    event->acceptProposedAction();
    statusBar()->showMessage(tr("Drop to open %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    statusBar()->clearMessage();
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    statusBar()->clearMessage();
    const QString path = droppedPath(event->mimeData());
    if (path.isEmpty())
        return;
    event->acceptProposedAction();
    openPath(path);
}

void MainWindow::setHoverRow(int row)
{
    if (row == m_hoverDelegate->hoverRow())
        return;
    m_hoverDelegate->setHoverRow(row);
    if (row >= 0)
        m_table->viewport()->setCursor(Qt::PointingHandCursor);
    else
        m_table->viewport()->unsetCursor();
    m_table->viewport()->update();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_table && watched == m_table->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto *move = static_cast<QMouseEvent *>(event);
            setHoverRow(currentArchive() ? m_table->rowAt(int(move->position().y())) : -1);
        } else if (event->type() == QEvent::Leave) {
            setHoverRow(-1);
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::currentName() const
{
    return m_currentEntries.isEmpty() ? QFileInfo(m_currentPath).fileName()
                                      : QFileInfo(m_currentEntries.last()).fileName();
}

bool MainWindow::isLoading() const
{
    return !m_loadingPath.isEmpty();
}

void MainWindow::startNextLoad()
{
    if (!m_nextPath)
        return;
    m_loadingPath = *std::exchange(m_nextPath, std::nullopt);
    m_loadingLabel->setText(
        tr("Opening %1…").arg(m_nextEntries.isEmpty() ? QFileInfo(m_loadingPath).fileName()
                                                      : m_nextEntries.last().section(u'/', -1)));
    loadHashNames();
    if (!m_loadingTimer->isActive() && m_stack->currentIndex() != 2)
        m_loadingTimer->start();

    const int formatIndex = forcedFormat();
    m_loadWatcher->setFuture(QtConcurrent::run([registry = m_registry, path = m_loadingPath, formatIndex,
                                                entries = m_nextEntries, descend = m_nextDescend] {
        LoadResult result;
        try {
            const FormatReader *forced = formatIndex > 0 ? registry->readers().at(size_t(formatIndex - 1)).get() : nullptr;
            if (forced && entries.isEmpty()) {
                result.document = FormatRegistry::openWith(*forced, path, &result.error);
                result.format = forced->name();
            } else {
                const FormatReader *used = nullptr;
                result.document = registry->open(path, &used, &result.error);
                if (used)
                    result.format = used->name();
            }
            auto archiveOf = [](const Document &doc) -> std::shared_ptr<const Archive> {
                for (const auto &t : doc.tables)
                    if (auto a = t->archive())
                        return a;
                return nullptr;
            };
            auto enter = [&](const std::shared_ptr<const Archive> &archive, int index, const FormatReader *with) {
                const ArchiveEntry &entry = archive->entries[size_t(index)];
                if (with) {
                    result.document = FormatRegistry::openEntryWith(*with, *archive, size_t(index), &result.error);
                    result.format = with->name();
                } else {
                    const FormatReader *used = nullptr;
                    result.document = registry->openEntry(*archive, size_t(index), &used, &result.error);
                    result.format = used ? used->name() : QString();
                }
                result.entries << entry.name;
                result.parent = archive;
            };
            for (qsizetype i = 0; i < entries.size(); ++i) {
                const QString &name = entries[i];
                if (!result.document)
                    break;
                const auto archive = archiveOf(*result.document);
                const int index = archive ? archive->find(name) : -1;
                if (index < 0) {
                    result.document.reset();
                    result.error = QObject::tr("%1 is not in this archive.").arg(name);
                    break;
                }
                enter(archive, index, i + 1 == entries.size() ? forced : nullptr);
            }
            for (int depth = 0; descend && !forced && result.document && depth < 8; ++depth) {
                const auto archive = archiveOf(*result.document);
                if (!archive || archive->entries.size() != 1)
                    break;
                const bool showsItself = std::any_of(result.document->tables.begin(), result.document->tables.end(),
                                                     [](const auto &t) {
                                                         return t->hasModel()
                                                             || (t->archive() && (t->hasAudio() || t->hasImage()));
                                                     });
                if (showsItself)
                    break;
                enter(archive, 0, nullptr);
                ++result.automatic;
            }
        } catch (const std::exception &e) {
            result.document.reset();
            result.error = QObject::tr("Unexpected error: %1").arg(QString::fromUtf8(e.what()));
        }
        return result;
    }));
}

void MainWindow::loadFinished()
{
    LoadResult result = m_loadWatcher->result();
    const QString path = std::exchange(m_loadingPath, QString());
    if (m_nextPath) {
        startNextLoad();
        return;
    }
    if (std::exchange(m_discardLoad, false))
        return;
    m_loadingTimer->stop();

    m_model->clear();
    m_shownTable.reset();
    {
        const QSignalBlocker blocker(m_tableBox);
        m_tableBox->clear();
    }
    m_document = std::move(result.document);
    m_imageTable.reset();
    const bool restore = !m_restoreFor.isEmpty() && m_restoreFor == path + u'\n' + result.entries.join(u'\n');
    const int restoreTable = std::exchange(m_restoreTable, -1);
    m_restoreFor.clear();
    m_currentPath = path;
    m_currentEntries = result.entries;
    m_currentFormat = result.format;
    m_parentArchive = result.parent;
    if (!restore)
        m_autoEntries = result.automatic;
    m_autoEntries = std::min<int>(m_autoEntries, int(m_currentEntries.size()));
    updateLocationBar();
    watchCurrentFile();

    refreshFormatChoices();
    if (forcedFormat() == 0 && result.entries.isEmpty()
        && (!m_document || result.format == QLatin1String("Hex")) && Rpf6Reader::needsKey(path)) {
        m_document.reset();
        showMessage(tr("<h3>%1 is an encrypted archive</h3>"
                       "<p>Its file list is encrypted with a key that is stored in the game's executable "
                       "(the <tt>main</tt> file in the game's ExeFS). Dump the ExeFS with your emulator, then:</p>"
                       "<p><a href=\"nxgv:rpf6-key\">Find the key in the game's executable…</a></p>"
                       "<p style=\"color: gray\">The key is read from your own copy of the game and kept in this "
                       "app's settings. Every archive of the game uses the same key. To see the raw bytes "
                       "instead, choose Format → Hex.</p>")
                        .arg(currentName().toHtmlEscaped()));
        m_info->setText(currentName());
        return;
    }
    if (forcedFormat() == 0 && result.entries.isEmpty() && SwitchPackageReader::needsKeys(path)) {
        m_document.reset();
        showMessage(tr("<h3>%1 is an encrypted Switch game</h3>"
                       "<p>Its files are encrypted with the keys of the console. To browse it, the app needs the "
                       "<tt>prod.keys</tt> file dumped from your own console.</p>"
                       "<p><a href=\"nxgv:switch-keys\">Choose prod.keys…</a></p>"
                       "<p style=\"color: gray\">The key file is only read, and its path is kept in this app's "
                       "settings. To see the raw bytes instead, choose Format → Hex.</p>")
                        .arg(currentName().toHtmlEscaped()));
        m_info->setText(currentName());
        return;
    }
    if (!m_document) {
        showMessage(tr("<h3>Could not open %1</h3><pre>%2</pre>")
                        .arg(currentName().toHtmlEscaped(), result.error.toHtmlEscaped()));
        m_info->setText(currentName());
        return;
    }
    if (m_document->tables.empty()) {
        showMessage(tr("<h3>%1 has no tables</h3>").arg(QFileInfo(path).fileName().toHtmlEscaped()));
        return;
    }

    {
        const QSignalBlocker blocker(m_tableBox);
        for (const auto &table : m_document->tables)
            m_tableBox->addItem(tr("%1  (%2 rows)").arg(table->name()).arg(table->rowCount()));
    }
    m_tableBox->setEnabled(m_document->tables.size() > 1);
    if (restore && restoreTable > 0 && restoreTable < m_tableBox->count()) {
        const QSignalBlocker blocker(m_tableBox);
        m_tableBox->setCurrentIndex(restoreTable);
    } else {
        m_rowFilter->clear();
    }
    m_stack->setCurrentIndex(1);
    rebuildShownTable(false);
}

void MainWindow::watchCurrentFile()
{
    const QStringList watched = m_fileWatcher->files() + m_fileWatcher->directories();
    if (!watched.isEmpty())
        m_fileWatcher->removePaths(watched);
    m_reloadAction->setEnabled(!m_currentPath.isEmpty());
    const QFileInfo info(m_currentPath);
    m_loadedStamp = info.lastModified();
    m_loadedSize = info.exists() ? info.size() : -1;
    if (!info.exists())
        return;
    m_fileWatcher->addPath(m_currentPath);
    m_fileWatcher->addPath(info.absolutePath());
}

void MainWindow::reloadCurrent(bool automatic)
{
    if (m_currentPath.isEmpty())
        return;
    const QFileInfo info(m_currentPath);
    if (!info.exists()) {
        if (!m_fileWatcher->files().isEmpty())
            m_fileWatcher->removePaths(m_fileWatcher->files());
        statusBar()->showMessage(tr("%1 is no longer on disk; the viewer still shows what was read before")
                                     .arg(QDir::toNativeSeparators(m_currentPath)));
        return;
    }
    if (!m_fileWatcher->files().contains(m_currentPath))
        m_fileWatcher->addPath(m_currentPath);
    if (automatic && info.lastModified() == m_loadedStamp && info.size() == m_loadedSize)
        return;
    m_restoreFor = m_currentPath + u'\n' + m_currentEntries.join(u'\n');
    m_restoreTable = m_tableBox->currentIndex();
    statusBar()->showMessage(automatic ? tr("%1 changed on disk and was reloaded").arg(info.fileName())
                                       : tr("Reloaded %1").arg(info.fileName()),
                             5000);
    openLocation(m_currentPath, m_currentEntries, false);
}

void MainWindow::updateLocationBar()
{
    m_backAction->setEnabled(!m_currentEntries.isEmpty());
    m_locationBar->setVisible(!m_currentEntries.isEmpty());
    QStringList parts{QFileInfo(m_currentPath).fileName().toHtmlEscaped()};
    for (const QString &entry : m_currentEntries)
        parts << entry.toHtmlEscaped();
    if (!parts.isEmpty())
        parts.last() = QStringLiteral("<b>%1</b>").arg(parts.last());
    const qsizetype firstAutomatic = parts.size() - m_autoEntries;
    for (qsizetype i = firstAutomatic; i < parts.size(); ++i)
        parts[i] = QStringLiteral("<span style=\"color: gray\">%1</span>").arg(parts[i]);
    m_backAction->setToolTip(m_autoEntries > 0
                                 ? tr("Back to %1, where you opened this file (Alt+Left). The gray part was opened "
                                      "for you because it was the only file inside")
                                       .arg(m_currentEntries.size() > m_autoEntries
                                                ? m_currentEntries[m_currentEntries.size() - m_autoEntries - 1]
                                                      .section(u'/', -1)
                                                : QFileInfo(m_currentPath).fileName())
                                 : tr("Back to the archive this file is in (Alt+Left)"));
    m_locationLabel->setText(parts.join(QStringLiteral(" &rsaquo; ")));
}

void MainWindow::showMessage(const QString &text)
{
    m_exportJsonAction->setEnabled(false);
    m_exportImageAction->setEnabled(false);
    m_replaceTextureAction->setEnabled(false);
    m_replaceFileAction->setEnabled(false);
    m_replaceJsonAction->setEnabled(false);
    setModelExportsEnabled(false);
    m_imageView->setImage({});
    if (m_modelView) {
        m_modelView->clear();
        m_modelView->hide();
    }
    m_videoView->clear();
    m_videoView->hide();
    m_shownModel.reset();
    m_shownTextures.clear();
    m_imageTable.reset();
    ++*m_imageRequest;
    m_audioPlayer->forget();
    m_playAction->setEnabled(false);
    m_playButtonAction->setVisible(false);
    m_exportWavAction->setEnabled(false);
    m_message->setText(text);
    m_stack->setCurrentIndex(0);
    m_info->clear();
    updateExtractAction();
}

void MainWindow::rebuildShownTable(bool keepView)
{
    std::shared_ptr<const Table> table;
    const int index = m_tableBox->currentIndex();
    if (m_document && index >= 0 && size_t(index) < m_document->tables.size())
        table = m_document->tables[size_t(index)];
    const std::shared_ptr<const Table> fileTable = table;
    if (table && m_encodingBox->currentIndex() > 0) {
        if (auto reencoded = table->withTextEncoding(allTextEncodings().at(size_t(m_encodingBox->currentIndex() - 1))))
            table = reencoded;
    }

    if (table) {
        if (std::shared_ptr<const RecordSource> records = table->recordSource()) {
            table = std::make_shared<RecordTable>(
                records, m_columns.fields(m_currentFormat, records->recordTypeName(), records->recordSize()));
        }
    }

    const int scroll = m_table->horizontalScrollBar()->value();
    m_shownTable = table;
    m_model->setTable(m_shownTable);
    m_exportJsonAction->setEnabled(table && table->canExportJson());
    updateExtractAction();
    m_replaceTextureAction->setEnabled(table && table->textureSlot());
    m_replaceFileAction->setEnabled(!m_currentPath.isEmpty() && QFileInfo(m_currentPath).isFile()
                                    && !(SwitchPackageReader::isPackageName(m_currentPath) && m_currentEntries.isEmpty()));
    m_replaceJsonAction->setEnabled(m_replaceFileAction->isEnabled()
                                    && (m_currentFormat == BymlReader().name() || m_currentFormat == MsbtReader().name()));
    m_archiveHint->setVisible(fileTable && fileTable->archive());
    const bool encodable = fileTable && fileTable->canChangeTextEncoding();
    m_encodingBox->setEnabled(encodable);
    m_encodingBox->setToolTip(encodable ? tr("How text is decoded in text files and in the hex view")
                                        : tr("How text is decoded in text files and in the hex view. "
                                             "Open a text file, or choose Format → Hex, to use it."));
    setHoverRow(-1);

    const bool hasModel = fileTable && fileTable->hasModel();
    const bool showModel = hasModel && m_modelView && !m_modelView->failed();
    if (fileTable && fileTable == m_imageTable.lock()) {
        if (m_modelView)
            m_modelView->setVisible(showModel);
        m_imageView->setVisible(!showModel && !m_imageView->image().isNull());
    } else {
        m_imageTable = fileTable;
        const quint64 request = ++*m_imageRequest;
        m_shownModel.reset();
        m_shownTextures.clear();
        m_imageView->setImage({});
        m_imageView->hide();
        if (m_modelView) {
            m_modelView->clear();
            m_modelView->setVisible(showModel);
        }
        m_exportImageAction->setEnabled(false);
        setModelExportsEnabled(false);
        if (hasModel) {
            if (showModel)
                m_modelView->setMessage(tr("Loading the model and its textures…"));
            m_modelWatcher->setFuture(QtConcurrent::run([fileTable, registry = m_registry, request,
                                                         latest = m_imageRequest, location = m_currentPath,
                                                         container = m_parentArchive] {
                ModelResult result;
                result.request = request;
                if (latest->load() != request)
                    return result;
                result.model = fileTable->model();
                if (result.model)
                    result.textures =
                        loadModelTextures(*result.model, *registry, &result.missing, nullptr, location, container);
                return result;
            }));
        }
        if (!showModel && fileTable && fileTable->hasImage()) {
            m_imageView->setPlaceholder(tr("Making the picture…"));
            m_imageView->show();
            m_imageWatcher->setFuture(QtConcurrent::run([fileTable, request, latest = m_imageRequest] {
                if (latest->load() != request)
                    return ImageResult{request, nullptr};
                return ImageResult{request, fileTable->image()};
            }));
        }
        m_videoView->clear();
        m_videoView->setVisible(fileTable && fileTable->hasVideo());
        if (fileTable && fileTable->hasVideo()) {
            const QString file = fileTable->videoFile();
            if (!file.isEmpty())
                m_videoView->playFile(file);
            else
                m_videoView->playData(fileTable->videoData(),
                                      QFileInfo(m_currentEntries.isEmpty() ? m_currentPath : m_currentEntries.last()).suffix());
        }
        if (!fileTable)
            m_imageTable.reset();
    }

    m_audioPlayer->forget();
    const bool sound = table && (table->hasAudio() || table->hasRowAudio());
    m_playAction->setEnabled(sound);
    m_playButtonAction->setVisible(sound);
    m_exportWavAction->setEnabled(sound);
    showPlaying(false);

    if (!keepView)
        m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_table->resizeColumnsToContents();
    for (int c = 0; c < m_model->columnCount(); ++c)
        m_table->setColumnWidth(c, std::min(m_table->columnWidth(c), 420));
    const int lastColumn = m_model->columnCount() - 1;
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setStretchLastSection(table && lastColumn >= 0 && !table->isNumeric(lastColumn));
    if (keepView)
        m_table->horizontalScrollBar()->setValue(scroll);
    updateInfo();
}

void MainWindow::imageFinished()
{
    const ImageResult result = m_imageWatcher->result();
    if (result.request != m_imageRequest->load())
        return;
    QImage picture;
    if (const std::shared_ptr<const Image> &image = result.image) {
        picture = QImage(reinterpret_cast<const uchar *>(image->rgba.constData()), image->width, image->height,
                         qsizetype(image->width) * 4, QImage::Format_RGBA8888,
                         [](void *keep) { delete static_cast<std::shared_ptr<const Image> *>(keep); },
                         new std::shared_ptr<const Image>(image));
    }
    m_imageView->setImage(picture);
    m_imageView->setVisible(!picture.isNull());
    m_exportImageAction->setEnabled(!picture.isNull());
}

void MainWindow::modelFinished()
{
    ModelResult result = m_modelWatcher->result();
    if (result.request != m_imageRequest->load())
        return;
    if (!result.model || result.model->primitives.empty()) {
        if (m_modelView)
            m_modelView->setMessage(tr("This model has no geometry that can be shown."));
        return;
    }
    m_shownModel = result.model;
    m_shownTextures = result.textures;
    setModelExportsEnabled(true);
    if (m_modelView && !m_modelView->failed()) {
        m_modelView->setModel(std::move(result.model), std::move(result.textures));
        m_exportImageAction->setEnabled(true);
    }
    if (!result.missing.isEmpty())
        m_info->setText(tr("Textures not found: %1").arg(result.missing.join(QStringLiteral(", "))));
}

void MainWindow::updateInfo()
{
    const Table *table = currentTable();
    if (!table) {
        m_info->clear();
        return;
    }
    const Table &t = *table;
    const int index = m_tableBox->currentIndex();
    QStringList parts{currentName(),
                      m_document && !m_document->container.isEmpty()
                          ? tr("%1 (%2 compressed)").arg(m_currentFormat, m_document->container)
                          : m_currentFormat};
    if (m_document->tables.size() > 1)
        parts << tr("table %1/%2").arg(index + 1).arg(m_document->tables.size());
    if (const QString d = t.description(); !d.isEmpty())
        parts << d;
    parts << tr("showing %1 rows").arg(m_proxy->rowCount());
    m_info->setText(parts.join(QStringLiteral("  •  ")));
}

int MainWindow::audioRow() const
{
    QModelIndex index = m_table->currentIndex();
    if (!index.isValid() && m_proxy->rowCount() > 0)
        index = m_proxy->index(0, 0);
    return index.isValid() ? m_proxy->mapToSource(index).row() : -1;
}

const Audio *MainWindow::decodedAudio()
{
    const Table *table = currentTable();
    if (!table)
        return nullptr;
    if (table->hasAudio())
        return m_audioPlayer->audioFor(*table);
    const int row = table->hasRowAudio() ? audioRow() : -1;
    return row >= 0 ? m_audioPlayer->audioFor(*table, row) : nullptr;
}

void MainWindow::playAudio(bool play)
{
    const Audio *audio = play ? decodedAudio() : nullptr;
    if (!audio) {
        m_audioPlayer->stop();
        return;
    }
    if (!m_audioPlayer->start(*audio))
        showMessage(tr("Cannot play the sound: no audio output is available."));
}

void MainWindow::showPlaying(bool playing)
{
    if (!m_playAction)
        return;
    {
        const QSignalBlocker blocker(m_playAction);
        m_playAction->setChecked(playing);
    }
    m_playAction->setText(playing ? tr("Stop") : tr("Play"));
    const Table *table = currentTable();
    const bool perRow = table && !table->hasAudio() && table->hasRowAudio();
    m_playAction->setToolTip(playing ? tr("Stop the sound")
                             : perRow ? tr("Play the sound selected in the list")
                                      : tr("Play the sound"));
    m_playAction->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
}

void MainWindow::extractGame()
{
    auto *dialog = new GameExtractDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &GameExtractDialog::openFolderRequested, this, [this](const QString &folder) { openFolder(folder); });
    dialog->open();
}

void MainWindow::showExtractHelp()
{
    QMessageBox box(QMessageBox::Information, tr("How to extract a game"),
                    tr("The app can unpack your own game dumps (NSP, XCI, NSZ or XCZ files) into the game's files."),
                    QMessageBox::Ok, this);
    box.setInformativeText(
        tr("<ol>"
           "<li>Choose <b>File → Extract game…</b></li>"
           "<li>Click <b>Choose keys…</b> and pick the <b>prod.keys</b> dumped from your own console. The app "
           "remembers it.</li>"
           "<li>Click <b>Add files…</b> or <b>Add folder…</b> to list your games.</li>"
           "<li>Select a game and click <b>Extract…</b>, then type a folder or pick one with <b>Browse…</b></li>"
           "</ol>"
           "<p>The game's <b>exefs</b> and <b>romfs</b> folders are saved there. Base games and DLC can be "
           "extracted; updates cannot yet.</p>"));
    box.exec();
}

void MainWindow::showAbout()
{
    QMessageBox box(QMessageBox::NoIcon, tr("About NX Game Viewer"),
                    tr("<h3>NX Game Viewer</h3>"
                       "<p>Browse, view, extract and mod the files of Nintendo Switch games.</p>"
                       "<p>MIT licence. The libraries and projects it builds on are listed in THIRD_PARTY.md.</p>"
                       "<p><a href=\"https://github.com/lextrack/NXGameViewer\">Source Code</a></p>"),
                    QMessageBox::Ok, this);
    box.setWindowFlags(withoutMinMax(box.windowFlags()));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.setIconPixmap(windowIcon().pixmap(64, 64));
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label")))
        label->setOpenExternalLinks(true);
    box.exec();
}
