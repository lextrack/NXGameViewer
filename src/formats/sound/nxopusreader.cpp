#include "sound/nxopusreader.h"
#include "core/simpletable.h"

#include <QFile>
#include <QObject>
#include <QtEndian>

#include <opus.h>

#include <algorithm>
#include <memory>
#include <new>

namespace {

constexpr qsizetype kHeaderSize = 0x38;
constexpr int kMaxPacketSamples = 5760;

quint32 u32le(const QByteArray &d, qsizetype pos) { return qFromLittleEndian<quint32>(d.constData() + pos); }
quint32 u32be(const QByteArray &d, qsizetype pos) { return qFromBigEndian<quint32>(d.constData() + pos); }

struct Sound
{
    int channels = 0;
    int sampleRate = 0;
    qint64 sampleCount = 0;
    int preSkip = 0;
    std::vector<QByteArrayView> packets;
    QByteArray data;
};

QString duration(qint64 samples, int rate)
{
    if (rate <= 0)
        return {};
    const qint64 ms = samples * 1000 / rate;
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

class SoundTable : public SimpleTable
{
public:
    SoundTable(std::vector<Row> rows, std::shared_ptr<const Sound> sound)
        : SimpleTable(QObject::tr("Sound"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_sound(std::move(sound)) {}

    bool hasAudio() const override { return true; }

    std::optional<Audio> decodeAudio(QString *error) const override
    {
        const Sound &s = *m_sound;
        int opusError = OPUS_OK;
        std::unique_ptr<OpusDecoder, void (*)(OpusDecoder *)> decoder(
            opus_decoder_create(s.sampleRate, s.channels, &opusError), opus_decoder_destroy);
        if (!decoder) {
            if (error)
                *error = QObject::tr("Opus decoder: %1").arg(QString::fromUtf8(opus_strerror(opusError)));
            return std::nullopt;
        }

        try {
            return decode(decoder.get(), error);
        } catch (const std::bad_alloc &) {
            if (error)
                *error = QObject::tr("Not enough memory to decode this sound.");
            return std::nullopt;
        }
    }

private:
    std::optional<Audio> decode(OpusDecoder *decoder, QString *error) const
    {
        const Sound &s = *m_sound;
        Audio audio;
        audio.sampleRate = s.sampleRate;
        audio.channels = s.channels;
        const qint64 maxFrames = qint64(s.packets.size()) * kMaxPacketSamples;
        audio.pcm.reserve(std::min(s.sampleCount, maxFrames) * s.channels * 2);
        std::vector<opus_int16> buffer(size_t(kMaxPacketSamples) * size_t(s.channels));
        for (size_t i = 0; i < s.packets.size(); ++i) {
            const QByteArrayView packet = s.packets[i];
            const int samples = opus_decode(decoder, reinterpret_cast<const unsigned char *>(packet.data()),
                                            opus_int32(packet.size()), buffer.data(), kMaxPacketSamples, 0);
            if (samples < 0) {
                if (error)
                    *error = QObject::tr("Opus packet %1: %2").arg(i).arg(QString::fromUtf8(opus_strerror(samples)));
                return std::nullopt;
            }
            audio.pcm.append(reinterpret_cast<const char *>(buffer.data()), qsizetype(samples) * s.channels * 2);
        }
        if (s.preSkip > 0)
            audio.pcm.remove(0, std::min<qsizetype>(audio.pcm.size(), qsizetype(s.preSkip) * s.channels * 2));
        if (s.sampleCount > 0 && audio.frameCount() > s.sampleCount)
            audio.pcm.truncate(s.sampleCount * s.channels * 2);
        return audio;
    }

    std::shared_ptr<const Sound> m_sound;
};

}

int NxOpusReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("foxn") ? 100 : 0;
}

std::unique_ptr<Document> NxOpusReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 256 MB.");
        return nullptr;
    }
    return fromBytes(file.readAll(), error);
}

std::unique_ptr<Document> NxOpusReader::fromBytes(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < kHeaderSize || !data.startsWith("foxn"))
        return fail(QObject::tr("Not a Falcom nxopus sound (no \"foxn\" at the start)."));

    auto sound = std::make_shared<Sound>();
    sound->data = data;
    sound->channels = uchar(data[5]);
    sound->sampleRate = int(u32le(data, 0x08));
    const quint32 start = u32le(data, 0x18), size = u32le(data, 0x1C);
    sound->sampleCount = u32le(data, 0x20);
    const quint32 loopStart = u32le(data, 0x30), loopEnd = u32le(data, 0x34);

    if (sound->channels < 1 || sound->channels > 2)
        return fail(QObject::tr("%1 channels: only mono and stereo sounds are supported.").arg(sound->channels));
    if (sound->sampleRate != 8000 && sound->sampleRate != 12000 && sound->sampleRate != 16000
        && sound->sampleRate != 24000 && sound->sampleRate != 48000)
        return fail(QObject::tr("Unexpected sample rate %1.").arg(sound->sampleRate));
    if (start < kHeaderSize || qint64(start) + size > data.size())
        return fail(QObject::tr("The sound data does not fit in the file."));

    for (qint64 pos = start, end = qint64(start) + size; pos < end;) {
        if (pos + 8 > end)
            return fail(QObject::tr("Broken packet header at 0x%1.").arg(pos, 0, 16));
        const quint32 packetSize = u32be(sound->data, pos);
        if (packetSize == 0 || packetSize > end - pos - 8)
            return fail(QObject::tr("Broken packet at 0x%1.").arg(pos, 0, 16));
        sound->packets.push_back(QByteArrayView(sound->data).sliced(pos + 8, packetSize));
        pos += 8 + packetSize;
    }

    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("length"), duration(sound->sampleCount, sound->sampleRate)},
        {QObject::tr("channels"), sound->channels == 1 ? QObject::tr("1 (mono)") : QObject::tr("2 (stereo)")},
        {QObject::tr("sample rate"), QObject::tr("%1 Hz").arg(sound->sampleRate)},
        {QObject::tr("samples"), sound->sampleCount},
        {QObject::tr("loop"), loopEnd > 0 ? QObject::tr("%1 – %2 (%3 – %4)")
                                                  .arg(duration(loopStart, sound->sampleRate),
                                                       duration(loopEnd, sound->sampleRate))
                                                  .arg(loopStart)
                                                  .arg(loopEnd)
                                            : QObject::tr("none")},
        {QObject::tr("Opus packets"), qint64(sound->packets.size())},
        {QObject::tr("data size"), qint64(size)},
    };
    auto table = std::make_shared<SoundTable>(std::move(rows), sound);
    table->setDescription(QObject::tr("%1, %2 Hz").arg(duration(sound->sampleCount, sound->sampleRate)).arg(sound->sampleRate));

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(table);
    return doc;
}

int NintendoOpusReader::score(const QString &, QByteArrayView head) const
{
    return head.size() >= 4 && quint8(head[0]) == 0x01 && quint8(head[1]) == 0x00 && quint8(head[2]) == 0x00
                   && quint8(head[3]) == 0x80
             ? 100
             : 0;
}

std::unique_ptr<Document> NintendoOpusReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > NxOpusReader::kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 256 MB.");
        return nullptr;
    }
    return fromBytes(file.readAll(), error);
}

std::unique_ptr<Document> NintendoOpusReader::fromBytes(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < 0x20 || u32le(data, 0) != 0x80000001u)
        return fail(QObject::tr("Not a Nintendo Opus sound (no 0x80000001 header)."));
    auto sound = std::make_shared<Sound>();
    sound->data = data;
    sound->channels = uchar(data[9]);
    sound->sampleRate = int(u32le(data, 0x0C));
    const quint32 start = u32le(data, 0x10);
    sound->preSkip = qFromLittleEndian<quint16>(data.constData() + 0x1C);
    if (sound->channels < 1 || sound->channels > 2)
        return fail(QObject::tr("%1 channels: only mono and stereo sounds are supported.").arg(sound->channels));
    if (sound->sampleRate != 8000 && sound->sampleRate != 12000 && sound->sampleRate != 16000
        && sound->sampleRate != 24000 && sound->sampleRate != 48000)
        return fail(QObject::tr("Unexpected sample rate %1.").arg(sound->sampleRate));
    if (qint64(start) + 8 > data.size() || u32le(data, start) != 0x80000004u)
        return fail(QObject::tr("The sound data chunk is missing."));
    const qint64 size = std::min<qint64>(u32le(data, start + 4), data.size() - start - 8);
    qint64 samples = 0;
    for (qint64 pos = start + 8, end = start + 8 + size; pos < end;) {
        if (pos + 8 > end)
            return fail(QObject::tr("Broken packet header at 0x%1.").arg(pos, 0, 16));
        const quint32 packetSize = u32be(sound->data, pos);
        if (packetSize == 0 || packetSize > end - pos - 8)
            return fail(QObject::tr("Broken packet at 0x%1.").arg(pos, 0, 16));
        const QByteArrayView packet = QByteArrayView(sound->data).sliced(pos + 8, packetSize);
        const int n = opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(packet.data()),
                                                 opus_int32(packet.size()), sound->sampleRate);
        if (n > 0)
            samples += n;
        sound->packets.push_back(packet);
        pos += 8 + packetSize;
    }
    sound->sampleCount = std::max<qint64>(0, samples - sound->preSkip);

    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("length"), duration(sound->sampleCount, sound->sampleRate)},
        {QObject::tr("channels"), sound->channels == 1 ? QObject::tr("1 (mono)") : QObject::tr("2 (stereo)")},
        {QObject::tr("sample rate"), QObject::tr("%1 Hz").arg(sound->sampleRate)},
        {QObject::tr("samples"), sound->sampleCount},
        {QObject::tr("pre-skip"), sound->preSkip},
        {QObject::tr("Opus packets"), qint64(sound->packets.size())},
        {QObject::tr("data size"), size},
    };
    auto table = std::make_shared<SoundTable>(std::move(rows), sound);
    table->setDescription(QObject::tr("Nintendo Opus, %1, %2 Hz").arg(duration(sound->sampleCount, sound->sampleRate)).arg(sound->sampleRate));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(table);
    return doc;
}
