#pragma once

#include <QByteArrayView>
#include <QString>
#include <QVariant>

#include "models/model3d.h"
#include "core/storedreader.h"
#include "core/textdecode.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

enum class DisplayMode { Auto, Hex };

constexpr qint64 kMaxEntryInMemory = qint64{1024} * 1024 * 1024;

class RecordSource;

struct ArchiveEntry
{
    QString name;
    qsizetype offset = 0;
    qsizetype size = 0;
    QByteArray head = {};
};

struct ExtractedFile
{
    QString name;
    QByteArray data;
};

struct Archive
{
    QByteArray data;
    std::vector<ArchiveEntry> entries;
    std::function<QByteArray(size_t index)> loader = {};
    std::function<std::optional<ExtractedFile>(size_t index)> extractor = {};
    std::function<StoredReader(size_t index)> stream = {};
    std::function<QByteArray(size_t index, qsizetype length)> headLoader = {};

    QByteArray entryData(size_t index) const
    {
        const ArchiveEntry &e = entries.at(index);
        if (e.size > kMaxEntryInMemory)
            return {};
        if (loader)
            return loader(index);
        return data.mid(e.offset, e.size);
    }
    QByteArray entryHead(size_t index, qsizetype length) const
    {
        const ArchiveEntry &e = entries.at(index);
        if (!e.head.isEmpty())
            return e.head.left(length);
        if (headLoader)
            return headLoader(index, length);
        if (loader)
            return {};
        return data.mid(e.offset, std::min(e.size, length));
    }
    StoredReader entryReader(size_t index) const
    {
        if (stream)
            return stream(index);
        if (loader)
            return storedBytesReader(entryData(index));
        const ArchiveEntry &e = entries.at(index);
        return [bytes = data, offset = e.offset, size = e.size](qint64 at, qint64 length) {
            if (at < 0 || length < 0 || at > size - length)
                return QByteArray();
            return bytes.mid(qsizetype(offset + at), qsizetype(length));
        };
    }
    int find(const QString &name) const
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].name == name)
                return int(i);
        return -1;
    }
};

struct Image
{
    int width = 0;
    int height = 0;
    QByteArray rgba;
};

struct Audio
{
    int sampleRate = 0;
    int channels = 0;
    QByteArray pcm;

    qint64 frameCount() const { return channels > 0 ? pcm.size() / (2 * channels) : 0; }
    QByteArray toWav() const;
};

struct TextureSlot
{
    qsizetype container = 0;
    int index = 0;
    QString name;
};

class Table
{
public:
    Table() = default;
    virtual ~Table() = default;

    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;
    Table(Table &&) = delete;
    Table &operator=(Table &&) = delete;

    virtual QString name() const = 0;
    virtual int rowCount() const = 0;
    virtual int columnCount() const = 0;
    virtual QString columnName(int column) const = 0;
    virtual QString columnToolTip(int /*column*/) const { return {}; }
    virtual QString text(int row, int column, DisplayMode mode) const = 0;
    virtual QVariant sortKey(int row, int column) const { return text(row, column, DisplayMode::Auto); }
    virtual QString toolTip(int /*row*/, int /*column*/) const { return {}; }
    virtual bool isNumeric(int /*column*/) const { return false; }
    virtual QString rowLabel(int row) const { return QString::number(row); }
    virtual bool monospace() const { return false; }
    virtual QString description() const { return {}; }
    virtual bool canExportJson() const { return false; }
    virtual QByteArray exportJson() const { return {}; }
    virtual std::shared_ptr<const RecordSource> recordSource() const { return nullptr; }
    virtual bool hasImage() const { return false; }
    virtual std::shared_ptr<const Image> image() const { return nullptr; }
    virtual std::optional<TextureSlot> textureSlot() const { return std::nullopt; }
    virtual bool hasModel() const { return false; }
    virtual std::shared_ptr<const Model3D> model() const { return nullptr; }
    virtual std::shared_ptr<const Table> withTextEncoding(TextEncoding /*encoding*/) const { return nullptr; }
    virtual bool canChangeTextEncoding() const { return false; }
    virtual std::shared_ptr<const Archive> archive() const { return nullptr; }
    virtual bool hasAudio() const { return false; }
    virtual std::optional<Audio> decodeAudio(QString * /*error*/) const { return std::nullopt; }
    virtual bool hasRowAudio() const { return false; }
    virtual std::optional<Audio> decodeRowAudio(int /*row*/, QString * /*error*/) const { return std::nullopt; }
    virtual bool hasVideo() const { return false; }
    virtual QString videoFile() const { return {}; }
    virtual QByteArray videoData() const { return {}; }
};

struct Document
{
    std::vector<std::shared_ptr<const Table>> tables;
    QString container;

    std::shared_ptr<const Archive> archive() const
    {
        for (const auto &t : tables)
            if (auto a = t->archive())
                return a;
        return nullptr;
    }
};

class FormatReader
{
public:
    FormatReader() = default;
    virtual ~FormatReader() = default;

    FormatReader(const FormatReader &) = delete;
    FormatReader &operator=(const FormatReader &) = delete;
    FormatReader(FormatReader &&) = delete;
    FormatReader &operator=(FormatReader &&) = delete;

    virtual QString name() const = 0;
    virtual int score(const QString &fileName, QByteArrayView head) const = 0;
    virtual std::unique_ptr<Document> read(const QString &path, QString *error) const = 0;
    virtual bool readsBytes() const { return false; }
    virtual std::unique_ptr<Document> readBytes(const QByteArray & /*data*/, const QString & /*fileName*/,
                                                QString * /*error*/) const
    {
        return nullptr;
    }

    virtual bool readsStored() const { return false; }
    virtual std::unique_ptr<Document> readStored(const StoredReader & /*read*/, qint64 /*size*/,
                                                 const QString & /*fileName*/, QString * /*error*/) const
    {
        return nullptr;
    }
};
