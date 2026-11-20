#include "media/videofilereader.h"

#include "core/simpletable.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QtEndian>

#include <bit>
#include <cmath>

namespace {

constexpr quint32 kEbml = 0x1A45DFA3;
constexpr quint32 kDocType = 0x4282;
constexpr quint32 kSegment = 0x18538067;
constexpr quint32 kInfo = 0x1549A966;
constexpr quint32 kTimecodeScale = 0x2AD7B1;
constexpr quint32 kDuration = 0x4489;
constexpr quint32 kTracks = 0x1654AE6B;
constexpr quint32 kTrackEntry = 0xAE;
constexpr quint32 kTrackType = 0x83;
constexpr quint32 kCodecId = 0x86;
constexpr quint32 kDefaultDuration = 0x23E383;
constexpr quint32 kVideo = 0xE0;
constexpr quint32 kPixelWidth = 0xB0;
constexpr quint32 kPixelHeight = 0xBA;
constexpr quint32 kAudio = 0xE1;
constexpr quint32 kSamplingFrequency = 0xB5;
constexpr quint32 kChannels = 0x9F;
constexpr quint32 kCluster = 0x1F43B675;
constexpr int kMaxDepth = 8;

struct Element
{
    quint32 id = 0;
    qint64 start = 0;
    qint64 end = 0;
};

class Ebml
{
public:
    explicit Ebml(QByteArrayView data) : m_data(data) {}

    std::optional<Element> next(qint64 &at, qint64 limit) const
    {
        if (at >= limit)
            return std::nullopt;
        int length = 0;
        const quint64 id = vint(at, limit, true, &length);
        if (length == 0 || length > 4)
            return std::nullopt;
        at += length;
        bool unknown = false;
        const quint64 size = vint(at, limit, false, &length, &unknown);
        if (length == 0)
            return std::nullopt;
        at += length;
        Element e{quint32(id), at, unknown || size > quint64(limit - at) ? limit : at + qint64(size)};
        return e;
    }
    quint64 uint(const Element &e) const
    {
        quint64 v = 0;
        for (qint64 i = e.start; i < e.end && i - e.start < 8; ++i)
            v = v << 8 | uchar(m_data[i]);
        return v;
    }
    double real(const Element &e) const
    {
        if (e.end - e.start == 4)
            return double(std::bit_cast<float>(qFromBigEndian<quint32>(m_data.data() + e.start)));
        if (e.end - e.start == 8)
            return std::bit_cast<double>(qFromBigEndian<quint64>(m_data.data() + e.start));
        return 0;
    }
    QString text(const Element &e) const
    {
        QByteArray bytes = m_data.sliced(e.start, e.end - e.start).toByteArray();
        const qsizetype nul = bytes.indexOf('\0');
        if (nul >= 0)
            bytes.truncate(nul);
        return QString::fromLatin1(bytes);
    }

private:
    quint64 vint(qint64 at, qint64 limit, bool keepMarker, int *length, bool *unknown = nullptr) const
    {
        *length = 0;
        if (at >= limit)
            return 0;
        const uchar first = uchar(m_data[at]);
        int n = 1;
        while (n <= 8 && !(first & (0x80 >> (n - 1))))
            ++n;
        if (n > 8 || at + n > limit)
            return 0;
        quint64 v = keepMarker ? first : first & (0xFF >> n);
        bool allOnes = (first & (0xFF >> n)) == (0xFF >> n);
        for (int i = 1; i < n; ++i) {
            v = v << 8 | uchar(m_data[at + i]);
            allOnes = allOnes && uchar(m_data[at + i]) == 0xFF;
        }
        if (unknown)
            *unknown = allOnes;
        *length = n;
        return v;
    }

    QByteArrayView m_data;
};

QString codecName(const QString &id)
{
    if (id == QLatin1String("V_AV1"))
        return QStringLiteral("AV1");
    if (id == QLatin1String("V_VP9"))
        return QStringLiteral("VP9");
    if (id == QLatin1String("V_VP8"))
        return QStringLiteral("VP8");
    if (id == QLatin1String("V_MPEG4/ISO/AVC"))
        return QStringLiteral("H.264");
    if (id == QLatin1String("V_MPEGH/ISO/HEVC"))
        return QStringLiteral("H.265");
    if (id == QLatin1String("A_OPUS"))
        return QStringLiteral("Opus");
    if (id == QLatin1String("A_VORBIS"))
        return QStringLiteral("Vorbis");
    if (id == QLatin1String("A_AAC"))
        return QStringLiteral("AAC");
    return id;
}

void readTrack(const Ebml &ebml, const Element &track, VideoInfo &info)
{
    int type = 0;
    QString codec;
    quint64 frameNs = 0;
    int width = 0, height = 0, channels = 0;
    double rate = 0;
    qint64 at = track.start;
    while (const auto e = ebml.next(at, track.end)) {
        if (e->id == kTrackType)
            type = int(ebml.uint(*e));
        else if (e->id == kCodecId)
            codec = ebml.text(*e);
        else if (e->id == kDefaultDuration)
            frameNs = ebml.uint(*e);
        else if (e->id == kVideo || e->id == kAudio) {
            qint64 inner = e->start;
            while (const auto f = ebml.next(inner, e->end)) {
                if (f->id == kPixelWidth)
                    width = int(ebml.uint(*f));
                else if (f->id == kPixelHeight)
                    height = int(ebml.uint(*f));
                else if (f->id == kSamplingFrequency)
                    rate = ebml.real(*f);
                else if (f->id == kChannels)
                    channels = int(ebml.uint(*f));
                inner = f->end;
            }
        }
        at = e->end;
    }
    if (type == 1 && info.videoCodec.isEmpty()) {
        info.videoCodec = codecName(codec);
        info.width = width;
        info.height = height;
        info.framesPerSecond = frameNs ? 1e9 / double(frameNs) : 0;
    } else if (type == 2 && info.audioCodec.isEmpty()) {
        info.audioCodec = codecName(codec);
        info.sampleRate = rate;
        info.channels = channels ? channels : 1;
    }
}

QString duration(double seconds)
{
    if (seconds < 0)
        return QObject::tr("unknown");
    const qint64 ms = qint64(std::llround(seconds * 1000));
    return QStringLiteral("%1:%2.%3").arg(ms / 60000).arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

class VideoTable : public SimpleTable
{
public:
    VideoTable(std::vector<Row> rows, QString path, QByteArray data)
        : SimpleTable(QObject::tr("Video"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_path(std::move(path)), m_data(std::move(data))
    {
    }

    bool hasVideo() const override { return true; }
    QString videoFile() const override { return m_path; }
    QByteArray videoData() const override { return m_data; }

private:
    QString m_path;
    QByteArray m_data;
};

bool isMp4(QByteArrayView head)
{
    return head.size() >= 12 && head.sliced(4, 4) == QByteArrayView("ftyp");
}

std::unique_ptr<Document> makeDocument(QByteArrayView head, qint64 size, const QString &fileName, QString path,
                                       QByteArray data, QString *error)
{
    std::vector<SimpleTable::Row> rows;
    QString summary;
    if (const std::optional<VideoInfo> info = parseMatroska(head)) {
        rows.push_back({QObject::tr("container"), info->container});
        rows.push_back({QObject::tr("length"), duration(info->seconds)});
        if (!info->videoCodec.isEmpty()) {
            rows.push_back({QObject::tr("video"), info->videoCodec});
            rows.push_back({QObject::tr("size"), QStringLiteral("%1 × %2").arg(info->width).arg(info->height)});
            if (info->framesPerSecond > 0)
                rows.push_back({QObject::tr("frame rate"), QObject::tr("%1 per second").arg(info->framesPerSecond, 0, 'g', 5)});
        }
        if (!info->audioCodec.isEmpty())
            rows.push_back({QObject::tr("audio"), QObject::tr("%1, %2 Hz, %n channel(s)", nullptr, info->channels)
                                                      .arg(info->audioCodec)
                                                      .arg(info->sampleRate, 0, 'g', 6)});
        summary = QStringLiteral("%1, %2 × %3, %4").arg(info->videoCodec).arg(info->width).arg(info->height).arg(duration(info->seconds));
    } else if (isMp4(head)) {
        rows.push_back({QObject::tr("container"), QStringLiteral("MP4")});
        summary = QStringLiteral("MP4");
    } else {
        if (error)
            *error = QObject::tr("Not a video this app can play.");
        return nullptr;
    }
    rows.push_back({QObject::tr("file size"), QLocale().formattedDataSize(size)});
    auto table = std::make_shared<VideoTable>(std::move(rows), std::move(path), std::move(data));
    table->setDescription(QObject::tr("%1: press Play below the picture").arg(summary.isEmpty() ? QFileInfo(fileName).fileName() : summary));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

}

std::optional<VideoInfo> parseMatroska(QByteArrayView head)
{
    const Ebml ebml(head);
    qint64 at = 0;
    const std::optional<Element> header = ebml.next(at, head.size());
    if (!header || header->id != kEbml)
        return std::nullopt;
    VideoInfo info;
    info.container = QStringLiteral("Matroska");
    qint64 inner = header->start;
    while (const auto e = ebml.next(inner, header->end)) {
        if (e->id == kDocType)
            info.container = ebml.text(*e) == QLatin1String("webm") ? QStringLiteral("WebM") : QStringLiteral("Matroska");
        inner = e->end;
    }
    at = header->end;
    const std::optional<Element> segment = ebml.next(at, head.size());
    if (!segment || segment->id != kSegment)
        return info;
    quint64 scale = 1000000;
    double units = -1;
    qint64 p = segment->start;
    while (const auto e = ebml.next(p, segment->end)) {
        if (e->id == kCluster)
            break;
        if (e->id == kInfo) {
            qint64 q = e->start;
            while (const auto f = ebml.next(q, e->end)) {
                if (f->id == kTimecodeScale)
                    scale = ebml.uint(*f);
                else if (f->id == kDuration)
                    units = ebml.real(*f);
                q = f->end;
            }
        } else if (e->id == kTracks) {
            qint64 q = e->start;
            while (const auto f = ebml.next(q, e->end)) {
                if (f->id == kTrackEntry)
                    readTrack(ebml, *f, info);
                q = f->end;
            }
        }
        p = e->end;
    }
    if (units >= 0)
        info.seconds = units * double(scale) / 1e9;
    return info;
}

int VideoFileReader::score(const QString &fileName, QByteArrayView head) const
{
    if (head.size() >= 4 && qFromBigEndian<quint32>(head.data()) == kEbml)
        return 95;
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (isMp4(head) && (suffix == QLatin1String("mp4") || suffix == QLatin1String("m4v") || suffix == QLatin1String("mov")))
        return 90;
    return 0;
}

std::unique_ptr<Document> VideoFileReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    const QByteArray head = file.read(kHeadBytes);
    return makeDocument(head, file.size(), path, QFileInfo(path).absoluteFilePath(), {}, error);
}

std::unique_ptr<Document> VideoFileReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return makeDocument(QByteArrayView(data).first(std::min<qsizetype>(data.size(), kHeadBytes)), data.size(), fileName, {},
                        data, error);
}
