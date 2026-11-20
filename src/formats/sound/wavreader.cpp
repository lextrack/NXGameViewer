#include "sound/wavreader.h"

#include "core/bytes.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QLocale>
#include <QObject>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {

constexpr int kMaxChannels = 32;
constexpr quint16 kPcm = 1;
constexpr quint16 kFloat = 3;
constexpr quint16 kExtensible = 0xFFFE;

qint16 sampleAt(const char *p, quint16 format, int bits)
{
    if (format == kFloat) {
        float v;
        std::memcpy(&v, p, 4);
        return qint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
    }
    switch (bits) {
    case 8: return qint16((int(uchar(p[0])) - 128) << 8);
    case 16: return qFromLittleEndian<qint16>(p);
    case 24: return qint16(qint16(uchar(p[1])) | qint16(qint16(qint8(p[2])) << 8));
    default: return qint16(qFromLittleEndian<qint32>(p) >> 16);
    }
}

class WavTable : public SimpleTable
{
public:
    WavTable(std::vector<Row> rows, Audio audio)
        : SimpleTable(QObject::tr("Sound"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_audio(std::move(audio))
    {
    }
    bool hasAudio() const override { return true; }
    std::optional<Audio> decodeAudio(QString *) const override { return m_audio; }

private:
    Audio m_audio;
};

std::unique_ptr<Document> parse(const QByteArray &data, QString *error)
{
    std::optional<Audio> audio = WavReader::decode(data, error);
    if (!audio)
        return nullptr;
    const double seconds = double(audio->frameCount()) / audio->sampleRate;
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("length"), QObject::tr("%1 s").arg(QLocale().toString(seconds, 'f', 2))},
        {QObject::tr("sample rate"), audio->sampleRate},
        {QObject::tr("channels"), audio->channels},
    };
    const int rate = audio->sampleRate, channels = audio->channels;
    auto table = std::make_shared<WavTable>(std::move(rows), std::move(*audio));
    table->setDescription(QObject::tr("WAV sound, %1 s, %2 Hz, %n channel(s)", nullptr, channels)
                              .arg(QLocale().toString(seconds, 'f', 2))
                              .arg(rate));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

}

std::optional<Audio> WavReader::decode(QByteArrayView data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Audio> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < 12 || !data.startsWith("RIFF") || data.sliced(8, 4) != "WAVE")
        return fail(QObject::tr("Not a WAV sound (no \"RIFF…WAVE\" at the start)."));
    quint16 format = 0, channels = 0, bits = 0;
    quint32 rate = 0;
    QByteArrayView samples;
    bool haveFormat = false;
    for (qsizetype at = 12; at + 8 <= data.size();) {
        const QByteArrayView id = data.sliced(at, 4);
        const qint64 size = le32(data, at + 4);
        const qsizetype body = at + 8;
        const qint64 available = std::min<qint64>(size, data.size() - body);
        if (id == "fmt " && available >= 16) {
            format = le16(data, body);
            channels = le16(data, body + 2);
            rate = le32(data, body + 4);
            bits = le16(data, body + 14);
            if (format == kExtensible && available >= 26)
                format = le16(data, body + 24);
            haveFormat = true;
        } else if (id == "data") {
            samples = data.sliced(body, qsizetype(available));
        }
        at = body + qsizetype(available) + (size & 1);
    }
    if (!haveFormat)
        return fail(QObject::tr("The WAV sound has no format chunk."));
    if (format != kPcm && format != kFloat)
        return fail(QObject::tr("This WAV sound is compressed (format %1); only plain PCM can be read here.").arg(format));
    if ((format == kPcm && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (format == kFloat && bits != 32))
        return fail(QObject::tr("This WAV sound uses %1-bit samples, which cannot be read.").arg(bits));
    if (channels < 1 || channels > kMaxChannels || rate == 0)
        return fail(QObject::tr("The WAV sound's format is damaged."));
    const int step = bits / 8;
    const qint64 count = samples.size() / step / channels * channels;
    Audio audio;
    audio.sampleRate = int(rate);
    audio.channels = channels;
    audio.pcm.resize(qsizetype(count * 2));
    auto *out = reinterpret_cast<char *>(audio.pcm.data());
    for (qint64 i = 0; i < count; ++i)
        qToLittleEndian<qint16>(sampleAt(samples.data() + i * step, format, bits), out + i * 2);
    return audio;
}

int WavReader::score(const QString &, QByteArrayView head) const
{
    return head.size() >= 12 && head.startsWith("RIFF") && head.sliced(8, 4) == "WAVE" ? 85 : 0;
}

std::unique_ptr<Document> WavReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return parse(*bytes, error);
}

std::unique_ptr<Document> WavReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    return parse(data, error);
}
