#pragma once

#include "switchfs/switchgame.h"
#include "switchfs/switchkeys.h"
#include "switchfs/switchkeysearch.h"

#include <QDialog>
#include <QFutureWatcher>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QTimer;
class QLineEdit;
class QPushButton;
class QTreeWidget;

struct ScannedPackage
{
    QString path;
    std::vector<SwitchTitle> titles;
    QString error;
};

struct ExtractOutcome
{
    bool ok = false;
    SwitchExtractResult result;
    QString error;
};

struct PackageScan
{
    std::vector<ScannedPackage> packages;
    SwitchKeySearch keys;
    bool replacesList = false;
    QStringList withoutKeys;
};

class GameExtractDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GameExtractDialog(QWidget *parent = nullptr, std::optional<KeySearchPlaces> places = std::nullopt);
    ~GameExtractDialog() override;

    void addPackages(const QStringList &paths);
    bool setKeyFile(const QString &path, QString *error = nullptr);
    int titleCount() const;
    std::optional<SwitchTitle> selectedTitle() const;
    static QString suggestedFolderName(const SwitchTitle &title);
    bool isScanning() const;
    QString keyFile() const { return m_keyPath; }
    bool keysChosenByUser() const { return m_keysChosen; }

signals:
    void openFolderRequested(const QString &folder);

private:
    void chooseKeys();
    void addFiles();
    void addFolder();
    void showScanned(const std::vector<ScannedPackage> &packages);
    void updateButtons();
    void extractSelected();
    void runExtraction(const SwitchTitle &title, const QString &destination);
    void verifySelected();
    using Progress = std::function<bool(qint64 done, qint64 total)>;
    using TitleJob = std::function<ExtractOutcome(const Progress &progress)>;
    void startTitleJob(const QString &label, const QString &title, const TitleJob &job,
                       const std::function<void(const ExtractOutcome &, bool canceled)> &finished);
    KeySearchPlaces placesFor(const QStringList &packages) const;
    QStringList listedPackages() const;
    void applyKeys(const SwitchKeySearch &search, bool chosen);
    void showKeyProblem();
    void rebuildList();
    void setStatus(const QString &text);
    void flashStatus(const QString &text);

    QLabel *m_keysLabel = nullptr;
    QLabel *m_status = nullptr;
    QString m_baseStatus;
    QTimer *m_statusTimer = nullptr;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_extract = nullptr;
    QPushButton *m_verify = nullptr;
    std::optional<SwitchKeys> m_keys;
    QString m_keyPath;
    QString m_keyProblem;
    bool m_keysChosen = false;
    std::optional<KeySearchPlaces> m_places;
    std::vector<SwitchTitle> m_titles;
    QStringList m_pending;
    QFutureWatcher<PackageScan> *m_scan = nullptr;
    bool m_scanPending = false;
    QFutureWatcherBase *m_extraction = nullptr;
    std::shared_ptr<std::atomic<bool>> m_extractionCancel;
};

class ExtractDestinationDialog : public QDialog
{
    Q_OBJECT

public:
    ExtractDestinationDialog(const SwitchTitle &title, const QString &suggested, QWidget *parent = nullptr);
    QString destination() const;
    void setDestination(const QString &path);

private:
    void browse();

    QLineEdit *m_path = nullptr;
    QString m_folderName;
};
