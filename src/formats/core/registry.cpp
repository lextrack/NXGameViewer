#include "core/registry.h"

#include "sound/barsreader.h"
#include "data/bcsvreader.h"
#include "data/restblreader.h"
#include "data/ainbreader.h"
#include "data/evflreader.h"
#include "data/asbreader.h"
#include "data/baevreader.h"
#include "data/terrainreader.h"
#include "models/bfresreader.h"
#include "sound/bwavreader.h"
#include "sound/wavreader.h"
#include "data/bymlreader.h"
#include "data/msbtreader.h"
#include "data/nrbfreader.h"
#include "cri/crireader.h"
#include "cri/usmreader.h"
#include "textures/bntxreader.h"
#include "textures/ddsreader.h"
#include "data/falcomjsonreader.h"
#include "models/falcommdlreader.h"
#include "data/falcomscriptreader.h"
#include "data/falcomtblreader.h"
#include "archives/decompress.h"
#include "archives/zstddictionaries.h"
#include "data/hexreader.h"
#include "data/jsonreader.h"
#include "sound/nxopusreader.h"
#include "archives/rpf6reader.h"
#include "rage/awcreader.h"
#include "rage/ragetexturereader.h"
#include "rage/ragestringtablereader.h"
#include "rage/ragemodelreader.h"
#include "rage/rageanimation.h"
#include "archives/sarcreader.h"
#include "switchfs/switchpackagereader.h"
#include "data/textreader.h"
#include "textures/xtxreader.h"
#include "unity/unityreader.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QObject>

#include <algorithm>
#include <exception>
#include <new>
#include <optional>

FormatRegistry::FormatRegistry(std::vector<std::unique_ptr<FormatReader>> extraReaders)
{
    // ---> To support another game or file type, add its reader here. <---
    m_readers.push_back(std::make_unique<FalcomTblReader>());
    m_readers.push_back(std::make_unique<FalcomJsonReader>());
    m_readers.push_back(std::make_unique<FalcomScriptReader>());
    m_readers.push_back(std::make_unique<XtxReader>());
    m_readers.push_back(std::make_unique<BntxReader>());
    m_readers.push_back(std::make_unique<DdsReader>());
    m_readers.push_back(std::make_unique<BfresReader>());
    m_readers.push_back(std::make_unique<BymlReader>());
    m_readers.push_back(std::make_unique<MsbtReader>());
    m_readers.push_back(std::make_unique<BcsvReader>());
    m_readers.push_back(std::make_unique<RestblReader>());
    m_readers.push_back(std::make_unique<AinbReader>());
    m_readers.push_back(std::make_unique<EvflReader>());
    m_readers.push_back(std::make_unique<AsbReader>());
    m_readers.push_back(std::make_unique<BaevReader>());
    m_readers.push_back(std::make_unique<TerrainReader>());
    m_readers.push_back(std::make_unique<BwavReader>());
    m_readers.push_back(std::make_unique<WavReader>());
    m_readers.push_back(std::make_unique<BarsReader>());
    m_readers.push_back(std::make_unique<NxOpusReader>());
    m_readers.push_back(std::make_unique<NintendoOpusReader>());
    m_readers.push_back(std::make_unique<FalcomMdlReader>());
    m_readers.push_back(std::make_unique<SarcReader>());
    m_readers.push_back(std::make_unique<Rpf6Reader>());
    m_readers.push_back(std::make_unique<SwitchPackageReader>());
    m_readers.push_back(std::make_unique<RageModelReader>());
    m_readers.push_back(std::make_unique<RageAnimationReader>());
    m_readers.push_back(std::make_unique<RageTextureDictionaryReader>());
    m_readers.push_back(std::make_unique<RageStringTableReader>());
    m_readers.push_back(std::make_unique<RageLanguageTableReader>());
    m_readers.push_back(std::make_unique<AwcReader>());
    m_readers.push_back(std::make_unique<UnityBundleReader>());
    m_readers.push_back(std::make_unique<UnityAssetsReader>());
    m_readers.push_back(std::make_unique<UnityObjectReader>());
    m_readers.push_back(std::make_unique<NrbfReader>());
    m_readers.push_back(std::make_unique<CriUtfReader>());
    m_readers.push_back(std::make_unique<Afs2Reader>());
    m_readers.push_back(std::make_unique<UsmReader>());
    for (auto &reader : extraReaders)
        m_readers.push_back(std::move(reader));
    m_readers.push_back(std::make_unique<JsonReader>());
    m_readers.push_back(std::make_unique<TextReader>());
    m_readers.push_back(std::make_unique<HexReader>());  // fallback: opens anything
}

namespace {

constexpr qint64 kMaxWholeFile = qint64{1024} * 1024 * 1024;
constexpr qsizetype kHeadSize = 64;

QByteArrayView headOf(const QByteArray &bytes)
{
    return QByteArrayView(bytes).first(std::min<qsizetype>(bytes.size(), kHeadSize));
}

template <typename Function>
std::unique_ptr<Document> guarded(Function &&function, QString *error)
{
    QString e;
    try {
        if (auto doc = function(&e))
            return doc;
    } catch (const std::bad_alloc &) {
        e = QObject::tr("Not enough memory to open this file.");
    } catch (const std::exception &ex) {
        e = QObject::tr("Unexpected error: %1").arg(QString::fromUtf8(ex.what()));
    }
    if (error)
        *error = e;
    return nullptr;
}

std::optional<QByteArray> readCompressedFile(QFile &file, QString *error)
{
    if (file.size() > kMaxWholeFile) {
        if (error)
            *error = fileTooLargeMessage(kMaxWholeFile);
        return std::nullopt;
    }
    try {
        return file.readAll();
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory to open this file.");
        return std::nullopt;
    }
}

std::optional<QByteArray> compressedFileBytes(const QString &path, bool *compressed, QString *error)
{
    QFile file(path);
    *compressed = file.open(QIODevice::ReadOnly) && detectCompression(file.peek(4)) != Compression::None;
    if (!*compressed)
        return std::nullopt;
    loadZstdDictionariesNear(path);
    return readCompressedFile(file, error);
}

QString tooLargeMessage(const ArchiveEntry &entry)
{
    return QObject::tr("%1 is %2. Files over %3 inside archives can only be opened by formats that are read in "
                       "pieces (RPF6, USM, Unity bundles, Hex). Extract it to open it another way.")
        .arg(entry.name, QLocale().formattedDataSize(entry.size), QLocale().formattedDataSize(kMaxEntryInMemory));
}

QString joinedErrors(const QStringList &errors)
{
    return errors.isEmpty() ? QObject::tr("No reader can open this file.") : errors.join('\n');
}

}

const FormatReader *FormatRegistry::byName(const QString &name) const
{
    for (const auto &r : m_readers) {
        if (r->name() == name)
            return r.get();
    }
    return nullptr;
}

std::vector<const FormatReader *> FormatRegistry::scored(const QString &fileName, QByteArrayView head,
                                                         bool bytesOnly) const
{
    std::vector<std::pair<int, const FormatReader *>> scores;
    for (const auto &r : m_readers) {
        if (bytesOnly && !r->readsBytes())
            continue;
        if (const int s = r->score(fileName, head); s > 0)
            scores.emplace_back(s, r.get());
    }
    std::stable_sort(scores.begin(), scores.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

    std::vector<const FormatReader *> result;
    result.reserve(scores.size());
    for (const auto &[s, r] : scores)
        result.push_back(r);
    return result;
}

std::vector<const FormatReader *> FormatRegistry::candidates(const QString &path) const
{
    QByteArray head;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        head = file.read(kHeadSize);
    return scored(QFileInfo(path).fileName(), head, false);
}

std::vector<const FormatReader *> FormatRegistry::candidates(const QString &fileName, QByteArrayView head,
                                                             bool bytesOnly) const
{
    return scored(fileName, head, bytesOnly);
}

std::unique_ptr<Document> FormatRegistry::open(const QString &path, const FormatReader **usedReader,
                                               QString *error) const
{
    bool compressed = false;
    if (const std::optional<QByteArray> bytes = compressedFileBytes(path, &compressed, error); compressed)
        return bytes ? openBytes(*bytes, QFileInfo(path).fileName(), usedReader, error) : nullptr;
    QStringList errors;
    for (const FormatReader *reader : candidates(path)) {
        QString e;
        if (auto doc = readWith(*reader, path, &e)) {
            if (usedReader)
                *usedReader = reader;
            return doc;
        }
        errors << QStringLiteral("%1: %2").arg(reader->name(), e);
    }
    if (error)
        *error = joinedErrors(errors);
    return nullptr;
}

std::unique_ptr<Document> FormatRegistry::readWith(const FormatReader &reader, const QString &path, QString *error)
{
    return guarded([&](QString *e) { return reader.read(path, e); }, error);
}

std::unique_ptr<Document> FormatRegistry::openBytes(const QByteArray &data, const QString &fileName,
                                                    const FormatReader **usedReader, QString *error) const
{
    const std::optional<UnwrappedData> unwrapped = unwrapCompression(data, fileName, error);
    if (!unwrapped)
        return nullptr;
    QStringList containers;
    for (const QByteArray &head : unwrapped->layerHeads)
        containers << compressionName(detectCompression(head));

    QStringList errors;
    for (const FormatReader *reader : scored(unwrapped->name, headOf(unwrapped->bytes), true)) {
        QString e;
        if (auto doc = guarded([&](QString *inner) { return reader->readBytes(unwrapped->bytes, unwrapped->name, inner); },
                               &e)) {
            if (usedReader)
                *usedReader = reader;
            doc->container = containers.join(QStringLiteral(", "));
            return doc;
        }
        errors << QStringLiteral("%1: %2").arg(reader->name(), e);
    }
    if (error)
        *error = joinedErrors(errors);
    return nullptr;
}

std::unique_ptr<Document> FormatRegistry::openBytesWith(const FormatReader &reader, const QByteArray &data,
                                                        const QString &fileName, QString *error)
{
    if (!reader.readsBytes()) {
        if (error)
            *error = QObject::tr("%1 can only read files on disk, not files inside archives.").arg(reader.name());
        return nullptr;
    }
    const std::optional<UnwrappedData> unwrapped = unwrapCompression(
        data, fileName, error,
        [&reader](const QByteArray &bytes, const QString &name) { return reader.score(name, headOf(bytes)) > 0; });
    if (!unwrapped)
        return nullptr;
    return guarded([&](QString *e) { return reader.readBytes(unwrapped->bytes, unwrapped->name, e); }, error);
}

std::unique_ptr<Document> FormatRegistry::openWith(const FormatReader &reader, const QString &path, QString *error)
{
    if (!reader.readsBytes())
        return readWith(reader, path, error);
    bool compressed = false;
    const std::optional<QByteArray> bytes = compressedFileBytes(path, &compressed, error);
    if (!compressed)
        return readWith(reader, path, error);
    return bytes ? openBytesWith(reader, *bytes, QFileInfo(path).fileName(), error) : nullptr;
}

std::vector<const FormatReader *> FormatRegistry::storedReaders(const QString &fileName, QByteArrayView head) const
{
    std::vector<const FormatReader *> out;
    for (const FormatReader *reader : scored(fileName, head, false))
        if (reader->readsStored())
            out.push_back(reader);
    return out;
}

std::unique_ptr<Document> FormatRegistry::openEntry(const Archive &archive, size_t index,
                                                    const FormatReader **usedReader, QString *error) const
{
    const ArchiveEntry &entry = archive.entries.at(index);
    if (entry.size <= kMaxEntryInMemory) {
        const QByteArray data = archive.entryData(index);
        if (needsUnknownZstdDictionary(data))
            loadZstdDictionariesFrom(archive);
        return openBytes(data, entry.name, usedReader, error);
    }
    const StoredReader read = archive.entryReader(index);
    const QByteArray head = read(0, std::min<qint64>(entry.size, kHeadSize));
    QStringList errors;
    for (const FormatReader *reader : storedReaders(entry.name, head)) {
        QString e;
        if (auto doc = guarded([&](QString *inner) { return reader->readStored(read, entry.size, entry.name, inner); },
                               &e)) {
            if (usedReader)
                *usedReader = reader;
            return doc;
        }
        errors << QStringLiteral("%1: %2").arg(reader->name(), e);
    }
    if (error)
        *error = (QStringList{tooLargeMessage(entry)} + errors).join(u'\n');
    return nullptr;
}

std::unique_ptr<Document> FormatRegistry::openEntryWith(const FormatReader &reader, const Archive &archive,
                                                        size_t index, QString *error)
{
    const ArchiveEntry &entry = archive.entries.at(index);
    if (entry.size <= kMaxEntryInMemory)
        return openBytesWith(reader, archive.entryData(index), entry.name, error);
    if (!reader.readsStored()) {
        if (error)
            *error = tooLargeMessage(entry);
        return nullptr;
    }
    const StoredReader read = archive.entryReader(index);
    return guarded([&](QString *e) { return reader.readStored(read, entry.size, entry.name, e); }, error);
}
