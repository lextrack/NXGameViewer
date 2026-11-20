#pragma once

#include "formats/models/animationsearch.h"
#include "formats/core/batchexport.h"
#include "formats/media/mediaformats.h"
#include "formats/core/registry.h"
#include "fasticonprovider.h"
#include "formats/core/schema.h"
#include "formats/modding/nestededit.h"

#include <QFutureWatcher>

#include <atomic>
#include <functional>
#include <memory>
#include <QDateTime>
#include <QMainWindow>

class QAction;
class ImageView;
class ModelView;
class VideoView;
class AudioPlayer;
class QComboBox;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;
class QFileSystemWatcher;
class QTableView;
class QTreeView;
class HoverRowDelegate;
class TableModel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QString &startPath = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

    void setActiveMod(const QString &romfs);
    QString activeModFolder() const { return m_activeMod; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupToolbar();
    QWidget *setupBrowser();
    void setupViewer();
    void setupBackgroundWork();
    void connectSignals();
    void showWelcome();
    void chooseFolder();
    void chooseFile();
    void openFolder(const QString &folder);
    void openFile(const QString &path);
    void openPath(const QString &path);
    void openFileWith(const QString &path, int formatId);
    int forcedFormat() const;
    void selectFormat(int formatId);
    void refreshFormatChoices();
    void chooseOtherFormat();
    void addGroupedFormats(QMenu *menu, const QList<int> &skip, int checked,
                           const std::function<void(int formatId)> &pick);
    static QString formatFamily(const QString &readerName);
    void showFileMenu(const QPoint &pos);
    void openLocation(const QString &path, const QStringList &entries, bool descend);
    std::shared_ptr<const Archive> currentArchive() const;
    void openArchiveEntry(int row);
    QList<int> selectedArchiveRows() const;
    void showTableMenu(const QPoint &pos);
    void extractArchiveRows(bool selectionOnly);
    void extractShown();
    void extractPaths(const QStringList &paths);
    std::optional<BatchOptions> chooseExtractOptions(const QString &what, const QString &sourceFolder);
    void startExtraction(std::vector<ExtractItem> items, const BatchOptions &options);
    void showExtractSummary(const ExtractReport &report, const QString &folder);
    void updateExtractAction();
    QString currentName() const;
    void updateLocationBar();
    void revealCurrentFile();
    void showFolder(const QString &folder);
    void setHoverRow(int row);
    void startFileSearch();
    void revealMatch(const QString &path, bool open);
    void revealPendingMatch();
    void scrollIntoView(const QModelIndex &index);
    void updateSearchStatus();
    void nextMatch();
    void searchFinished();
    QString modFolder() const;
    bool chooseModFolder();
    bool newMod();
    void updateModMenu();
    bool isOpenGameFolder(const QString &folder) const;
    void showModHelp();
    void extractGame();
    void showExtractHelp();
    void showAbout();
    void watchCurrentFile();
    void reloadCurrent(bool automatic);
    void loadArchiveKeys();
    void findArchiveKey();
    void loadSwitchKeys();
    void chooseSwitchKeys();
    QString gamePath(const QString &path) const;
    static bool isXtxFile(const QString &path);
    void replaceTextureWithPng(const QString &path, const QStringList &entries = {}, const TextureSlot &slot = {});
    void replaceFileWith(const QString &path, const QStringList &entries = {});
    void replaceWithEditedJson(const QString &path, const QStringList &entries = {});
    void manageMods();

    struct ModSource
    {
        QString diskPath;
        QString package;
        QString packageEntry;
        QString outerName;
        QStringList nested;
        QString gamePath;
        QString target;
        std::optional<QString> titleId;
        QString game;
    };
    std::optional<ModSource> modSource(const QString &path, const QStringList &entries, QString *error);
    std::optional<QByteArray> modSourceBytes(const ModSource &source, QString *error);
    bool checkModTarget(const ModSource &source, const QString &title);
    struct ModSave
    {
        std::vector<RebuiltLayer> rebuilt;
        QString resourceSizes;
        bool resourceSizesHandled = false;
    };
    std::optional<ModSave> saveToMod(const ModSource &source, const QString &title, const LeafEdit &edit,
                                     bool askToBuildOnEarlier, bool needsOriginal = true);
    std::optional<std::pair<QString, QByteArray>> gameResourceSizeTable(const ModSource &source);
    QString updateResourceSizes(const ModSource &source, const QByteArray &file, bool *handled);
    QStringList selectedBrowserPaths() const;
    void showInFileManager(const QString &path);
    bool isLoading() const;
    void startNextLoad();
    void loadFinished();
    void rebuildShownTable(bool keepView);
    const Table *currentTable() const { return m_shownTable.get(); }

    QString columnsFolder() const;
    void loadGameProfiles(const QString &select);
    void loadHashNames();
    void selectGame(const QString &name);
    void createGame();
    void gameChosen(int index);
    void showColumnMenu(const QPoint &pos);
    void saveColumns();
    void showMessage(const QString &text);
    void exportCsv();
    void exportJson();
    void exportImage();
    void exportWav();
    void playAudio(bool play);
    void showPlaying(bool playing);
    const Audio *decodedAudio();
    int audioRow() const;
    void updateInfo();
    QString lastLocation() const;

    struct LoadResult
    {
        std::shared_ptr<const Document> document;
        QString format;
        QString error;
        QStringList entries;
        std::shared_ptr<const Archive> parent;
        int automatic = 0;
    };
    std::shared_ptr<const FormatRegistry> m_registry = std::make_shared<const FormatRegistry>(mediaFormatReaders());
    QFutureWatcher<LoadResult> *m_loadWatcher = nullptr;
    QString m_loadingPath;
    std::optional<QString> m_nextPath;
    QStringList m_nextEntries;
    bool m_nextDescend = true;
    QTimer *m_loadingTimer = nullptr;
    QWidget *m_loadingPage = nullptr;
    QLabel *m_loadingLabel = nullptr;

    struct ImageResult
    {
        quint64 request = 0;
        std::shared_ptr<const Image> image;
    };
    void imageFinished();
    QFutureWatcher<ImageResult> *m_imageWatcher = nullptr;
    std::shared_ptr<std::atomic<quint64>> m_imageRequest = std::make_shared<std::atomic<quint64>>(0);
    std::weak_ptr<const Table> m_imageTable;

    struct ModelResult
    {
        quint64 request = 0;
        std::shared_ptr<const Model3D> model;
        std::vector<std::shared_ptr<const Image>> textures;
        QStringList missing;
    };
    void modelFinished();
    void exportModel();
    void exportModelWithAnimations();
    void startModelExport(const QString &path, const std::vector<AnimationFile> &animations);
    void setModelExportsEnabled(bool enabled);
    QFutureWatcher<ModelResult> *m_modelWatcher = nullptr;
    struct ExportOutcome
    {
        bool ok = false;
        QString message;
    };
    QFutureWatcher<ExportOutcome> *m_exportWatcher = nullptr;
    ModelView *m_modelView = nullptr;
    VideoView *m_videoView = nullptr;
    QAction *m_exportModelAction = nullptr;
    QAction *m_exportAnimatedAction = nullptr;
    QFutureWatcher<std::vector<AnimationFile>> *m_animationSearch = nullptr;
    std::shared_ptr<const Model3D> m_shownModel;
    std::vector<std::shared_ptr<const Image>> m_shownTextures;
    std::shared_ptr<const Document> m_document;
    std::shared_ptr<const Table> m_shownTable;
    SchemaStore m_columns;
    QDateTime m_hashNamesTime;
    QString m_currentPath;
    QString m_currentFormat;
    QStringList m_currentEntries;
    QWidget *m_locationBar = nullptr;
    QLabel *m_locationLabel = nullptr;
    QAction *m_backAction = nullptr;
    QAction *m_extractAction = nullptr;

    FastIconProvider m_iconProvider;
    QFileSystemModel *m_fsModel = nullptr;
    QTreeView *m_tree = nullptr;
    QLineEdit *m_fileFilter = nullptr;
    HoverRowDelegate *m_hoverDelegate = nullptr;
    QLabel *m_archiveHint = nullptr;
    QLabel *m_searchStatus = nullptr;
    QTimer *m_searchTimer = nullptr;
    QFutureWatcher<QStringList> *m_searchWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_searchCancel;
    QStringList m_matches;
    int m_matchIndex = -1;
    bool m_searchTruncated = false;
    QString m_pendingMatch;
    QStringList m_searchExpanded;
    QLineEdit *m_folderEdit = nullptr;
    QAction *m_upAction = nullptr;
    bool m_revealCurrent = false;
    bool m_discardLoad = false;
    QAction *m_replaceTextureAction = nullptr;
    QAction *m_replaceFileAction = nullptr;
    QAction *m_replaceJsonAction = nullptr;
    QString m_activeMod;
    QAction *m_modFolderAction = nullptr;
    QAction *m_openModFolderAction = nullptr;
    QMenu *m_modMenu = nullptr;
    QFileSystemWatcher *m_fileWatcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
    QAction *m_reloadAction = nullptr;
    QString m_restoreFor;
    int m_restoreTable = -1;
    QDateTime m_loadedStamp;
    qint64 m_loadedSize = -1;
    QFutureWatcher<ExtractReport> *m_extractWatcher = nullptr;
    std::shared_ptr<const Archive> m_parentArchive;
    int m_autoEntries = 0;
    struct KeySearch
    {
        std::optional<QByteArray> key;
        QString error;
    };
    QFutureWatcher<KeySearch> *m_keyWatcher = nullptr;
    QWidget *m_noFolder = nullptr;
    QWidget *m_browserBar = nullptr;
    QAction *m_playButtonAction = nullptr;

    QStackedWidget *m_stack = nullptr;
    QLabel *m_message = nullptr;
    TableModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QComboBox *m_tableBox = nullptr;
    QLineEdit *m_rowFilter = nullptr;
    QTableView *m_table = nullptr;
    ImageView *m_imageView = nullptr;
    QSplitter *m_imageSplitter = nullptr;

    QComboBox *m_formatBox = nullptr;
    int m_shownFormatChoice = 0;
    QComboBox *m_gameBox = nullptr;
    QComboBox *m_encodingBox = nullptr;
    QAction *m_hexAction = nullptr;
    QAction *m_exportJsonAction = nullptr;
    QAction *m_exportImageAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_exportWavAction = nullptr;

    AudioPlayer *m_audioPlayer = nullptr;
    QLabel *m_info = nullptr;
};
