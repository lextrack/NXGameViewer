#include "cri/usmreader.h"

#include "core/simpletable.h"
#include "core/storedreader.h"
#include "cri/criutf.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QObject>
#include <QtEndian>

#include <functional>
#include <vector>

namespace {

constexpr qint64 kChunkHeader = 0x20;

struct Piece
{
    qint64 offset = 0;
    qint64 size = 0;
};

struct Stream
{
    QByteArray signature;
    int channel = 0;
    std::vector<Piece> pieces;
    qint64 size = 0;
    std::optional<CriUtfTable> header;
    QString name;
};

QString extensionFor(const QByteArray &signature, QByteArrayView head)
{
    if (head.startsWith("DKIF"))
        return QStringLiteral("ivf");
    if (head.startsWith(QByteArrayView("\x00\x00\x00\x01", 4)) || head.startsWith(QByteArrayView("\x00\x00\x01\x09", 4)))
        return QStringLiteral("h264");
    if (head.startsWith(QByteArrayView("\x00\x00\x01\xb3", 4)) || head.startsWith(QByteArrayView("\x00\x00\x01\xba", 4)))
        return QStringLiteral("m2v");
    if (head.size() >= 4 && (uchar(head[0]) & 0x7F) == 'H' && (uchar(head[1]) & 0x7F) == 'C' && (uchar(head[2]) & 0x7F) == 'A')
        return QStringLiteral("hca");
    if (head.size() >= 2 && uchar(head[0]) == 0x80 && uchar(head[1]) == 0x00)
        return QStringLiteral("adx");
    return signature == "@SFV" ? QStringLiteral("video.bin") : QStringLiteral("audio.bin");
}

QString headerSummary(const Stream &s)
{
    if (!s.header || s.header->rows.empty())
        return {};
    const CriUtfTable &h = *s.header;
    auto value = [&h](const char *column) { return h.value(0, QString::fromLatin1(column)); };
    if (s.signature == "@SFV") {
        QStringList parts;
        if (value("width").isValid() && value("height").isValid())
            parts << QStringLiteral("%1 × %2").arg(value("width").toLongLong()).arg(value("height").toLongLong());
        const qint64 frames = value("total_frames").toLongLong();
        const double n = value("framerate_n").toDouble(), d = value("framerate_d").toDouble();
        if (n > 0 && d > 0) {
            parts << QObject::tr("%1 fps").arg(QLocale().toString(n / d, 'f', 2));
            if (frames > 0)
                parts << QObject::tr("%1 s").arg(QLocale().toString(frames * d / n, 'f', 1));
        }
        if (frames > 0)
            parts << QObject::tr("%n frame(s)", nullptr, int(frames));
        return parts.join(QStringLiteral(", "));
    }
    QStringList parts;
    if (const qint64 rate = value("sampling_rate").toLongLong(); rate > 0)
        parts << QStringLiteral("%1 Hz").arg(rate);
    if (const qint64 channels = value("num_channels").toLongLong(); channels > 0)
        parts << QObject::tr("%n channel(s)", nullptr, int(channels));
    return parts.join(QStringLiteral(", "));
}

class StreamTable : public SimpleTable
{
public:
    StreamTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Streams"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("kind")}, {QObject::tr("details")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

std::unique_ptr<Document> parse(const StoredReader &readAt, qint64 fileSize, const QString &fileName, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    QMap<QPair<QByteArray, int>, Stream> streams;
    qint64 pos = 0;
    int damaged = 0;
    while (pos + kChunkHeader <= fileSize) {
        const QByteArray head = readAt(pos, kChunkHeader);
        if (head.size() != kChunkHeader)
            break;
        const QByteArray signature = head.left(4);
        const qint64 size = qFromBigEndian<quint32>(head.constData() + 4);
        const int dataOffset = uchar(head[9]);
        const int padding = qFromBigEndian<quint16>(head.constData() + 10);
        const int channel = uchar(head[12]);
        const int type = uchar(head[15]) & 3;
        if (pos == 0 && signature != "CRID")
            return fail(QObject::tr("Not a USM movie (no \"CRID\" at the start)."));
        if (size < dataOffset + padding || size > fileSize - pos - 8) {
            ++damaged;
            break;
        }
        const qint64 payloadAt = pos + 8 + dataOffset, payloadSize = size - dataOffset - padding;
        if (signature == "@SFV" || signature == "@SFA") {
            Stream &s = streams[{signature, channel}];
            s.signature = signature;
            s.channel = channel;
            if (type == 0 && payloadSize > 0) {
                s.pieces.push_back({payloadAt, payloadSize});
                s.size += payloadSize;
            } else if (type == 1 && !s.header) {
                s.header = readCriUtf(readAt(payloadAt, payloadSize), nullptr);
            }
        }
        pos += 8 + size;
    }
    if (streams.isEmpty())
        return fail(QObject::tr("This USM movie has no video or audio streams."));

    const QString base = QFileInfo(fileName).completeBaseName().isEmpty() ? QStringLiteral("movie")
                                                                          : QFileInfo(fileName).completeBaseName();
    auto source = std::make_shared<std::vector<Stream>>();
    std::vector<Stream> ordered;
    for (const char *signature : {"@SFV", "@SFA"})
        for (Stream &s : streams)
            if (s.signature == signature)
                ordered.push_back(std::move(s));
    for (Stream &s : ordered) {
        const QByteArray first = s.pieces.empty() ? QByteArray() : readAt(s.pieces.front().offset, std::min<qint64>(s.pieces.front().size, 16));
        const QString kind = s.signature == "@SFV" ? QStringLiteral("video") : QStringLiteral("audio");
        s.name = QStringLiteral("%1_%2%3.%4")
                     .arg(base, kind, s.channel ? QString::number(s.channel) : QString(), extensionFor(s.signature, first));
        source->push_back(std::move(s));
    }

    auto archive = std::make_shared<Archive>();
    std::vector<SimpleTable::Row> rows;
    for (const Stream &s : *source) {
        archive->entries.push_back({s.name, 0, s.size});
        rows.push_back({s.name, s.size, s.signature == "@SFV" ? QObject::tr("video") : QObject::tr("audio"), headerSummary(s)});
    }
    archive->loader = [source, readAt](size_t index) {
        if (index >= source->size())
            return QByteArray();
        const Stream &s = (*source)[index];
        QByteArray out;
        out.reserve(qsizetype(std::min<qint64>(s.size, qint64{1} << 30)));
        for (const Piece &p : s.pieces) {
            const QByteArray bytes = readAt(p.offset, p.size);
            if (bytes.size() != p.size)
                return QByteArray();
            out += bytes;
        }
        return out;
    };
    auto table = std::make_shared<StreamTable>(std::move(rows), archive);
    QString description = QObject::tr("CRI movie, %n stream(s)", nullptr, int(source->size()));
    if (damaged)
        description += QObject::tr("; the file ends early or is damaged");
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

}

int UsmReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("CRID") ? 100 : 0;
}

std::unique_ptr<Document> UsmReader::read(const QString &path, QString *error) const
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > kMaxFileSize) {
        if (error)
            *error = info.isFile() ? QObject::tr("File is larger than 4 GB.") : QObject::tr("The file does not exist.");
        return nullptr;
    }
    const qint64 fileSize = info.size();
    const QDateTime modified = info.lastModified();
    return parse(storedFileReader(path, fileSize, modified), fileSize, path, error);
}

std::unique_ptr<Document> UsmReader::readStored(const StoredReader &read, qint64 size, const QString &fileName,
                                                QString *error) const
{
    if (size > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 4 GB.");
        return nullptr;
    }
    return parse(read, size, fileName, error);
}

std::unique_ptr<Document> UsmReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return parse(storedBytesReader(data), data.size(), fileName, error);
}
