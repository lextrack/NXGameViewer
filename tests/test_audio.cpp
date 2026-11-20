#include "sound/nxopusreader.h"
#include "sound/wavreader.h"

#include <QTest>
#include <QtEndian>

#include <opus.h>

#include <cmath>
#include <vector>
#include <numbers>

// A hand-made .nxopus file (no game data): a 440 Hz tone, encoded with libopus into
// Nintendo-framed packets.

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = 960;  // 20 ms
constexpr int kPackets = 5;
constexpr int kSamples = kFrame * kPackets - 100;  // the last packet is padded

void putU32(QByteArray &b, qsizetype pos, quint32 v) { qToLittleEndian(v, b.data() + pos); }

QByteArray makeSound(int channels = 1)
{
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(kRate, channels, OPUS_APPLICATION_AUDIO, &error);
    Q_ASSERT(encoder);
    QByteArray packets;
    std::vector<opus_int16> pcm(size_t(kFrame) * channels);
    for (int p = 0; p < kPackets; ++p) {
        for (int i = 0; i < kFrame; ++i)
            for (int c = 0; c < channels; ++c)
                pcm[size_t(i * channels + c)] = opus_int16(8000 * std::sin(2 * std::numbers::pi * 440 * (p * kFrame + i) / kRate));
        unsigned char packet[1500];
        const int size = opus_encode(encoder, pcm.data(), kFrame, packet, sizeof packet);
        Q_ASSERT(size > 0);
        char header[8];
        qToBigEndian(quint32(size), header);
        qToBigEndian(quint32(0), header + 4);  // final range: not checked
        packets.append(header, 8);
        packets.append(reinterpret_cast<const char *>(packet), size);
    }
    opus_encoder_destroy(encoder);

    QByteArray b(0x40, '\0');
    b.replace(0, 4, "foxn");
    b[5] = char(channels);
    putU32(b, 0x08, kRate);
    putU32(b, 0x18, 0x40);
    putU32(b, 0x1C, quint32(packets.size()));
    putU32(b, 0x20, kSamples);
    putU32(b, 0x30, 100);
    putU32(b, 0x34, 2000);
    return b + packets;
}

double rms(const Audio &audio)
{
    const auto *s = reinterpret_cast<const qint16 *>(audio.pcm.constData());
    double sum = 0;
    const qsizetype n = audio.pcm.size() / 2;
    for (qsizetype i = 0; i < n; ++i)
        sum += double(s[i]) * s[i];
    return n ? std::sqrt(sum / n) : 0;
}

} // namespace

class TestAudio : public QObject
{
    Q_OBJECT

private slots:
    void readsHeader()
    {
        QString error;
        const auto doc = NxOpusReader::fromBytes(makeSound(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables.at(0);
        QVERIFY(t.hasAudio());
        // length, channels, sample rate, samples, loop, packets, data size
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("0:00.097"));
        QCOMPARE(t.text(1, 1, DisplayMode::Auto), QString("1 (mono)"));
        QCOMPARE(t.text(3, 1, DisplayMode::Auto), QString::number(kSamples));
        QVERIFY(t.text(4, 1, DisplayMode::Auto).contains("(100 – 2000)"));
        QCOMPARE(t.text(5, 1, DisplayMode::Auto), QString::number(kPackets));
    }

    void decodesTheTone()
    {
        for (int channels : {1, 2}) {
            QString error;
            const auto doc = NxOpusReader::fromBytes(makeSound(channels), &error);
            QVERIFY2(doc, qPrintable(error));
            const auto audio = doc->tables.at(0)->decodeAudio(&error);
            QVERIFY2(audio, qPrintable(error));
            QCOMPARE(audio->channels, channels);
            QCOMPARE(audio->sampleRate, kRate);
            QCOMPARE(audio->frameCount(), qint64(kSamples));  // cut to the header's length
            // A sine of amplitude 8000 has an RMS of about 5660; lossy coding changes it a little.
            QVERIFY2(rms(*audio) > 4000 && rms(*audio) < 7000, qPrintable(QString::number(rms(*audio))));
        }
    }

    void hugeSampleCountDoesNotReserveMemory()
    {
        QByteArray sound = makeSound(2);
        putU32(sound, 0x20, 0xFFFFFFFF);
        QString error;
        const auto doc = NxOpusReader::fromBytes(sound, &error);
        QVERIFY2(doc, qPrintable(error));
        const auto audio = doc->tables.at(0)->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->frameCount(), qint64(kPackets) * kFrame);
        QVERIFY(audio->pcm.capacity() <= qsizetype(kPackets) * 5760 * 2 * 2);
    }

    void writesWav()
    {
        Audio audio;
        audio.sampleRate = 48000;
        audio.channels = 2;
        audio.pcm = QByteArray(400, '\x01');
        const QByteArray wav = audio.toWav();
        QCOMPARE(wav.size(), qsizetype(44 + 400));
        QVERIFY(wav.startsWith("RIFF"));
        QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 22), quint16(2));         // channels
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 28), quint32(48000 * 4));  // bytes per second
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 40), quint32(400));
    }

    void readsPlainWavFromBytes()
    {
        Audio audio;
        audio.sampleRate = 22050;
        audio.channels = 2;
        for (qint16 v : {qint16(100), qint16(-100), qint16(32767), qint16(-32768)})
            audio.pcm.append(reinterpret_cast<const char *>(&v), 2);
        WavReader reader;
        QVERIFY(reader.readsBytes());
        QVERIFY(reader.score("x.bin", audio.toWav()) > 0);
        QString error;
        auto doc = reader.readBytes(audio.toWav(), "x.wav", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables.front()->hasAudio());
        const std::optional<Audio> back = doc->tables.front()->decodeAudio(&error);
        QVERIFY(back);
        QCOMPARE(back->sampleRate, 22050);
        QCOMPARE(back->channels, 2);
        QCOMPARE(back->pcm, audio.pcm);

        QByteArray eight = audio.toWav();
        eight.truncate(44);
        qToLittleEndian<quint16>(1, eight.data() + 22);
        qToLittleEndian<quint16>(1, eight.data() + 32);
        qToLittleEndian<quint16>(8, eight.data() + 34);
        qToLittleEndian<quint32>(2, eight.data() + 40);
        eight.append("\x80\xFF", 2);
        const std::optional<Audio> small = WavReader::decode(eight, &error);
        QVERIFY2(small, qPrintable(error));
        QCOMPARE(small->frameCount(), qint64(2));
        QCOMPARE(qFromLittleEndian<qint16>(small->pcm.constData()), qint16(0));
        QCOMPARE(qFromLittleEndian<qint16>(small->pcm.constData() + 2), qint16(127 << 8));

        QByteArray floats = audio.toWav();
        floats.truncate(44);
        qToLittleEndian<quint16>(3, floats.data() + 20);
        qToLittleEndian<quint16>(1, floats.data() + 22);
        qToLittleEndian<quint16>(32, floats.data() + 34);
        qToLittleEndian<quint32>(4, floats.data() + 40);
        const float half = 0.5f;
        floats.append(reinterpret_cast<const char *>(&half), 4);
        const std::optional<Audio> fromFloat = WavReader::decode(floats, &error);
        QVERIFY2(fromFloat, qPrintable(error));
        QCOMPARE(qFromLittleEndian<qint16>(fromFloat->pcm.constData()), qint16(16384));

        QByteArray adpcm = audio.toWav();
        qToLittleEndian<quint16>(2, adpcm.data() + 20);
        QVERIFY(!WavReader::decode(adpcm, &error));
        QVERIFY(!WavReader::decode(QByteArray("RIFF\0\0\0\0WAVE"), &error));
    }

    void rejectsBrokenFiles()
    {
        QString error;
        QByteArray tooManyChannels = makeSound();
        tooManyChannels[5] = 6;
        QVERIFY(!NxOpusReader::fromBytes(tooManyChannels, &error));
        QByteArray badRate = makeSound();
        putU32(badRate, 0x08, 44100);  // Opus does not decode at this rate
        QVERIFY(!NxOpusReader::fromBytes(badRate, &error));
        QByteArray badPacket = makeSound();
        badPacket[0x40] = char(0x7F);  // packet size far past the end
        QVERIFY(!NxOpusReader::fromBytes(badPacket, &error));
    }

    void truncatedSoundsNeverCrash()
    {
        const QByteArray full = makeSound();
        for (qsizetype n = 0; n < full.size(); n += 3) {
            QString error;
            if (const auto doc = NxOpusReader::fromBytes(full.left(n), &error))
                doc->tables.at(0)->decodeAudio(&error);
        }
    }
};

QTEST_MAIN(TestAudio)
#include "test_audio.moc"
