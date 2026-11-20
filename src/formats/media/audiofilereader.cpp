#include "media/audiofilereader.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

QString containerFromContent(QByteArrayView head)
{
    if (head.size() >= 12 && head.startsWith("RIFF") && head.sliced(8, 4) == "WAVE")
        return QStringLiteral("WAV");
    if (head.startsWith("OggS"))
        return QStringLiteral("Ogg");
    if (head.startsWith("fLaC"))
        return QStringLiteral("FLAC");
    if (head.startsWith("ID3"))
        return QStringLiteral("MP3");
    if (head.size() >= 12 && head.sliced(4, 4) == "ftyp" && head.sliced(8, 3) == "M4A")
        return QStringLiteral("M4A");
    if (head.size() >= 16 && head.startsWith(QByteArrayView("\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)))
        return QStringLiteral("WMA");
    return {};
}

QString containerFromSuffix(const QString &fileName)
{
    static const QStringList known = {"wav", "mp3", "ogg", "oga", "opus", "flac", "m4a", "aac", "wma", "aif", "aiff"};
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return known.contains(suffix) ? suffix.toUpper() : QString();
}

void appendAsInt16(const QAudioBuffer &buffer, QByteArray &pcm)
{
    const QAudioFormat format = buffer.format();
    const qsizetype samples = qsizetype(buffer.frameCount()) * format.channelCount();
    const char *data = buffer.constData<char>();
    const qsizetype start = pcm.size();
    pcm.resize(start + samples * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm.data() + start);
    switch (format.sampleFormat()) {
    case QAudioFormat::Int16:
        std::memcpy(out, data, size_t(samples) * 2);
        break;
    case QAudioFormat::UInt8:
        for (qsizetype i = 0; i < samples; ++i)
            out[i] = qint16((int(uchar(data[i])) - 128) << 8);
        break;
    case QAudioFormat::Int32:
        for (qsizetype i = 0; i < samples; ++i) {
            qint32 v;
            std::memcpy(&v, data + i * 4, 4);
            out[i] = qint16(v >> 16);
        }
        break;
    case QAudioFormat::Float:
        for (qsizetype i = 0; i < samples; ++i) {
            float v;
            std::memcpy(&v, data + i * 4, 4);
            out[i] = qint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
        }
        break;
    default:
        pcm.resize(start);
        break;
    }
}

QString duration(qint64 frames, int rate)
{
    if (rate <= 0)
        return {};
    const qint64 ms = frames * 1000 / rate;
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

class AudioFileTable : public SimpleTable
{
public:
    AudioFileTable(std::vector<Row> rows, Audio audio)
        : SimpleTable(QObject::tr("Sound"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_audio(std::move(audio)) {}

    bool hasAudio() const override { return true; }
    std::optional<Audio> decodeAudio(QString *) const override { return m_audio; }

private:
    Audio m_audio;
};

}

int AudioFileReader::score(const QString &fileName, QByteArrayView head) const
{
    if (!containerFromContent(head).isEmpty())
        return 90;
    return containerFromSuffix(fileName).isEmpty() ? 0 : 50;
}

std::optional<Audio> AudioFileReader::decodeFile(const QString &path, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::nullopt;
    };
    QAudioDecoder decoder;
    if (!decoder.isSupported())
        return fail(QObject::tr("No audio decoder is available (Qt Multimedia backend missing)."));

    Audio audio;
    QString problem;
    QEventLoop loop;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&] {
        const QAudioBuffer buffer = decoder.read();
        if (!buffer.isValid() || !problem.isEmpty())
            return;
        const QAudioFormat format = buffer.format();
        if (audio.channels == 0) {
            audio.channels = format.channelCount();
            audio.sampleRate = format.sampleRate();
        } else if (format.channelCount() != audio.channels || format.sampleRate() != audio.sampleRate) {
            problem = QObject::tr("The sound changes its format midway, which is not supported.");
        }
        if (problem.isEmpty())
            appendAsInt16(buffer, audio.pcm);
        if (problem.isEmpty() && audio.pcm.size() > kMaxPcmBytes)
            problem = QObject::tr("The sound is too long to load (more than 1 GB decoded).");
        if (!problem.isEmpty()) {
            decoder.stop();
            loop.quit();
        }
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop, [&] {
        if (problem.isEmpty())
            problem = decoder.errorString();
        loop.quit();
    });

    decoder.setSource(QUrl::fromLocalFile(path));
    QMetaObject::invokeMethod(&decoder, [&decoder] { decoder.start(); }, Qt::QueuedConnection);
    loop.exec();

    if (!problem.isEmpty())
        return fail(problem);
    if (audio.channels <= 0 || audio.sampleRate <= 0 || audio.pcm.isEmpty())
        return fail(QObject::tr("No sound could be decoded from this file."));
    return audio;
}

std::unique_ptr<Document> AudioFileReader::read(const QString &path, QString *error) const
{
    const QFileInfo info(path);
    if (info.size() > kMaxFileSize) {
        if (error)
            *error = fileTooLargeMessage(kMaxFileSize);
        return nullptr;
    }
    QString container;
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            container = containerFromContent(file.read(64));
    }
    if (container.isEmpty())
        container = containerFromSuffix(info.fileName());

    std::optional<Audio> audio = decodeFile(path, error);
    if (!audio)
        return nullptr;

    const QString length = duration(audio->frameCount(), audio->sampleRate);
    const int sampleRate = audio->sampleRate;
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("format"), container.isEmpty() ? QObject::tr("unknown") : container},
        {QObject::tr("length"), length},
        {QObject::tr("channels"), audio->channels},
        {QObject::tr("sample rate"), QObject::tr("%1 Hz").arg(audio->sampleRate)},
        {QObject::tr("samples"), audio->frameCount()},
        {QObject::tr("file size"), QLocale().formattedDataSize(info.size())},
    };
    auto table = std::make_shared<AudioFileTable>(std::move(rows), std::move(*audio));
    table->setDescription(QObject::tr("%1 %2, %3 Hz").arg(container, length).arg(sampleRate));

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(table);
    return doc;
}
