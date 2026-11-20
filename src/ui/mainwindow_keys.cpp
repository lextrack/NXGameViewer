#include "mainwindow.h"
#include "progressdialogs.h"
#include "formats/archives/rpf6reader.h"
#include "formats/switchfs/switchkeysearch.h"
#include "formats/switchfs/switchpackagereader.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPromise>
#include <QSettings>
#include <QtConcurrentRun>

namespace {

constexpr auto kKeysSetting = "rpf6Keys";
constexpr auto kSwitchKeysSetting = "switch/prodKeys";
constexpr auto kSwitchKeysChosenSetting = "switch/prodKeysChosen";

QString suggestedExecutable(const QString &archive)
{
    QString romfsParent;
    QDir dir = QFileInfo(archive).absoluteDir();
    for (int depth = 0; depth < 8; ++depth) {
        for (const QString &name : {QStringLiteral("exefs/main"), QStringLiteral("ExeFS/main")})
            if (QFileInfo::exists(dir.filePath(name)))
                return dir.filePath(name);
        const bool romfs = dir.dirName().compare(QLatin1String("romfs"), Qt::CaseInsensitive) == 0;
        if (!dir.cdUp())
            break;
        if (romfs) {
            romfsParent = dir.absolutePath();
            for (const QString &name : {QStringLiteral("exefs/main"), QStringLiteral("ExeFS/main")})
                if (QFileInfo::exists(dir.filePath(name)))
                    return dir.filePath(name);
            break;
        }
    }
    return romfsParent.isEmpty() ? QFileInfo(archive).absolutePath() : romfsParent;
}

}

void MainWindow::loadArchiveKeys()
{
    QList<QByteArray> keys;
    for (const QString &hex : QSettings().value(kKeysSetting).toStringList())
        keys << QByteArray::fromHex(hex.toLatin1());
    Rpf6Reader::setKeys(keys);
}

void MainWindow::loadSwitchKeys()
{
    const QString stored = QSettings().value(kSwitchKeysSetting).toString();
    if (!stored.isEmpty() && QFileInfo::exists(stored)) {
        SwitchPackageReader::setKeyFile(stored);
        return;
    }
    const SwitchKeySearch search = findSwitchKeys({}, KeySearchPlaces::forSystem({}));
    SwitchPackageReader::setKeyFile(search.found ? search.found->prodKeys : QString());
}

void MainWindow::chooseSwitchKeys()
{
    const QString current = SwitchPackageReader::keyFile();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose your console's keys"),
                                                      current.isEmpty() ? QDir::homePath() : QFileInfo(current).absolutePath(),
                                                      tr("Key files (*.keys *.txt *.ini);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!SwitchKeys::load(path, &error)) {
        QMessageBox::warning(this, tr("Choose keys"), error);
        return;
    }
    QSettings settings;
    settings.setValue(kSwitchKeysSetting, path);
    settings.setValue(kSwitchKeysChosenSetting, true);
    SwitchPackageReader::setKeyFile(path);
    if (!m_currentPath.isEmpty())
        openLocation(m_currentPath, m_currentEntries, false);
}

void MainWindow::findArchiveKey()
{
    if (m_keyWatcher)
        return;
    const QString archive = m_currentPath;
    QFile file(archive);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray head = file.read(32);
    file.close();
    if (head.size() < 32)
        return;
    const QString executable = QFileDialog::getOpenFileName(
        this, tr("Choose the game's executable"), suggestedExecutable(archive),
        tr("Game executable (main *.nso);;All files (*)"));
    if (executable.isEmpty())
        return;
    QFile exe(executable);
    if (!exe.open(QIODevice::ReadOnly) || exe.size() > qint64{1024} * 1024 * 1024) {
        QMessageBox::warning(this, tr("Find key"), tr("Cannot read %1.").arg(QDir::toNativeSeparators(executable)));
        return;
    }
    const QByteArray bytes = exe.readAll();

    QProgressDialog *progress = makeProgressDialog(
        this, tr("Looking for the key in %1…").arg(QFileInfo(executable).fileName()), tr("Find key"), 100);
    m_keyWatcher = new QFutureWatcher<KeySearch>(this);
    connect(m_keyWatcher, &QFutureWatcherBase::progressValueChanged, progress, &QProgressDialog::setValue);
    connect(progress, &QProgressDialog::canceled, m_keyWatcher, &QFutureWatcherBase::cancel);
    connect(m_keyWatcher, &QFutureWatcherBase::finished, this, [this, progress, archive] {
        const bool canceled = m_keyWatcher->isCanceled();
        const KeySearch result = m_keyWatcher->future().resultCount() > 0 ? m_keyWatcher->result() : KeySearch{};
        m_keyWatcher->deleteLater();
        m_keyWatcher = nullptr;
        closeProgressDialog(progress);
        if (canceled)
            return;
        if (!result.key) {
            QMessageBox::warning(this, tr("Find key"), result.error);
            return;
        }
        QStringList stored = QSettings().value(kKeysSetting).toStringList();
        const QString hex = QString::fromLatin1(result.key->toHex());
        if (!stored.contains(hex))
            stored << hex;
        QSettings().setValue(kKeysSetting, stored);
        loadArchiveKeys();
        m_info->setText(tr("Found the key. Archives of this game open now."));
        if (m_currentPath == archive && m_currentEntries.isEmpty())
            openLocation(archive, {}, true);
    });
    const QByteArray first = head.mid(16, 16);
    m_keyWatcher->setFuture(QtConcurrent::run([bytes, first](QPromise<KeySearch> &promise) {
        promise.setProgressRange(0, 100);
        KeySearch result;
        result.key = findRpf6Key(bytes, first, &result.error, [&promise](int percent) {
            promise.setProgressValue(percent);
            return !promise.isCanceled();
        });
        promise.addResult(result);
    }));
}
