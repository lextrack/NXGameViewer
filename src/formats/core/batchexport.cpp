#include "core/batchexport.h"

#include "core/paths.h"
#include "models/gltfwriter.h"
#include "textures/texturesearch.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>
#include <thread>

namespace {

QString tableFileName(QString name)
{
    static const QStringList pictures = {"dds", "png", "tga", "xtx", "bntx", "bmp", "jpg"};
    if (const qsizetype dot = name.lastIndexOf(u'.'); dot > 0 && pictures.contains(name.mid(dot + 1).toLower()))
        name.truncate(dot);
    const QString safe = safeFileName(name).replace(u' ', u'_');
    return safe.isEmpty() ? QStringLiteral("table") : safe;
}

QString csvField(QString text)
{
    if (text.contains(u',') || text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r')) {
        text.replace(QStringLiteral("\""), QStringLiteral("\"\""));
        return u'"' + text + u'"';
    }
    return text;
}

constexpr int kMaxExtractDepth = 8;
constexpr qint64 kCopyChunk = qint64{8} * 1024 * 1024;

void addError(BatchFileResult &result, const QString &message)
{
    if (!result.error.isEmpty())
        result.error += QStringLiteral("; ");
    result.error += message;
}

class Writer
{
public:
    Writer(const BatchOptions &options, BatchFileResult &result, QString original = {})
        : m_options(options), m_result(result), m_original(QDir::cleanPath(std::move(original)))
    {
    }

    bool isOriginal(const QString &path) const
    {
        return !m_original.isEmpty() && QDir::cleanPath(path).compare(m_original, Qt::CaseInsensitive) == 0;
    }
    bool sameAsOriginal() const { return m_sameAsOriginal; }

    bool mayWrite(const QString &path)
    {
        if (QFileInfo::exists(path) && !m_options.replaceExisting) {
            m_result.kept << path;
            return false;
        }
        return true;
    }

    void convert(const QString &path, const QByteArray &data)
    {
        if (isOriginal(path)) {
            m_sameAsOriginal = true;
            return;
        }
        if (write(path, data))
            ++m_result.converted;
    }

    bool write(const QString &path, const QByteArray &data)
    {
        if (!mayWrite(path))
            return false;
        QSaveFile out(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !out.open(QIODevice::WriteOnly) || out.write(data) < 0
            || !out.commit()) {
            addError(m_result, QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), out.errorString()));
            return false;
        }
        m_result.written << path;
        return true;
    }

    bool stream(const QString &path, const StoredReader &read, qint64 size, const std::atomic_bool *stop)
    {
        if (!mayWrite(path))
            return false;
        QSaveFile out(path);
        bool ok = QDir().mkpath(QFileInfo(path).absolutePath()) && out.open(QIODevice::WriteOnly);
        for (qint64 at = 0; ok && at < size; at += kCopyChunk) {
            if (stop && stop->load())
                return false;
            const qint64 length = std::min(kCopyChunk, size - at);
            const QByteArray chunk = read(at, length);
            ok = chunk.size() == length && out.write(chunk) == chunk.size();
        }
        if (!ok || !out.commit()) {
            addError(m_result, QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(),
                                                             ok ? out.errorString() : QObject::tr("could not be read")));
            return false;
        }
        m_result.written << path;
        return true;
    }

private:
    const BatchOptions &m_options;
    BatchFileResult &m_result;
    QString m_original;
    bool m_sameAsOriginal = false;
};

std::shared_ptr<const Archive> containerOf(const Document &doc)
{
    for (const auto &t : doc.tables)
        if (t->hasModel())
            return nullptr;
    return doc.archive();
}

const Table *archiveTable(const Document &doc)
{
    for (const auto &t : doc.tables)
        if (t->archive())
            return t.get();
    return nullptr;
}

void convertDocument(const Document &doc, bool recognised, const QString &outputBase, const BatchOptions &options,
                     Writer &writer, BatchFileResult &result, const FormatRegistry &registry,
                     TextureFolderCache *textures, const QString &location,
                     const std::shared_ptr<const Archive> &container, bool skipArchiveTables)
{
    auto wants = [&](BatchKind kind, auto &&qualifies) {
        std::vector<const Table *> tables;
        if (options.kinds & kind)
            for (const auto &t : doc.tables)
                if (qualifies(*t))
                    tables.push_back(t.get());
        return tables;
    };
    auto target = [&](const std::vector<const Table *> &tables, const Table *t, const char *extension) {
        QString path = outputBase;
        if (tables.size() > 1)
            path += u'_' + tableFileName(t->name());
        return path + u'.' + QLatin1String(extension);
    };

    const auto pictures = wants(BatchPicture, [](const Table &t) { return t.hasImage() && !t.hasModel(); });
    for (const Table *t : pictures)
        if (const auto image = t->image(); image && image->width > 0 && image->height > 0)
            writer.convert(target(pictures, t, "png"), encodePng(*image));

    const auto models = wants(BatchModel, [](const Table &t) { return t.hasModel(); });
    for (const Table *t : models) {
        const auto model = t->model();
        if (!model || model->primitives.empty())
            continue;
        writer.convert(target(models, t, "glb"),
                       writeGlb(*model, loadModelTextures(*model, registry, nullptr, textures, location, container)));
    }

    const auto sounds = wants(BatchSound, [](const Table &t) { return t.hasAudio(); });
    for (const Table *t : sounds) {
        QString soundError;
        if (const auto audio = t->decodeAudio(&soundError))
            writer.convert(target(sounds, t, "wav"), audio->toWav());
        else if (!soundError.isEmpty())
            addError(result, soundError);
    }

    const auto json = wants(BatchJson, [](const Table &t) { return t.canExportJson(); });
    for (const Table *t : json)
        writer.convert(target(json, t, "json"), t->exportJson());

    const Table *listing = skipArchiveTables ? archiveTable(doc) : nullptr;
    const auto csv = wants(BatchCsv, [recognised, listing](const Table &t) {
        return recognised && &t != listing && !t.hasImage() && !t.hasAudio() && t.columnCount() > 0;
    });
    for (const Table *t : csv)
        writer.convert(target(csv, t, "csv"), tableToCsv(*t));
}

QString withoutExtension(const QString &path)
{
    const QFileInfo info(path);
    const QString stem = info.completeBaseName();
    if (stem.isEmpty())
        return path;
    return info.path() == QStringLiteral(".") && !path.startsWith(QStringLiteral("./")) ? stem
                                                                                         : info.path() + u'/' + stem;
}

QString uniqueName(const QString &path, QSet<QString> &used)
{
    if (!used.contains(path.toLower())) {
        used.insert(path.toLower());
        return path;
    }
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const QString stem = suffix.isEmpty() ? path : path.chopped(suffix.size() + 1);
    for (int n = 2;; ++n) {
        const QString candidate = suffix.isEmpty() ? QStringLiteral("%1_%2").arg(stem).arg(n)
                                                   : QStringLiteral("%1_%2.%3").arg(stem).arg(n).arg(suffix);
        if (!used.contains(candidate.toLower())) {
            used.insert(candidate.toLower());
            return candidate;
        }
    }
}

void assignOutputs(std::vector<ExtractItem> &items, const QStringList &outputs)
{
    QHash<QString, int> stems;
    for (const QString &output : outputs)
        ++stems[withoutExtension(output).toLower()];
    QSet<QString> used;
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].output = uniqueName(outputs[qsizetype(i)], used);
        const QString stem = withoutExtension(items[i].output);
        items[i].base = stems.value(withoutExtension(outputs[qsizetype(i)]).toLower()) > 1 ? items[i].output : stem;
    }
}

}

QStringList collectBatchFiles(const QStringList &paths)
{
    QStringList files;
    QSet<QString> seen;
    auto add = [&](const QString &path) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!seen.contains(absolute)) {
            seen.insert(absolute);
            files << absolute;
        }
    };
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isFile()) {
            add(path);
        } else if (info.isDir()) {
            QStringList inside;
            QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext())
                inside << it.next();
            inside.sort(Qt::CaseInsensitive);
            for (const QString &file : inside)
                add(file);
        }
    }
    return files;
}

QString commonParentFolder(const QStringList &paths)
{
    QStringList folders;
    for (const QString &path : paths)
        folders << QFileInfo(path).absolutePath();
    if (folders.isEmpty())
        return {};
    QStringList common = QDir::cleanPath(folders.first()).split(u'/');
    for (const QString &folder : folders) {
        const QStringList parts = QDir::cleanPath(folder).split(u'/');
        qsizetype n = 0;
        while (n < common.size() && n < parts.size() && common[n] == parts[n])
            ++n;
        common = common.first(n);
    }
    const QString joined = common.join(u'/');
    return joined.isEmpty() && folders.first().startsWith(u'/') ? QStringLiteral("/") : joined;
}

QStringList batchOutputBases(const QStringList &files, const BatchOptions &options)
{
    const QDir base(options.baseFolder);
    const QDir output(options.outputFolder);
    std::vector<QString> folders;
    std::map<QString, int> stems;
    for (const QString &file : files) {
        const QFileInfo info(file);
        QString folder = output.absolutePath();
        if (options.keepFolders && !options.baseFolder.isEmpty()) {
            const QString relative = base.relativeFilePath(info.absolutePath());
            if (relative != QStringLiteral(".") && !relative.startsWith(QStringLiteral("..")) && !QDir::isAbsolutePath(relative))
                folder = output.filePath(relative);
        }
        folders.push_back(folder);
        ++stems[(folder + u'/' + info.completeBaseName()).toLower()];
    }
    QStringList bases;
    QSet<QString> used;
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QFileInfo info(files[i]);
        const QString &folder = folders[size_t(i)];
        const bool clash = stems[(folder + u'/' + info.completeBaseName()).toLower()] > 1;
        QString candidate = folder + u'/' + (clash ? info.fileName() : info.completeBaseName());
        QString unique = candidate;
        for (int n = 2; used.contains(unique.toLower()); ++n)
            unique = candidate + QStringLiteral("_%1").arg(n);
        used.insert(unique.toLower());
        bases << unique;
    }
    return bases;
}

QByteArray tableToCsv(const Table &table)
{
    QByteArray out;
    QStringList fields{QStringLiteral("row")};
    for (int c = 0; c < table.columnCount(); ++c)
        fields << csvField(table.columnName(c));
    out += fields.join(u',').toUtf8() + '\n';
    for (int r = 0; r < table.rowCount(); ++r) {
        fields = {csvField(table.rowLabel(r))};
        for (int c = 0; c < table.columnCount(); ++c)
            fields << csvField(table.text(r, c, DisplayMode::Auto));
        out += fields.join(u',').toUtf8() + '\n';
    }
    return out;
}

std::vector<ExtractItem> diskExtractItems(const QStringList &files, const BatchOptions &options)
{
    const QStringList bases = batchOutputBases(files, options);
    std::vector<ExtractItem> items;
    QSet<QString> used;
    const QDir base(options.baseFolder);
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QFileInfo info(files[i]);
        ExtractItem item;
        item.diskPath = info.absoluteFilePath();
        item.location = item.diskPath;
        item.label = options.baseFolder.isEmpty() ? QDir::toNativeSeparators(item.diskPath)
                                                  : QDir::toNativeSeparators(base.relativeFilePath(item.diskPath));
        item.base = bases[i];
        item.output = uniqueName(QFileInfo(bases[i]).path() + u'/' + info.fileName(), used);
        items.push_back(std::move(item));
    }
    return items;
}

namespace {

std::vector<ExtractItem> childItems(const std::shared_ptr<const Archive> &archive, const QList<int> &rows,
                                    const QString &folder, const QString &location, const QString &label, int depth,
                                    const BatchOptions &options)
{
    std::vector<ExtractItem> items;
    QStringList outputs;
    const bool single = archive->entries.size() == 1;
    for (int row : rows) {
        if (row < 0 || size_t(row) >= archive->entries.size())
            continue;
        const QString &name = archive->entries[size_t(row)].name;
        QString relative = options.keepFolders ? sanitizedRelativePath(name) : safeFileName(name.section(u'/', -1));
        if (relative.isEmpty())
            relative = QStringLiteral("entry_%1").arg(row);
        ExtractItem item;
        item.archive = archive;
        item.index = size_t(row);
        item.location = location;
        item.depth = depth;
        item.label = label.isEmpty() ? name : label + QStringLiteral(" › ") + name;
        if (single) {
            const QString suffix = QFileInfo(relative).suffix();
            outputs << (suffix.isEmpty() ? folder : folder + u'.' + suffix);
        } else {
            outputs << folder + u'/' + relative;
        }
        items.push_back(std::move(item));
    }
    assignOutputs(items, outputs);
    return items;
}

class Extraction
{
public:
    Extraction(const FormatRegistry &registry, const BatchOptions &options, const ExtractProgress &progress)
        : m_registry(registry), m_options(options), m_progress(progress)
    {
    }

    ExtractReport run(std::vector<ExtractItem> items, int threads)
    {
        m_total = qint64(items.size());
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            m_queue.push_back(std::move(*it));
        report();
        const int count = std::max(1, threads > 0 ? threads : QThread::idealThreadCount());
        std::vector<std::thread> workers;
        for (int i = 1; i < count; ++i)
            workers.emplace_back([this] { work(); });
        work();
        for (std::thread &t : workers)
            t.join();
        m_report.canceled = m_stop.load();
        m_report.items = m_done;
        return std::move(m_report);
    }

private:
    void work()
    {
        for (;;) {
            ExtractItem item;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_queue.empty() || m_active == 0 || m_stop.load(); });
                if (m_stop.load() || m_queue.empty()) {
                    m_wake.notify_all();
                    return;
                }
                item = std::move(m_queue.back());
                m_queue.pop_back();
                ++m_active;
            }
            std::vector<ExtractItem> children;
            std::optional<BatchFileResult> result;
            try {
                process(item, children, result);
            } catch (const std::bad_alloc &) {
                result = BatchFileResult{item.label, {}, {}, QObject::tr("Not enough memory to extract this file.")};
            }
            {
                std::lock_guard lock(m_mutex);
                if (result && (!result->written.isEmpty() || !result->kept.isEmpty() || !result->error.isEmpty()))
                    m_report.results << std::move(*result);
                if (!children.empty())
                    ++m_report.archives;
                m_total += qint64(children.size());
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    m_queue.push_back(std::move(*it));
                ++m_done;
                --m_active;
                report();
            }
            m_wake.notify_all();
        }
    }

    void report()
    {
        if (m_progress && !m_progress(m_done, m_total))
            m_stop.store(true);
    }

    std::unique_ptr<Document> open(const ExtractItem &item, const FormatReader **used, QString *error) const
    {
        if (item.archive)
            return m_registry.openEntry(*item.archive, item.index, used, error);
        return m_registry.open(item.diskPath, used, error);
    }

    void process(const ExtractItem &item, std::vector<ExtractItem> &children, std::optional<BatchFileResult> &out)
    {
        BatchFileResult result;
        result.source = item.label;
        const FormatReader *used = nullptr;
        QString error;
        std::unique_ptr<Document> doc = open(item, &used, &error);
        const bool recognised = doc && used && used->name() != QStringLiteral("Hex");
        const std::shared_ptr<const Archive> inner = recognised ? containerOf(*doc) : nullptr;
        if (inner && m_options.openArchives && item.depth < kMaxExtractDepth && !inner->entries.empty()) {
            QList<int> rows;
            for (int i = 0; i < int(inner->entries.size()); ++i)
                rows << i;
            children = childItems(inner, rows, item.base, item.location, item.label, item.depth + 1, m_options);
            return;
        }
        Writer writer(m_options, result, item.output);
        const bool plain = used && (used->name() == QStringLiteral("Text") || used->name() == QStringLiteral("JSON"));
        if (plain) {
            doc.reset();
            if (m_options.keepUnconverted || m_options.keepOriginals)
                saveOriginal(item, writer, result);
            out = std::move(result);
            return;
        }
        if (recognised)
            convertDocument(*doc, recognised, item.base, m_options, writer, result, m_registry, &m_textures,
                            item.location, item.archive, inner != nullptr);
        doc.reset();
        const bool produced = result.converted > 0 || !result.kept.isEmpty();
        if (m_options.keepOriginals || writer.sameAsOriginal() || (!produced && m_options.keepUnconverted))
            saveOriginal(item, writer, result);
        out = std::move(result);
    }

    void saveOriginal(const ExtractItem &item, Writer &writer, BatchFileResult &result)
    {
        if (item.archive && item.archive->extractor) {
            const std::optional<ExtractedFile> file = item.archive->extractor(item.index);
            if (!file) {
                addError(result, QObject::tr("could not be read"));
                return;
            }
            const QString name = safeFileName(file->name.section(u'/', -1));
            const QString target = QFileInfo(item.output).path() + u'/' + (name.isEmpty() ? QFileInfo(item.output).fileName() : name);
            if (writer.write(target, file->data))
                result.asIs = true;
            return;
        }
        if (item.archive) {
            const qint64 size = item.archive->entries[item.index].size;
            if (item.archive->loader && size <= kMaxEntryInMemory) {
                const QByteArray data = item.archive->entryData(item.index);
                if (data.isEmpty() && size > 0)
                    addError(result, QObject::tr("could not be read"));
                else if (writer.write(item.output, data))
                    result.asIs = true;
                return;
            }
            if (writer.stream(item.output, item.archive->entryReader(item.index), size, &m_stop))
                result.asIs = true;
            return;
        }
        const QFileInfo info(item.diskPath);
        if (info.absoluteFilePath() == QFileInfo(item.output).absoluteFilePath())
            return;
        auto file = std::make_shared<QFile>(item.diskPath);
        if (!file->open(QIODevice::ReadOnly)) {
            addError(result, file->errorString());
            return;
        }
        const StoredReader read = [file](qint64 at, qint64 length) {
            return file->seek(at) ? file->read(length) : QByteArray();
        };
        if (writer.stream(item.output, read, file->size(), &m_stop))
            result.asIs = true;
    }

    const FormatRegistry &m_registry;
    const BatchOptions &m_options;
    const ExtractProgress &m_progress;
    TextureFolderCache m_textures;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<ExtractItem> m_queue;
    int m_active = 0;
    qint64 m_done = 0;
    qint64 m_total = 0;
    std::atomic_bool m_stop{false};
    ExtractReport m_report;
};

}

std::vector<ExtractItem> archiveExtractItems(const std::shared_ptr<const Archive> &archive, const QList<int> &rows,
                                             const QString &location, const QString &label,
                                             const BatchOptions &options)
{
    if (!archive)
        return {};
    std::vector<ExtractItem> items = childItems(archive, rows, QDir(options.outputFolder).absolutePath(), location,
                                                label, 0, options);
    if (archive->entries.size() == 1 && !items.empty()) {
        QString relative = options.keepFolders ? sanitizedRelativePath(archive->entries.front().name)
                                               : safeFileName(archive->entries.front().name.section(u'/', -1));
        if (relative.isEmpty())
            relative = QStringLiteral("entry_0");
        items.front().output = QDir(options.outputFolder).absoluteFilePath(relative);
        items.front().base = withoutExtension(items.front().output);
    }
    return items;
}

ExtractReport runExtraction(const FormatRegistry &registry, std::vector<ExtractItem> items, const BatchOptions &options,
                            const ExtractProgress &progress, int threads)
{
    Extraction extraction(registry, options, progress);
    return extraction.run(std::move(items), threads);
}
