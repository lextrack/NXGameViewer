#include "sound/barsreader.h"
#include "sound/bwavreader.h"
#include "core/registry.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <opus.h>

#include <cmath>
#include <numbers>
#include <QtEndian>

namespace {

struct ChannelSpec
{
    int codec = 1;
    QByteArray data;
    quint32 samples = 0;
    std::array<qint16, 16> coefficients{};
    bool stub = false;
};

void put16(QByteArray &b, qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); }
void put32(QByteArray &b, qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); }

QByteArray makeBwav(const std::vector<ChannelSpec> &channels, int rate = 32000)
{
    const qsizetype header = 0x10 + qsizetype(channels.size()) * 0x4C;
    QByteArray b(header, '\0');
    b.replace(0, 4, "BWAV");
    b[4] = char(0xFF);
    b[5] = char(0xFE);
    put16(b, 6, 1);
    put16(b, 0x0E, quint16(channels.size()));
    for (size_t c = 0; c < channels.size(); ++c) {
        const qsizetype at = 0x10 + qsizetype(c) * 0x4C;
        const ChannelSpec &s = channels[c];
        while (b.size() % 0x40)
            b.append('\0');
        put16(b, at, quint16(s.codec));
        put32(b, at + 0x04, quint32(rate));
        put32(b, at + 0x08, s.samples);
        put32(b, at + 0x0C, s.stub ? 0 : s.samples);
        for (int k = 0; k < 16; ++k)
            put16(b, at + 0x10 + k * 2, quint16(s.coefficients[size_t(k)]));
        put32(b, at + 0x34, quint32(s.stub ? 0x10000 : b.size()));
        put32(b, at + 0x3C, 0xFFFFFFFF);
        put32(b, at + 0x40, 0xFFFFFFFF);
        if (!s.stub)
            b += s.data;
    }
    return b;
}

QByteArray nintendoOpus(double frequency, int packets)
{
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error);
    QByteArray data;
    std::vector<opus_int16> pcm(960);
    for (int p = 0; p < packets; ++p) {
        for (int i = 0; i < 960; ++i)
            pcm[size_t(i)] = opus_int16(9000 * std::sin(2 * std::numbers::pi * frequency * (p * 960 + i) / 48000));
        unsigned char packet[1500];
        const int size = opus_encode(encoder, pcm.data(), 960, packet, sizeof packet);
        QByteArray head(8, '\0');
        qToBigEndian(quint32(size), head.data());
        data += head + QByteArray(reinterpret_cast<const char *>(packet), size);
    }
    opus_encoder_destroy(encoder);
    QByteArray b(0x28, '\0');
    put32(b, 0, 0x80000001u);
    put32(b, 4, 0x18);
    b[9] = 1;
    put16(b, 0x0A, 960);
    put32(b, 0x0C, 48000);
    put32(b, 0x10, 0x20);
    put32(b, 0x20, 0x80000004u);
    put32(b, 0x24, quint32(data.size()));
    return b + data;
}

QByteArray makeBars(const std::vector<std::pair<QByteArray, QByteArray>> &sounds)
{
    const quint32 count = quint32(sounds.size());
    QByteArray b(16 + qsizetype(count) * 12, '\0');
    b.replace(0, 4, "BARS");
    b[8] = char(0xFF);
    b[9] = char(0xFE);
    put16(b, 10, 0x0101);
    put32(b, 12, count);
    for (quint32 i = 0; i < count; ++i) {
        put32(b, 16 + i * 4, 0x1000 + i);
        const qsizetype amta = b.size();
        QByteArray block(0x40, '\0');
        block.replace(0, 4, "AMTA");
        block[4] = char(0xFF);
        block[5] = char(0xFE);
        put16(block, 6, 0x0500);
        put32(block, 8, quint32(0x40 + sounds[i].first.size() + 1));
        put32(block, 0x24, 0x1C);
        block += sounds[i].first + '\0';
        while (block.size() % 0x20)
            block.append('\0');
        b += block;
        const qsizetype bwav = b.size();
        b += sounds[i].second;
        while (b.size() % 0x20)
            b.append('\0');
        put32(b, 16 + count * 4 + i * 8, quint32(amta));
        put32(b, 16 + count * 4 + i * 8 + 4, quint32(bwav));
    }
    put32(b, 4, quint32(b.size()));
    return b;
}

std::vector<qint16> samplesOf(const Audio &a)
{
    std::vector<qint16> out(size_t(a.pcm.size() / 2));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = qFromLittleEndian<qint16>(a.pcm.constData() + i * 2);
    return out;
}

ChannelSpec dspChannel()
{
    ChannelSpec c;
    c.samples = 20;
    c.coefficients[2] = 2048;
    c.data = QByteArray::fromHex("00123456789abcde" "10111111111111ff");
    return c;
}

}
class TestSound : public QObject
{
    Q_OBJECT

private slots:
    void findsSamplesInTheStreamFolder()
    {
        QByteArray pcm(200 * 2, '\0');
        for (int i = 0; i < 200; ++i)
            qToLittleEndian(qint16(i * 50 - 5000), pcm.data() + i * 2);
        ChannelSpec stub{0, {}, 200};
        stub.stub = true;
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("Stream"));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        const QString stubPath = dir.filePath("Line_000.bwav");
        QVERIFY(write(stubPath, makeBwav({stub}, 48000)));
        QString error;
        const auto alone = BwavReader().read(stubPath, &error);
        QVERIFY2(alone, qPrintable(error));
        QVERIFY(!alone->tables.front()->decodeAudio(&error));

        QVERIFY(write(dir.filePath("Stream/Line_000.bwav"), makeBwav({ChannelSpec{0, pcm, 200}}, 48000)));
        const auto doc = BwavReader().read(stubPath, &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables.front()->description().contains("Stream"));
        const auto audio = doc->tables.front()->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->frameCount(), qint64(200));
        QCOMPARE(audio->pcm, pcm);
    }

    void playsOpusBwavs()
    {
        ChannelSpec left{2, nintendoOpus(440, 25), 24000};
        ChannelSpec right{2, nintendoOpus(880, 25), 24000};
        QString error;
        const auto doc = BwavReader().readBytes(makeBwav({left, right}, 48000), "voice.bwav", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto audio = doc->tables.front()->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->channels, 2);
        QCOMPARE(audio->sampleRate, 48000);
        QCOMPARE(audio->frameCount(), qint64(24000));
        const auto *pcm = reinterpret_cast<const qint16 *>(audio->pcm.constData());
        double l = 0, r = 0;
        int crossL = 0, crossR = 0;
        for (qint64 f = 2000; f < 22000; ++f) {
            l += double(pcm[f * 2]) * pcm[f * 2];
            r += double(pcm[f * 2 + 1]) * pcm[f * 2 + 1];
            crossL += (pcm[f * 2] >= 0) != (pcm[f * 2 - 2] >= 0);
            crossR += (pcm[f * 2 + 1] >= 0) != (pcm[f * 2 - 1] >= 0);
        }
        QVERIFY(std::sqrt(l / 20000) > 3000 && std::sqrt(r / 20000) > 3000);
        QVERIFY2(std::abs(crossL - 2 * 440 * 20000 / 48000) < 20, qPrintable(QString::number(crossL)));
        QVERIFY2(std::abs(crossR - 2 * 880 * 20000 / 48000) < 20, qPrintable(QString::number(crossR)));

        QByteArray preview = makeBwav({ChannelSpec{2, QByteArray(0x400, '\0'), 512}}, 48000);
        put16(preview, 0x0C, 1);
        const auto stub = BwavReader().readBytes(preview, "preview.bwav", &error);
        QVERIFY2(stub, qPrintable(error));
        QVERIFY(!stub->tables.front()->decodeAudio(&error));
        QVERIFY2(error.contains("Stream folder"), qPrintable(error));
    }

    void decodesDspAdpcmFrames()
    {
        const ChannelSpec c = dspChannel();
        const std::vector<qint16> s = decodeDspAdpcm(c.data, c.samples, c.coefficients, 0, 0);
        QCOMPARE(s.size(), size_t(20));
        const std::vector<qint16> first = {1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2};
        QCOMPARE(std::vector<qint16>(s.begin(), s.begin() + 14), first);
        QCOMPARE(s[14], qint16(-1));
        QCOMPARE(s[15], qint16(0));
        QCOMPARE(s[19], qint16(4));
    }

    void appliesTheScaleAndClamps()
    {
        std::array<qint16, 16> coefficients{};
        const std::vector<qint16> s = decodeDspAdpcm(QByteArray::fromHex("0d78000000000000"), 2, coefficients, 0, 0);
        QCOMPARE(s, (std::vector<qint16>{32767, -32768}));
    }

    void playsAMonoDspSound()
    {
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        const auto doc = registry.openBytes(makeBwav({dspChannel()}), "hit.bwav", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), BwavReader().name());
        const Table &t = *doc->tables.front();
        QVERIFY(t.hasAudio());
        const auto audio = t.decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->channels, 1);
        QCOMPARE(audio->sampleRate, 32000);
        QCOMPARE(audio->frameCount(), qint64(20));
        QCOMPARE(samplesOf(*audio)[7], qint16(-8));
    }

    void interleavesStereoPcm()
    {
        ChannelSpec left, right;
        left.codec = right.codec = 0;
        left.samples = right.samples = 3;
        left.data = QByteArray::fromHex("010002000300");
        right.data = QByteArray::fromHex("ffff9cffe803");
        const auto doc = BwavReader().readBytes(makeBwav({left, right}), "music.bwav", nullptr);
        QVERIFY(doc);
        const auto audio = doc->tables.front()->decodeAudio(nullptr);
        QVERIFY(audio);
        QCOMPARE(audio->channels, 2);
        QCOMPARE(samplesOf(*audio), (std::vector<qint16>{1, -1, 2, -100, 3, 1000}));
    }

    void explainsSoundsStoredElsewhere()
    {
        ChannelSpec stub = dspChannel();
        stub.stub = true;
        QString error;
        const auto doc = BwavReader().readBytes(makeBwav({stub}), "stream.bwav", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables.front()->description().contains("another file"));
        QVERIFY(!doc->tables.front()->decodeAudio(&error));
        QVERIFY(error.contains("Stream"));
    }

    void listsTheSoundsOfAPack()
    {
        ChannelSpec stub = dspChannel();
        stub.stub = true;
        const QByteArray pack = makeBars({{"Jump", makeBwav({dspChannel()})}, {"Theme", makeBwav({stub})}});
        QString error;
        const auto doc = BarsReader().readBytes(pack, "Pack.bars", &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables.front();
        const auto archive = t.archive();
        QVERIFY(archive);
        QCOMPARE(archive->entries.size(), size_t(2));
        QCOMPARE(archive->entries[0].name, QString("Jump.bwav"));
        QCOMPARE(archive->entries[1].name, QString("Theme.bwav"));
        QVERIFY(t.text(1, 3, DisplayMode::Auto).contains("description only"));
        QVERIFY(t.description().contains("1 only describe"));

        FormatRegistry registry;
        const auto inner = registry.openBytes(archive->entryData(0), archive->entries[0].name, nullptr, &error);
        QVERIFY2(inner, qPrintable(error));
        const auto audio = inner->tables.front()->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->frameCount(), qint64(20));
    }

    void survivesDamagedFiles()
    {
        const QByteArray pack = makeBars({{"Jump", makeBwav({dspChannel()})}});
        for (qsizetype n = 0; n < pack.size(); ++n) {
            QString error;
            if (const auto doc = BarsReader().readBytes(pack.left(n), "cut.bars", &error))
                if (const auto archive = doc->tables.front()->archive())
                    for (size_t i = 0; i < archive->entries.size(); ++i)
                        if (const auto inner = BwavReader().readBytes(archive->entryData(i), "x.bwav", &error))
                            inner->tables.front()->decodeAudio(&error);
        }
        const QByteArray sound = makeBwav({dspChannel(), dspChannel()});
        for (qsizetype n = 0; n < sound.size(); ++n) {
            QString error;
            if (const auto doc = BwavReader().readBytes(sound.left(n), "cut.bwav", &error))
                doc->tables.front()->decodeAudio(&error);
        }
    }
};

QTEST_GUILESS_MAIN(TestSound)
#include "test_sound.moc"
