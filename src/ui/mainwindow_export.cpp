#include "mainwindow.h"
#include "progressdialogs.h"
#include "animationdialog.h"
#include "batchexportdialog.h"
#include "formats/models/gltfwriter.h"
#include "imageview.h"
#include "core/paths.h"
#include "modelview.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QPromise>
#include <QtConcurrentRun>

#include <climits>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace {

QString csvField(QString text)
{
    if (text.contains(QLatin1Char(',')) || text.contains(QLatin1Char('"')) || text.contains(QLatin1Char('\n'))
        || text.contains(QLatin1Char('\r'))) {
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + text + QLatin1Char('"');
    }
    return text;
}

}

void MainWindow::updateExtractAction()
{
    if (!m_extractAction)
        return;
    const bool browsing = m_tree && !m_tree->isHidden();
    m_extractAction->setEnabled(!m_extractWatcher
                                && (currentArchive() || !m_currentEntries.isEmpty() || !m_currentPath.isEmpty() || browsing));
}

std::optional<BatchOptions> MainWindow::chooseExtractOptions(const QString &what, const QString &sourceFolder)
{
    if (m_extractWatcher)
        return std::nullopt;
    BatchExportDialog dialog(what, sourceFolder, this);
    if (dialog.exec() != QDialog::Accepted || m_extractWatcher)
        return std::nullopt;
    return dialog.options();
}

void MainWindow::extractShown()
{
    if (m_extractWatcher)
        return;
    if (currentArchive()) {
        extractArchiveRows(true);
        return;
    }
    if (!m_currentEntries.isEmpty() && m_parentArchive) {
        const int index = m_parentArchive->find(m_currentEntries.last());
        if (index >= 0) {
            const std::optional<BatchOptions> options = chooseExtractOptions(currentName(), {});
            if (!options)
                return;
            QStringList chain{QFileInfo(m_currentPath).fileName()};
            chain += m_currentEntries.first(m_currentEntries.size() - 1);
            startExtraction(archiveExtractItems(m_parentArchive, {index}, m_currentPath,
                                                chain.join(QStringLiteral(" › ")), *options),
                            *options);
            return;
        }
    }
    QStringList paths = m_tree && !m_tree->isHidden() ? selectedBrowserPaths() : QStringList();
    if (paths.isEmpty() && !m_currentPath.isEmpty())
        paths << m_currentPath;
    extractPaths(paths);
}

void MainWindow::extractPaths(const QStringList &paths)
{
    if (m_extractWatcher || paths.isEmpty())
        return;
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    const QStringList files = collectBatchFiles(paths);
    QGuiApplication::restoreOverrideCursor();
    if (files.isEmpty()) {
        showMessage(tr("There are no files to extract in the selection."));
        return;
    }

    const QString baseFolder = commonParentFolder(paths);
    const QString what = paths.size() == 1 && files.size() == 1
                             ? QFileInfo(files.first()).fileName()
                             : paths.size() == 1 ? tr("%1 (%n file(s))", nullptr, int(files.size())).arg(QFileInfo(paths.first()).fileName())
                                                 : tr("%n file(s) selected", nullptr, int(files.size()));
    std::optional<BatchOptions> options = chooseExtractOptions(what, baseFolder);
    if (!options)
        return;
    options->baseFolder = baseFolder;
    startExtraction(diskExtractItems(files, *options), *options);
}

void MainWindow::startExtraction(std::vector<ExtractItem> items, const BatchOptions &options)
{
    if (m_extractWatcher || items.empty())
        return;
    struct Job
    {
        std::shared_ptr<const FormatRegistry> registry;
        std::vector<ExtractItem> items;
        BatchOptions options;
        ExtractReport report;
    };
    auto job = std::make_shared<Job>();
    job->registry = m_registry;
    job->items = std::move(items);
    job->options = options;

    QProgressDialog *progress = makeProgressDialog(this, tr("Extracting…"), tr("Extract"), 0);

    m_extractWatcher = new QFutureWatcher<ExtractReport>(this);
    updateExtractAction();
    connect(m_extractWatcher, &QFutureWatcherBase::progressRangeChanged, progress, &QProgressDialog::setRange);
    connect(m_extractWatcher, &QFutureWatcherBase::progressValueChanged, progress, &QProgressDialog::setValue);
    connect(m_extractWatcher, &QFutureWatcherBase::progressTextChanged, progress, &QProgressDialog::setLabelText);
    connect(progress, &QProgressDialog::canceled, m_extractWatcher, &QFutureWatcherBase::cancel);
    connect(m_extractWatcher, &QFutureWatcherBase::finished, this, [this, progress, job] {
        const bool canceled = m_extractWatcher->isCanceled();
        m_extractWatcher->deleteLater();
        m_extractWatcher = nullptr;
        closeProgressDialog(progress);
        updateExtractAction();
        job->report.canceled = job->report.canceled || canceled;
        showExtractSummary(job->report, job->options.outputFolder);
    });

    m_extractWatcher->setFuture(QtConcurrent::run([job](QPromise<ExtractReport> &promise) {
        job->report = runExtraction(*job->registry, std::move(job->items), job->options,
                                    [&promise](qint64 done, qint64 total) {
                                        const int maximum = int(std::min<qint64>(total, INT_MAX));
                                        promise.setProgressRange(0, maximum);
                                        promise.setProgressValueAndText(
                                            int(std::min<qint64>(done, maximum)),
                                            QObject::tr("Extracting… %1 of %2 files").arg(done).arg(total));
                                        return !promise.isCanceled();
                                    });
    }));
}

void MainWindow::showExtractSummary(const ExtractReport &report, const QString &folder)
{
    qsizetype outputs = 0, converted = 0, asIs = 0, failed = 0, kept = 0;
    QStringList details, written;
    for (const BatchFileResult &r : report.results) {
        outputs += r.written.size();
        written += r.written;
        kept += r.kept.size();
        if (r.converted > 0)
            ++converted;
        if (r.asIs)
            ++asIs;
        if (!r.error.isEmpty()) {
            ++failed;
            details << tr("Failed: %1 — %2").arg(r.source, QString(r.error).replace(u'\n', u' '));
        }
        for (const QString &k : r.kept)
            details << tr("Kept existing: %1").arg(QDir::toNativeSeparators(k));
    }

    QStringList lines;
    if (report.canceled)
        lines << tr("Extraction canceled after %n file(s).", nullptr, int(report.items));
    else if (outputs == 0)
        lines << tr("Nothing was extracted.");
    else
        lines << tr("Extracted %n file(s).", nullptr, int(outputs));
    if (converted > 0)
        lines << tr("%n file(s) were converted to common formats.", nullptr, int(converted));
    if (asIs > 0)
        lines << tr("%n file(s) were saved as they are.", nullptr, int(asIs));
    if (report.archives > 0)
        lines << tr("%n archive(s) were opened and their files extracted.", nullptr, int(report.archives));
    if (kept > 0)
        lines << tr("%n file(s) already existed and were left as they were.", nullptr, int(kept));
    if (failed > 0)
        lines << tr("%n file(s) could not be extracted.", nullptr, int(failed));
    const QString savedIn = written.isEmpty() ? folder : commonParentFolder(written);
    if (!written.isEmpty())
        lines << QString() << tr("Saved in %1").arg(QDir::toNativeSeparators(savedIn));
    m_info->setText(written.isEmpty() ? lines.first()
                                      : tr("%1 Saved in %2").arg(lines.first(), QDir::toNativeSeparators(savedIn)));

    QMessageBox box(failed > 0 ? QMessageBox::Warning : QMessageBox::Information, tr("Extract"), lines.join(u'\n'),
                    QMessageBox::Ok, this);
    if (!details.isEmpty())
        box.setDetailedText(details.join(u'\n'));
    QPushButton *open = written.isEmpty() ? nullptr
                      : box.addButton(written.size() == 1 ? tr("Show file") : tr("Open folder"), QMessageBox::ActionRole);
    if (open)
        open->setToolTip(written.size() == 1 ? tr("Show the extracted file in your file manager")
                                             : tr("Open the folder the files were saved in"));
    box.exec();
    if (open && box.clickedButton() == open) {
        if (written.size() == 1)
            showInFileManager(written.first());
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(savedIn));
    }
}

void MainWindow::exportModel()
{
    if (!m_shownModel || m_exportWatcher->isRunning())
        return;
    const QString suggested = QFileInfo(currentName()).completeBaseName() + ".glb";
    const QString path = QFileDialog::getSaveFileName(this, tr("Export model"), QDir::home().filePath(suggested),
                                                      tr("glTF binary (*.glb)"));
    if (path.isEmpty())
        return;
    startModelExport(path, {});
}

void MainWindow::exportModelWithAnimations()
{
    if (!m_shownModel || m_exportWatcher->isRunning() || m_animationSearch)
        return;
    const std::shared_ptr<const Model3D> model = m_shownModel;
    const QString modelPath = m_currentPath;
    setModelExportsEnabled(false);
    m_info->setText(tr("Looking for animations…"));
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    m_animationSearch = new QFutureWatcher<std::vector<AnimationFile>>(this);
    connect(m_animationSearch, &QFutureWatcherBase::finished, this, [this, model, modelPath] {
        QGuiApplication::restoreOverrideCursor();
        const std::vector<AnimationFile> found = m_animationSearch->result();
        m_animationSearch->deleteLater();
        m_animationSearch = nullptr;
        setModelExportsEnabled(m_shownModel != nullptr);
        updateInfo();
        if (model != m_shownModel)
            return;
        AnimationDialog dialog(found, model, QFileInfo(modelPath).absolutePath(), this);
        if (dialog.exec() != QDialog::Accepted || model != m_shownModel)
            return;
        const std::vector<AnimationFile> animations = dialog.selected();
        const QString suggested = QFileInfo(modelPath).completeBaseName() + "_animated.glb";
        const QString path = QFileDialog::getSaveFileName(this, tr("Export model with animations"),
                                                          QDir::home().filePath(suggested), tr("glTF binary (*.glb)"));
        if (!path.isEmpty())
            startModelExport(path, animations);
    });
    m_animationSearch->setFuture(
        QtConcurrent::run([model, modelPath] { return findAnimationFiles(modelPath, *model); }));
}

void MainWindow::startModelExport(const QString &path, const std::vector<AnimationFile> &animationFiles)
{
    if (!m_shownModel || m_exportWatcher->isRunning())
        return;
    setModelExportsEnabled(false);
    m_info->setText(tr("Exporting %1…").arg(QFileInfo(path).fileName()));
    m_exportWatcher->setFuture(
        QtConcurrent::run([model = m_shownModel, textures = m_shownTextures, path, animationFiles]() -> ExportOutcome {
            try {
                QStringList problems;
                const std::vector<ModelAnimation> animations = loadAnimations(animationFiles, *model, &problems);
                const QByteArray glb = writeGlb(*model, textures, {}, animations);
                QSaveFile out(path);
                if (!out.open(QIODevice::WriteOnly) || out.write(glb) < 0 || !out.commit())
                    return {false, out.errorString()};
                if (animationFiles.empty())
                    return {true, QObject::tr("Exported the model to %1").arg(path)};
                QString message = QObject::tr("Exported the model with %n animation(s) to %1", nullptr,
                                              int(animations.size()))
                                      .arg(path);
                if (!problems.isEmpty())
                    message += QObject::tr(" — %n animation(s) skipped: %1", nullptr, int(problems.size()))
                                   .arg(problems.join(QStringLiteral("; ")));
                return {true, message};
            } catch (const std::bad_alloc &) {
                return {false, QObject::tr("Not enough memory to export this model.")};
            }
        }));
}

void MainWindow::setModelExportsEnabled(bool enabled)
{
    m_exportModelAction->setEnabled(enabled);
    m_exportAnimatedAction->setEnabled(enabled);
}

void MainWindow::exportCsv()
{
    const Table *table = currentTable();
    if (!table || m_proxy->columnCount() == 0)
        return;

    const QString suggested = QFileInfo(currentName()).completeBaseName() + '_' + table->name() + ".csv";
    const QString path = QFileDialog::getSaveFileName(this, tr("Export CSV"),
                                                      QDir::home().filePath(suggested),
                                                      tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export failed"), out.errorString());
        return;
    }

    QStringList fields{QStringLiteral("row")};
    for (int c = 0; c < m_proxy->columnCount(); ++c)
        fields << csvField(m_proxy->headerData(c, Qt::Horizontal).toString());
    out.write(fields.join(',').toUtf8() + '\n');

    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        fields = {csvField(m_proxy->headerData(r, Qt::Vertical).toString())};
        for (int c = 0; c < m_proxy->columnCount(); ++c)
            fields << csvField(m_proxy->index(r, c).data().toString());
        out.write(fields.join(',').toUtf8() + '\n');
    }

    if (out.commit())
        m_info->setText(tr("Exported %1 rows to %2").arg(m_proxy->rowCount()).arg(path));
    else
        QMessageBox::warning(this, tr("Export failed"), out.errorString());
}

void MainWindow::exportJson()
{
    const Table *table = currentTable();
    if (!table || !table->canExportJson())
        return;

    const QString suggested = currentName() + ".plain.json";
    const QString path = QFileDialog::getSaveFileName(this, tr("Export JSON"),
                                                      QDir::home().filePath(suggested),
                                                      tr("JSON files (*.json)"));
    if (path.isEmpty())
        return;

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(table->exportJson()) < 0 || !out.commit()) {
        QMessageBox::warning(this, tr("Export failed"), out.errorString());
        return;
    }
    m_info->setText(tr("Exported JSON to %1").arg(path));
}

void MainWindow::exportImage()
{
    const QImage image = m_modelView && m_modelView->isVisible() ? m_modelView->grabCleanImage() : m_imageView->image();
    const Table *table = currentTable();
    if (image.isNull() || !table)
        return;

    QString suggested = QFileInfo(currentName()).completeBaseName();
    if (m_document->tables.size() > 1)
        suggested += '_' + safeFileName(table->name()).replace(' ', '_');
    const QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"),
                                                      QDir::home().filePath(suggested + ".png"),
                                                      tr("PNG images (*.png)"));
    if (path.isEmpty())
        return;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || !image.save(&out, "PNG") || !out.commit()) {
        QMessageBox::warning(this, tr("Export failed"), out.errorString());
        return;
    }
    m_info->setText(tr("Exported PNG to %1").arg(path));
}

void MainWindow::exportWav()
{
    const Audio *audio = decodedAudio();
    if (!audio)
        return;
    QString suggested = QFileInfo(currentName()).completeBaseName() + ".wav";
    const Table *table = currentTable();
    const auto archive = currentArchive();
    const int row = audioRow();
    if (table && !table->hasAudio() && archive && row >= 0 && size_t(row) < archive->entries.size())
        suggested = QFileInfo(archive->entries[size_t(row)].name).completeBaseName() + ".wav";
    const QString path = QFileDialog::getSaveFileName(this, tr("Export WAV"), QDir::home().filePath(suggested),
                                                      tr("WAV sounds (*.wav)"));
    if (path.isEmpty())
        return;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(audio->toWav()) < 0 || !out.commit()) {
        QMessageBox::warning(this, tr("Export failed"), out.errorString());
        return;
    }
    m_info->setText(tr("Exported WAV to %1").arg(path));
}
