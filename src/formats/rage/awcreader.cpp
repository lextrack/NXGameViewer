#include "rage/awcreader.h"

#include "core/simpletable.h"
#include "sound/bwavreader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <vector>

namespace {

constexpr quint32 kMaxEntries = 100'000;
constexpr int kMaxChannels = 32;
constexpr qint64 kMaxSamples = qint64{48000} * 60 * 60;
constexpr int kFrameBytes = 8;
constexpr int kFrameSamples = 14;

enum Tag : quint8 { Data = 0x55, MusicHeader = 0x48, SfxHeader = 0xFA };

QString codecName(int codec)
{
    switch (codec) {
    case 0x00: case 0x01: return QStringLiteral("PCM");
    case 0x04: return QStringLiteral("IMA ADPCM");
    case 0x05: return QStringLiteral("XMA2");
    case 0x07: return QStringLiteral("MPEG");
    case 0x08: return QStringLiteral("Vorbis");
    case 0x0C: case 0x10: return QStringLiteral("DSP ADPCM");
    case 0x0D: return QStringLiteral("Opus");
    case 0x0F: return QStringLiteral("ATRAC9");
    case 0x11: return QStringLiteral("MS ADPCM");
    default: return QObject::tr("codec 0x%1").arg(codec, 2, 16, QLatin1Char('0'));
    }
}

bool isDsp(int codec)
{
    return codec == 0x0C || codec == 0x10;
}

bool isPcm(int codec)
{
    return codec == 0x00 || codec == 0x01;
}

struct Chunk
{
    qint64 offset = 0;
    qint64 size = 0;
};

struct Sound
{
    QString name;
    int sampleRate = 0;
    int channels = 1;
    qint64 samples = 0;
    int codec = -1;
    bool streamed = false;
    Chunk data;
    qint64 blockCount = 0;
    qint64 blockSize = 0;
};

class Source
{
public:
    Source(QByteArray data, bool big) : m_data(std::move(data)), m_big(big) {}

    const QByteArray &data() const { return m_data; }
    bool fits(qint64 offset, qint64 size) const { return offset >= 0 && size >= 0 && offset <= m_data.size() - size; }
    quint16 u16(qint64 at) const { return m_big ? qFromBigEndian<quint16>(m_data.constData() + at) : qFromLittleEndian<quint16>(m_data.constData() + at); }
    quint32 u32(qint64 at) const { return m_big ? qFromBigEndian<quint32>(m_data.constData() + at) : qFromLittleEndian<quint32>(m_data.constData() + at); }
    quint64 u64(qint64 at) const { return m_big ? qFromBigEndian<quint64>(m_data.constData() + at) : qFromLittleEndian<quint64>(m_data.constData() + at); }

private:
    QByteArray m_data;
    bool m_big;
};

struct DspState
{
    std::array<qint16, 16> coefficients{};
    qint16 history1 = 0;
    qint16 history2 = 0;
};

DspState dspHeader(const QByteArray &data, qint64 at)
{
    DspState s;
    for (int i = 0; i < 16; ++i)
        s.coefficients[size_t(i)] = qFromLittleEndian<qint16>(data.constData() + at + 0x1C + 2 * i);
    s.history1 = qFromLittleEndian<qint16>(data.constData() + at + 0x40);
    s.history2 = qFromLittleEndian<qint16>(data.constData() + at + 0x42);
    return s;
}

void decodeDspRun(const QByteArray &data, qint64 at, qint64 frames, qint64 keep, DspState &state, std::vector<qint16> &out)
{
    const std::vector<qint16> decoded = decodeDspAdpcm(QByteArrayView(data).sliced(at, frames * kFrameBytes),
                                                       frames * kFrameSamples, state.coefficients, state.history1,
                                                       state.history2);
    if (decoded.size() >= 2) {
        state.history1 = decoded[decoded.size() - 1];
        state.history2 = decoded[decoded.size() - 2];
    } else if (decoded.size() == 1) {
        state.history2 = state.history1;
        state.history1 = decoded.back();
    }
    out.insert(out.end(), decoded.begin(), decoded.begin() + qsizetype(std::min<qint64>(keep, qint64(decoded.size()))));
}

std::optional<Audio> decodeSound(const Source &src, const Sound &s, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Audio> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!isDsp(s.codec) && !(isPcm(s.codec) && !s.streamed))
        return fail(QObject::tr("Sounds in %1 cannot be played yet.").arg(codecName(s.codec)));
    if (s.sampleRate <= 0 || s.samples <= 0 || s.samples > kMaxSamples || s.channels < 1 || s.channels > kMaxChannels)
        return fail(QObject::tr("The sound's header is damaged."));
    const QByteArray &d = src.data();
    std::vector<std::vector<qint16>> channels(size_t(s.channels));
    if (!s.streamed && isPcm(s.codec)) {
        const qint64 count = std::min(s.samples, s.data.size / 2);
        channels[0].reserve(size_t(count));
        for (qint64 i = 0; i < count; ++i)
            channels[0].push_back(qint16(src.u16(s.data.offset + 2 * i)));
    } else if (!s.streamed) {
        if (s.data.size < 0x60)
            return fail(QObject::tr("The sound data is cut off."));
        DspState state = dspHeader(d, s.data.offset);
        const qint64 frames = std::min((s.samples + kFrameSamples - 1) / kFrameSamples, (s.data.size - 0x60) / kFrameBytes);
        decodeDspRun(d, s.data.offset + 0x60, frames, s.samples, state, channels[0]);
    } else {
        std::vector<DspState> states(size_t(s.channels));
        bool first = true;
        const qint64 headerSize = qint64(0x10 + 0x70) * s.channels;
        for (qint64 b = 0; b < s.blockCount; ++b) {
            const qint64 block = s.data.offset + b * s.blockSize;
            const qint64 blockEnd = std::min(block + s.blockSize, s.data.offset + s.data.size);
            if (block + headerSize > blockEnd)
                break;
            qint64 at = block + headerSize;
            for (int ch = 0; ch < s.channels; ++ch) {
                const qint64 entries = src.u32(block + 0x10 * ch + 0x04);
                const qint64 samples = src.u32(block + 0x10 * ch + 0x0C);
                if (entries < 0 || entries > (blockEnd - at) / kFrameBytes)
                    return fail(QObject::tr("A block of the music is damaged."));
                if (first)
                    states[size_t(ch)] = dspHeader(d, block + 0x10 * s.channels + 0x70 * ch + 0x10);
                decodeDspRun(d, at, entries, samples, states[size_t(ch)], channels[size_t(ch)]);
                at += entries * kFrameBytes;
            }
            first = false;
        }
    }
    qint64 frames = 0;
    for (const auto &c : channels)
        frames = std::max<qint64>(frames, qint64(c.size()));
    frames = std::min(frames, s.samples);
    Audio audio;
    audio.sampleRate = s.sampleRate;
    audio.channels = s.channels;
    audio.pcm.resize(qsizetype(frames * s.channels * 2));
    auto *out = reinterpret_cast<char *>(audio.pcm.data());
    for (qint64 i = 0; i < frames; ++i)
        for (int ch = 0; ch < s.channels; ++ch) {
            const auto &c = channels[size_t(ch)];
            qToLittleEndian<qint16>(i < qint64(c.size()) ? c[size_t(i)] : 0, out + (i * s.channels + ch) * 2);
        }
    return audio;
}

class SoundTable : public SimpleTable
{
public:
    SoundTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive, std::shared_ptr<const Source> source,
               std::shared_ptr<const std::vector<Sound>> sounds)
        : SimpleTable(QObject::tr("Sounds"),
                      {{QObject::tr("name")}, {QObject::tr("length")}, {QObject::tr("sample rate")},
                       {QObject::tr("channels")}, {QObject::tr("codec")}},
                      std::move(rows)),
          m_archive(std::move(archive)), m_source(std::move(source)), m_sounds(std::move(sounds))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }
    bool hasAudio() const override { return m_sounds->size() == 1; }
    std::optional<Audio> decodeAudio(QString *error) const override
    {
        if (m_sounds->size() != 1)
            return std::nullopt;
        return decodeSound(*m_source, m_sounds->front(), error);
    }
    bool hasRowAudio() const override { return m_sounds->size() > 1; }
    std::optional<Audio> decodeRowAudio(int row, QString *error) const override
    {
        if (row < 0 || size_t(row) >= m_sounds->size())
            return std::nullopt;
        return decodeSound(*m_source, (*m_sounds)[size_t(row)], error);
    }

private:
    std::shared_ptr<const Archive> m_archive;
    std::shared_ptr<const Source> m_source;
    std::shared_ptr<const std::vector<Sound>> m_sounds;
};

std::unique_ptr<Document> parse(const QByteArray &data, const QString &fileName, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < 0x10 || !(data.startsWith("ADAT") || data.startsWith("TADA")))
        return fail(QObject::tr("Not a RAGE audio container (no \"ADAT\" at the start)."));
    auto source = std::make_shared<const Source>(data, data.startsWith("TADA"));
    const Source &src = *source;
    const quint32 flags = src.u32(4), entries = src.u32(8);
    if ((flags & 0xFF00FFFFu) != 0xFF000001u || (flags & 0x00F00000u) || entries == 0 || entries > kMaxEntries)
        return fail(QObject::tr("This audio container uses a layout the app does not know (flags 0x%1).")
                        .arg(flags, 8, 16, QLatin1Char('0')));
    if (flags & 0x00080000u)
        return fail(QObject::tr("This audio container is encrypted."));
    qint64 at = 0x10 + ((flags & 0x00010000u) ? 2 * qint64(entries) : 0);
    if (!src.fits(at, 4 * qint64(entries)))
        return fail(QObject::tr("The audio container is cut off."));
    std::vector<quint32> infos(entries);
    for (quint32 i = 0; i < entries; ++i)
        infos[i] = src.u32(at + 4 * i);
    at += 4 * qint64(entries);

    std::vector<QHash<quint8, Chunk>> tags(entries);
    for (quint32 i = 0; i < entries; ++i) {
        const int count = int((infos[i] >> 29) & 7);
        if (!src.fits(at, 8 * count))
            return fail(QObject::tr("The audio container is cut off."));
        for (int t = 0; t < count; ++t) {
            const quint64 tag = src.u64(at + 8 * t);
            const Chunk c{qint64(tag & 0x0FFFFFFF), qint64((tag >> 28) & 0x0FFFFFFF)};
            if (src.fits(c.offset, c.size))
                tags[i].insert(quint8(tag >> 56), c);
        }
        at += 8 * count;
    }

    std::vector<Sound> sounds;
    int problems = 0;
    if ((infos[0] & 0x1FFFFFFF) == 0) {
        const QHash<quint8, Chunk> &t = tags[0];
        if (!t.contains(MusicHeader) || !t.contains(Data))
            return fail(QObject::tr("The music in this audio container has no header."));
        const Chunk h = t.value(MusicHeader);
        if (h.size < 0x0C)
            return fail(QObject::tr("The music header is cut off."));
        Sound s;
        s.streamed = true;
        s.blockCount = src.u32(h.offset);
        s.blockSize = src.u32(h.offset + 4);
        s.channels = int(src.u32(h.offset + 8));
        if (s.channels < 1 || s.channels > kMaxChannels || h.size < 0x0C + 0x10 * qint64(s.channels) || s.blockSize <= 0)
            return fail(QObject::tr("The music header is damaged."));
        for (int ch = 0; ch < s.channels; ++ch) {
            const qint64 c = h.offset + 0x0C + 0x10 * ch;
            s.samples = std::max<qint64>(s.samples, src.u32(c + 4));
            s.sampleRate = src.u16(c + 0x0A);
            s.codec = uchar(data[qsizetype(c + 0x0C)]);
        }
        s.data = t.value(Data);
        s.name = (QFileInfo(fileName).completeBaseName().isEmpty() ? QStringLiteral("music")
                                                                   : QFileInfo(fileName).completeBaseName()) + QStringLiteral(".wav");
        sounds.push_back(s);
    } else {
        for (quint32 i = 0; i < entries; ++i) {
            const QHash<quint8, Chunk> &t = tags[i];
            if (!t.contains(SfxHeader) || !t.contains(Data)) {
                if (!t.isEmpty())
                    ++problems;
                continue;
            }
            const Chunk h = t.value(SfxHeader);
            if (h.size < 0x14) {
                ++problems;
                continue;
            }
            Sound s;
            s.samples = src.u32(h.offset);
            s.sampleRate = src.u16(h.offset + 8);
            s.codec = uchar(data[qsizetype(h.offset + 0x13)]);
            s.data = t.value(Data);
            s.name = QStringLiteral("%1.wav").arg(infos[i] & 0x1FFFFFFF, 8, 16, QLatin1Char('0'));
            sounds.push_back(s);
        }
    }

    auto shared = std::make_shared<const std::vector<Sound>>(std::move(sounds));
    auto archive = std::make_shared<Archive>();
    std::vector<SimpleTable::Row> rows;
    int unsupported = 0;
    for (const Sound &s : *shared) {
        const bool playable = isDsp(s.codec) || (isPcm(s.codec) && !s.streamed);
        unsupported += playable ? 0 : 1;
        const double seconds = s.sampleRate > 0 ? double(s.samples) / s.sampleRate : 0;
        rows.push_back({s.name, QObject::tr("%1 s").arg(QLocale().toString(seconds, 'f', 2)), s.sampleRate, s.channels,
                        codecName(s.codec)});
        archive->entries.push_back({s.name, 0, 44 + s.samples * s.channels * 2});
    }
    archive->loader = [source, shared](size_t index) {
        if (index >= shared->size())
            return QByteArray();
        const std::optional<Audio> audio = decodeSound(*source, (*shared)[index], nullptr);
        return audio ? audio->toWav() : QByteArray();
    };
    auto table = std::make_shared<SoundTable>(std::move(rows), archive, source, shared);
    QString description = shared->size() == 1 && shared->front().streamed
                              ? QObject::tr("RAGE audio container, music with %n channel(s)", nullptr, shared->front().channels)
                              : QObject::tr("RAGE audio container, %n sound(s)", nullptr, int(shared->size()));
    if (unsupported)
        description += QObject::tr("; %n in codecs that cannot be played yet", nullptr, unsupported);
    if (problems)
        description += QObject::tr("; %n damaged entry(s) skipped", nullptr, problems);
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

}

int AwcReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("ADAT") || head.startsWith("TADA") ? 100 : 0;
}

std::unique_ptr<Document> AwcReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 1 GB.");
        return nullptr;
    }
    return parse(file.readAll(), path, error);
}

std::unique_ptr<Document> AwcReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return parse(data, fileName, error);
}
