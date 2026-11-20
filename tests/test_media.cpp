#include "media/audiofilereader.h"
#include "media/fontfilereader.h"
#include "media/videofilereader.h"
#include "media/imagefilereader.h"
#include "media/mediaformats.h"
#include "core/registry.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <QtConcurrentRun>

#include <cmath>
#include <numbers>

namespace {

QByteArray be16(quint16 v)
{
    QByteArray b(2, '\0');
    qToBigEndian(v, b.data());
    return b;
}

QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    qToBigEndian(v, b.data());
    return b;
}

QByteArray sampleFont(bool cff)
{
    QByteArray strings;
    QByteArray records;
    const std::vector<std::pair<int, QString>> names{{1, "Testy"}, {2, "Bold"}, {4, "Testy Bold"}};
    for (const auto &[id, text] : names) {
        QByteArray utf16;
        for (QChar c : text)
            utf16 += be16(c.unicode());
        records += be16(3) + be16(1) + be16(0x409) + be16(quint16(id)) + be16(quint16(utf16.size())) + be16(quint16(strings.size()));
        strings += utf16;
    }
    const QByteArray name = be16(0) + be16(quint16(names.size())) + be16(quint16(6 + records.size())) + records + strings;
    const QByteArray maxp = be32(0x00005000) + be16(7);
    const QByteArray cffTable(8, '\1');
    std::vector<std::pair<QByteArray, QByteArray>> tables{{"maxp", maxp}, {"name", name}};
    if (cff)
        tables.insert(tables.begin(), {"CFF ", cffTable});
    QByteArray font = (cff ? QByteArray("OTTO") : QByteArray("\x00\x01\x00\x00", 4)) + be16(quint16(tables.size())) + be16(0)
                    + be16(0) + be16(0);
    quint32 offset = quint32(12 + tables.size() * 16);
    QByteArray body;
    for (const auto &[tag, data] : tables) {
        font += tag + be32(0) + be32(offset + quint32(body.size())) + be32(quint32(data.size()));
        body += data;
        while (body.size() % 4)
            body += '\0';
    }
    return font + body;
}

QByteArray nintendoFont(const QByteArray &font, const QByteArray &key)
{
    const QByteArray magic("\x7F\x9A\x02\x18", 4);
    QByteArray out = magic + be32(quint32(font.size())) + font;
    for (qsizetype i = 0; i < out.size(); ++i)
        out[i] = char(uchar(out[i]) ^ uchar(key[i % 4]));
    return out;
}

}

class TestMedia : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    FormatRegistry m_registry{mediaFormatReaders()};

    QString write(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        return path;
    }

    QString saveImage(const QString &name, const char *format)
    {
        QImage image(3, 2, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
        image.setPixelColor(2, 1, QColor(0, 0, 255, 128));
        const QString path = m_dir.filePath(name);
        return image.save(path, format) ? path : QString();
    }

    static QByteArray toneWav(int rate, int channels, int frames)
    {
        Audio audio;
        audio.sampleRate = rate;
        audio.channels = channels;
        audio.pcm.resize(qsizetype(frames) * channels * 2);
        auto *s = reinterpret_cast<qint16 *>(audio.pcm.data());
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < channels; ++c)
                s[i * channels + c] = qint16(8000 * std::sin(2 * std::numbers::pi * 440 * i / rate));
        return audio.toWav();
    }

    const FormatReader *bestReader(const QString &path) const
    {
        const auto candidates = m_registry.candidates(path);
        return candidates.empty() ? nullptr : candidates.front();
    }

private slots:
    void pngIsDetectedByContentAndShown()
    {
        const QString path = saveImage("picture.bin", "PNG");
        QVERIFY(!path.isEmpty());
        QCOMPARE(bestReader(path)->name(), ImageFileReader().name());

        const FormatReader *used = nullptr;
        QString error;
        const auto doc = m_registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        const auto image = doc->tables.at(0)->image();
        QVERIFY(image);
        QCOMPARE(image->width, 3);
        QCOMPARE(image->height, 2);
        const auto *px = reinterpret_cast<const uchar *>(image->rgba.constData());
        QCOMPARE(px[0], uchar(255));
        QCOMPARE(px[3], uchar(255));
        QCOMPARE(px[(1 * 3 + 2) * 4 + 2], uchar(255));
        QCOMPARE(px[(1 * 3 + 2) * 4 + 3], uchar(128));
        QCOMPARE(doc->tables.at(0)->text(0, 1, DisplayMode::Auto), QString("PNG"));
    }

    void jpegIsDetectedByContent()
    {
        const QString path = saveImage("photo.dat", "JPEG");
        if (path.isEmpty())
            QSKIP("No JPEG support in this Qt");
        QCOMPARE(bestReader(path)->name(), ImageFileReader().name());
    }

    void brokenImageFallsBackToHex()
    {
        const QString path = write("broken.png", QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, 'x'));
        const FormatReader *used = nullptr;
        QString error;
        QVERIFY(m_registry.open(path, &used, &error));
        QCOMPARE(used->name(), QString("Hex"));
    }

    void decodesNintendoFonts()
    {
        for (bool cff : {false, true}) {
            const QByteArray plain = sampleFont(cff);
            const QByteArray stored = nintendoFont(plain, QByteArray("\x12\x34\x56\x78", 4));
            const QString fileName = cff ? "Testy.Nin_NX_NVN.bfotf.zs" : "Testy.Nin_NX_NVN.bfttf.zs";
            const FormatReader *used = nullptr;
            QString error;
            const auto doc = m_registry.openBytes(stored, fileName, &used, &error);
            QVERIFY2(doc, qPrintable(error));
            QCOMPARE(used->name(), FontFileReader().name());
            const Table &info = *doc->tables.front();
            QCOMPARE(info.text(1, 1, DisplayMode::Auto), QString("Testy"));
            QCOMPARE(info.text(2, 1, DisplayMode::Auto), QString("Bold"));
            QCOMPARE(info.text(3, 1, DisplayMode::Auto), QString("Testy Bold"));
            QCOMPARE(info.text(7, 1, DisplayMode::Auto), QString("7"));
            QCOMPARE(info.text(6, 1, DisplayMode::Auto).contains("CFF"), cff);
            QCOMPARE(doc->tables.size(), size_t(2));
            const auto archive = doc->tables[1]->archive();
            QVERIFY(archive);
            QCOMPARE(archive->entries.front().name, cff ? QString("Testy.otf") : QString("Testy.ttf"));
            QCOMPARE(archive->entryData(0), plain);
            QVERIFY(doc->tables[1]->hasImage());

            const auto direct = m_registry.openBytes(plain, cff ? "Testy.otf" : "Testy.ttf", &used, &error);
            QVERIFY2(direct, qPrintable(error));
            QCOMPARE(used->name(), FontFileReader().name());
            QCOMPARE(direct->tables.size(), size_t(1));
        }
        QByteArray noise(64, '\x5A');
        QVERIFY(!FontFileReader().readBytes(noise, "x.bfttf", nullptr));
    }

    void readsVideoHeaders()
    {
        auto element = [](QByteArray id, const QByteArray &payload) { return id + char(0x80 | payload.size()) + payload; };
        auto bigEndian = [](quint64 v, int bytes) {
            QByteArray b;
            for (int i = bytes - 1; i >= 0; --i)
                b += char((v >> (i * 8)) & 0xFF);
            return b;
        };
        const QByteArray header = element(QByteArray("\x1A\x45\xDF\xA3", 4), element(QByteArray("\x42\x82", 2), "webm"));
        float seconds = 9517.f;
        quint32 bits;
        std::memcpy(&bits, &seconds, 4);
        const QByteArray info = element(QByteArray("\x15\x49\xA9\x66", 4),
                                        element(QByteArray("\x2A\xD7\xB1", 3), bigEndian(1000000, 3)) + element(QByteArray("\x44\x89", 2), bigEndian(bits, 4)));
        const QByteArray video = element(QByteArray("\xE0", 1), element(QByteArray("\xB0", 1), bigEndian(1920, 2)) + element(QByteArray("\xBA", 1), bigEndian(1080, 2)));
        const QByteArray track = element(QByteArray("\xAE", 1), element(QByteArray("\x83", 1), bigEndian(1, 1)) + element(QByteArray("\x86", 1), "V_AV1")
                                                                    + element(QByteArray("\x23\xE3\x83", 3), bigEndian(16666667, 4)) + video);
        const QByteArray tracks = element(QByteArray("\x16\x54\xAE\x6B", 4), track);
        const QByteArray file = header + QByteArray("\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 12) + info + tracks;
        const std::optional<VideoInfo> parsed = parseMatroska(file);
        QVERIFY(parsed);
        QCOMPARE(parsed->container, QString("WebM"));
        QCOMPARE(parsed->videoCodec, QString("AV1"));
        QCOMPARE(parsed->width, 1920);
        QCOMPARE(parsed->height, 1080);
        QVERIFY(std::abs(parsed->seconds - 9.517) < 1e-6);
        QVERIFY(std::abs(parsed->framesPerSecond - 60) < 0.01);
        const QString path = write("clip.webm", file);
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = m_registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), VideoFileReader().name());
        QVERIFY(doc->tables.front()->hasVideo());
        QCOMPARE(QFileInfo(doc->tables.front()->videoFile()).fileName(), QString("clip.webm"));
        QVERIFY(!parseMatroska(QByteArray("not a video")));
    }

    void gameFilesAreNotClaimed()
    {
        const QString path = write("data.bin", QByteArray("#TBL\x01\0\0\0", 8) + QByteArray(80, '\0'));
        QVERIFY(ImageFileReader().score("data.bin", QByteArray("#TBL")) == 0);
        QVERIFY(AudioFileReader().score("data.bin", QByteArray("#TBL")) == 0);
        QVERIFY(bestReader(path)->name() != ImageFileReader().name());
    }

    void wavIsDetectedAndDecoded()
    {
        if (!QAudioDecoderAvailable())
            QSKIP("No audio decoder in this Qt");
        const QString path = write("voice.dat", toneWav(22050, 2, 22050));
        QCOMPARE(bestReader(path)->name(), AudioFileReader().name());

        QString error;
        const auto doc = AudioFileReader().read(path, &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &table = *doc->tables.at(0);
        QVERIFY(table.hasAudio());
        const auto audio = table.decodeAudio(&error);
        QVERIFY(audio);
        QCOMPARE(audio->channels, 2);
        QCOMPARE(audio->sampleRate, 22050);
        QVERIFY2(std::abs(audio->frameCount() - 22050) < 2048, qPrintable(QString::number(audio->frameCount())));
        QCOMPARE(table.text(0, 1, DisplayMode::Auto), QString("WAV"));
    }

    void audioDecodesOnAWorkerThread()
    {
        if (!QAudioDecoderAvailable())
            QSKIP("No audio decoder in this Qt");
        const QString path = write("tone.wav", toneWav(48000, 1, 4800));
        QString error;
        const auto audio = QtConcurrent::run([path] { return AudioFileReader::decodeFile(path, nullptr); }).result();
        QVERIFY(audio);
        QCOMPARE(audio->channels, 1);
    }

    void notAudioIsAnError()
    {
        if (!QAudioDecoderAvailable())
            QSKIP("No audio decoder in this Qt");
        QString error;
        QVERIFY(!AudioFileReader().read(write("noise.mp3", QByteArray(256, 'z')), &error));
        QVERIFY(!error.isEmpty());
    }

private:
    static bool QAudioDecoderAvailable()
    {
        QString error;
        AudioFileReader::decodeFile(QStringLiteral("/nonexistent"), &error);
        return !error.contains(QLatin1String("Qt Multimedia backend"));
    }
};

QTEST_MAIN(TestMedia)
#include "test_media.moc"
