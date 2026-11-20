#pragma once

#include "core/registry.h"
#include "textures/texturesearch.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

enum BatchKind {
    BatchPicture = 1,
    BatchModel = 2,
    BatchSound = 4,
    BatchJson = 8,
    BatchCsv = 16,
};

struct BatchOptions
{
    int kinds = BatchPicture | BatchModel | BatchSound | BatchJson;
    QString outputFolder;
    QString baseFolder;
    bool keepFolders = true;
    bool replaceExisting = false;
    bool openArchives = true;
    bool keepUnconverted = true;
    bool keepOriginals = false;
};

struct BatchFileResult
{
    QString source;
    QStringList written;
    QStringList kept;
    QString error;
    int converted = 0;
    bool asIs = false;
};

struct ExtractItem
{
    QString diskPath;
    std::shared_ptr<const Archive> archive;
    size_t index = 0;
    QString label;
    QString output;
    QString base;
    QString location;
    int depth = 0;
};

struct ExtractReport
{
    QList<BatchFileResult> results;
    qint64 items = 0;
    qint64 archives = 0;
    bool canceled = false;
};

using ExtractProgress = std::function<bool(qint64 done, qint64 total)>;

QStringList collectBatchFiles(const QStringList &paths);
QString commonParentFolder(const QStringList &paths);
QString commonFolderOf(const QStringList &files);
QStringList batchOutputBases(const QStringList &files, const BatchOptions &options);
BatchFileResult exportBatchFile(const FormatRegistry &registry, const QString &source, const QString &outputBase,
                                const BatchOptions &options, TextureFolderCache *textures = nullptr);
QByteArray tableToCsv(const Table &table);

std::vector<ExtractItem> diskExtractItems(const QStringList &files, const BatchOptions &options);
std::vector<ExtractItem> archiveExtractItems(const std::shared_ptr<const Archive> &archive, const QList<int> &rows,
                                             const QString &location, const QString &label,
                                             const BatchOptions &options);
ExtractReport runExtraction(const FormatRegistry &registry, std::vector<ExtractItem> items, const BatchOptions &options,
                            const ExtractProgress &progress = {}, int threads = 0);
