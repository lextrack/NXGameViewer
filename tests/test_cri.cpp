#include "core/contentkind.h"
#include "core/registry.h"
#include "cri/criutf.h"
#include "cri/usmreader.h"

#include <QTest>
#include <QtEndian>

#include <opus.h>

#include <cmath>
#include <numbers>

namespace {

class Bytes
{
public:
    QByteArray data;

    template <typename T> Bytes &le(T v)
    {
        char raw[sizeof(T)];
        qToLittleEndian(v, raw);
        data.append(raw, sizeof(T));
        return *this;
    }
    template <typename T> Bytes &be(T v)
    {
        char raw[sizeof(T)];
        qToBigEndian(v, raw);
        data.append(raw, sizeof(T));
        return *this;
    }
    Bytes &raw(const QByteArray &s)
    {
        data += s;
        return *this;
    }
    Bytes &lps(const QByteArray &s)
    {
        data += char(s.size());
        data += s;
        return *this;
    }
};

struct UtfColumn
{
    QByteArray name;
    quint8 type;
    bool perRow;
    std::vector<QVariant> values;
};

QByteArray utf(const QByteArray &tableName, const std::vector<UtfColumn> &columns)
{
    QByteArray strings("<NULL>\0", 7);
    QByteArray blobs;
    auto str = [&](const QByteArray &s) {
        const quint32 at = quint32(strings.size());
        strings += s + '\0';
        return at;
    };
    auto value = [&](Bytes &out, quint8 type, const QVariant &v) {
        switch (type) {
        case 0x00: out.be<quint8>(quint8(v.toInt())); break;
        case 0x02: out.be<quint16>(quint16(v.toInt())); break;
        case 0x04: out.be<quint32>(quint32(v.toLongLong())); break;
        case 0x0A: out.be<quint32>(str(v.toByteArray())); break;
        case 0x0B: {
            const QByteArray b = v.toByteArray();
            out.be<quint32>(quint32(blobs.size())).be<quint32>(quint32(b.size()));
            blobs += b;
            break;
        }
        default: QTest::qFail("unsupported test column type", __FILE__, __LINE__);
        }
    };
    const quint32 nameAt = str(tableName);
    Bytes schema;
    int rowWidth = 0;
    for (const UtfColumn &c : columns) {
        schema.be<quint8>(quint8(0x10 | (c.perRow ? 0x40 : 0x20) | c.type)).be<quint32>(str(c.name));
        if (!c.perRow)
            value(schema, c.type, c.values.front());
        else
            rowWidth += c.type == 0x00 ? 1 : c.type == 0x02 ? 2 : c.type == 0x0B ? 8 : 4;
    }
    const size_t rowCount = columns.empty() ? 0 : std::max_element(columns.begin(), columns.end(), [](auto &a, auto &b) {
                                                       return a.values.size() < b.values.size();
                                                   })->values.size();
    Bytes rows;
    for (size_t r = 0; r < rowCount; ++r)
        for (const UtfColumn &c : columns)
            if (c.perRow)
                value(rows, c.type, c.values.at(r));
    const quint32 rowsAt = 0x18 + quint32(schema.data.size());
    const quint32 stringsAt = rowsAt + quint32(rows.data.size());
    const quint32 dataAt = stringsAt + quint32(strings.size());
    Bytes body;
    body.be<quint16>(1).be<quint16>(quint16(rowsAt)).be<quint32>(stringsAt).be<quint32>(dataAt).be<quint32>(nameAt);
    body.be<quint16>(quint16(columns.size())).be<quint16>(quint16(rowWidth)).be<quint32>(quint32(rowCount));
    body.raw(schema.data).raw(rows.data).raw(strings).raw(blobs);
    return Bytes().raw("@UTF").be<quint32>(quint32(body.data.size())).raw(body.data).data;
}

QByteArray afs2(const std::vector<QByteArray> &files)
{
    const int alignment = 32;
    Bytes header;
    header.raw("AFS2").le<quint8>(1).le<quint8>(4).le<quint16>(2).le<quint32>(quint32(files.size()));
    header.le<quint16>(quint16(alignment)).le<quint16>(0);
    for (size_t i = 0; i < files.size(); ++i)
        header.le<quint16>(quint16(i));
    const qsizetype tableEnd = header.data.size() + qsizetype(files.size() + 1) * 4;
    QByteArray body;
    std::vector<quint32> offsets;
    qsizetype at = tableEnd;
    for (const QByteArray &f : files) {
        offsets.push_back(quint32(at));
        const qsizetype start = (at + alignment - 1) / alignment * alignment;
        body += QByteArray(start - at, '\0') + f;
        at = start + f.size();
    }
    offsets.push_back(quint32(at));
    for (quint32 o : offsets)
        header.le<quint32>(o);
    return header.data + body;
}

QByteArray usmChunk(const char *signature, int type, const QByteArray &payload, int channel = 0, int padding = 0)
{
    QByteArray out(signature, 4);
    Bytes b;
    b.be<quint32>(quint32(0x18 + payload.size() + padding));
    out += b.data;
    QByteArray head(0x18, '\0');
    head[1] = 0x18;
    qToBigEndian<quint16>(quint16(padding), head.data() + 2);
    head[4] = char(channel);
    head[7] = char(type);
    return out + head + payload + QByteArray(padding, '\0');
}

QByteArray nintendoOpus(int frames, int preSkip)
{
    constexpr int rate = 48000, frame = 960;
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_AUDIO, &error);
    Bytes packets;
    std::vector<opus_int16> pcm(frame);
    for (int p = 0; p < frames; ++p) {
        for (int i = 0; i < frame; ++i)
            pcm[size_t(i)] = opus_int16(6000 * std::sin(2 * std::numbers::pi * 330 * (p * frame + i) / rate));
        unsigned char packet[1500];
        const int size = opus_encode(encoder, pcm.data(), frame, packet, sizeof packet);
        packets.be<quint32>(quint32(size)).be<quint32>(0).raw(QByteArray(reinterpret_cast<const char *>(packet), size));
    }
    opus_encoder_destroy(encoder);
    Bytes file;
    file.le<quint32>(0x80000001).le<quint32>(0x18).le<quint8>(0).le<quint8>(1).le<quint16>(0).le<quint32>(rate);
    file.le<quint32>(0x20).le<quint32>(0).le<quint32>(0).le<quint16>(quint16(preSkip)).le<quint16>(0);
    file.le<quint32>(0x80000004).le<quint32>(quint32(packets.data.size())).raw(packets.data);
    return file.data;
}

}

class TestCri : public QObject
{
    Q_OBJECT

    FormatRegistry m_registry;

    std::shared_ptr<Document> open(const QByteArray &data, const QString &name, QString *reader = nullptr)
    {
        QString error;
        const FormatReader *used = nullptr;
        auto doc = m_registry.openBytes(data, name, &used, &error);
        if (reader)
            *reader = used ? used->name() : QString();
        if (!doc)
            qWarning() << error;
        return doc;
    }

private slots:
    void readsBinaryFormatterRecords()
    {
        Bytes b;
        b.le<quint8>(0).le<qint32>(1).le<qint32>(-1).le<qint32>(1).le<qint32>(0);
        b.le<quint8>(12).le<qint32>(2).lps("Game, Version=1.0.0.0");
        b.le<quint8>(7).le<qint32>(1).le<quint8>(0).le<qint32>(1).le<qint32>(2).le<quint8>(4).lps("Row").le<qint32>(2);
        b.le<quint8>(9).le<qint32>(3).le<quint8>(9).le<qint32>(4);
        b.le<quint8>(5).le<qint32>(3).lps("Row").le<qint32>(3).lps("<Id>k__BackingField").lps("Name").lps("Rate");
        b.le<quint8>(0).le<quint8>(1).le<quint8>(0).le<quint8>(8).le<quint8>(11).le<qint32>(2);
        b.le<qint32>(7).le<quint8>(6).le<qint32>(5).lps("seven");
        b.le<quint32>(0x3FC00000);
        b.le<quint8>(1).le<qint32>(4).le<qint32>(3);
        b.le<qint32>(8).le<quint8>(6).le<qint32>(6).lps("eight").le<quint32>(0x40000000);
        b.le<quint8>(11);

        QCOMPARE(describeContent(QByteArrayView(b.data).first(16)), QString(".NET BinaryFormatter data"));
        QString reader;
        auto doc = open(b.data, "skills.tbl", &reader);
        QVERIFY(doc);
        QCOMPARE(reader, QString(".NET BinaryFormatter data"));
        const auto &rows = doc->tables.front();
        QCOMPARE(rows->name(), QString("Row"));
        QCOMPARE(rows->columnCount(), 3);
        QCOMPARE(rows->columnName(0), QString("Id"));
        QCOMPARE(rows->rowCount(), 2);
        QCOMPARE(rows->text(0, 0, DisplayMode::Auto), QString("7"));
        QCOMPARE(rows->text(1, 1, DisplayMode::Auto), QString("eight"));
        QCOMPARE(rows->sortKey(1, 2).toDouble(), 2.0);
        const QByteArray json = rows->exportJson();
        QVERIFY2(json.contains("\"Name\": \"seven\""), json.constData());

        QByteArray cut = b.data.left(b.data.size() - 12);
        QString error;
        QVERIFY(!m_registry.byName(".NET BinaryFormatter data")->readBytes(cut, "cut.tbl", &error));
    }

    void readsUtfTables()
    {
        const QByteArray table = utf("Demo", {{"Name", 0x0A, true, {"one", "two"}},
                                              {"Id", 0x02, true, {10, 20}},
                                              {"Version", 0x00, false, {5}},
                                              {"Blob", 0x0B, true, {QByteArray("AFS2xxxx"), QByteArray()}}});
        const std::optional<CriUtfTable> parsed = readCriUtf(table, nullptr);
        QVERIFY(parsed);
        QCOMPARE(parsed->name, QString("Demo"));
        QCOMPARE(parsed->value(1, "Name").toString(), QString("two"));
        QCOMPARE(parsed->value(1, "Id").toInt(), 20);
        QCOMPARE(parsed->value(0, "Version").toInt(), 5);
        QCOMPARE(parsed->value(0, "Blob").toByteArray(), QByteArray("AFS2xxxx"));

        auto doc = open(table, "demo.acf");
        QVERIFY(doc);
        QCOMPARE(doc->tables.front()->text(0, 0, DisplayMode::Auto), QString("one"));
        auto archive = doc->tables.back()->archive();
        QVERIFY(archive);
        QCOMPARE(archive->entries.size(), size_t(1));
        QCOMPARE(archive->entries.front().name, QString("Blob[0].awb"));
        QVERIFY(!readCriUtf(table.left(40), nullptr));
    }

    void splitsUsmMoviesIntoStreams()
    {
        const QByteArray ivfHead = QByteArray("DKIF") + QByteArray(28, '\x01');
        const QByteArray frame1 = QByteArray(12, '\x02') + "first", frame2 = QByteArray(12, '\x03') + "second";
        const QByteArray adx = QByteArray("\x80\x00", 2) + QByteArray(30, '\x04');
        QByteArray file = usmChunk("CRID", 1, utf("CRIUSF_DIR_STREAM", {{"filename", 0x0A, true, {"demo.usm"}}}));
        file += usmChunk("@SFV", 1, utf("VIDEO_HDRINFO", {{"width", 0x04, true, {1280}}, {"height", 0x04, true, {720}},
                                                          {"total_frames", 0x04, true, {2}},
                                                          {"framerate_n", 0x04, true, {60000}},
                                                          {"framerate_d", 0x04, true, {1000}}}));
        file += usmChunk("@SFA", 1, utf("AUDIO_HDRINFO", {{"sampling_rate", 0x04, true, {48000}},
                                                          {"num_channels", 0x00, true, {2}}}));
        file += usmChunk("@SFA", 0, adx.left(10));
        file += usmChunk("@SFV", 0, ivfHead + frame1, 0, 7);
        file += usmChunk("@SFV", 2, "#CONTENTS END   ===============");
        file += usmChunk("@SFA", 0, adx.mid(10));
        file += usmChunk("@SFV", 0, frame2);

        auto doc = open(file, "demo.usm");
        QVERIFY(doc);
        const auto &table = doc->tables.front();
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->text(0, 0, DisplayMode::Auto), QString("demo_video.ivf"));
        QCOMPARE(table->text(1, 0, DisplayMode::Auto), QString("demo_audio.adx"));
        QVERIFY2(table->text(0, 3, DisplayMode::Auto).contains("1280 × 720"), qPrintable(table->text(0, 3, DisplayMode::Auto)));
        QVERIFY(table->text(1, 3, DisplayMode::Auto).contains("48000 Hz"));
        const auto archive = table->archive();
        QCOMPARE(archive->entryData(0), ivfHead + frame1 + frame2);
        QCOMPARE(archive->entryData(1), adx);

        QString error;
        const auto cut = UsmReader().readBytes(file.left(file.size() - 5), "cut.usm", &error);
        QVERIFY(cut);
        QVERIFY(cut->tables.front()->description().contains("damaged"));
        QVERIFY(!UsmReader().readBytes(QByteArray("CRID") + QByteArray(40, '\xff'), "bad.usm", &error));
        QVERIFY(!UsmReader().readBytes(QByteArray("NOPE") + QByteArray(40, '\0'), "bad.usm", &error));
    }

    void namesAcbSoundsAndPlaysNintendoOpus()
    {
        const QByteArray hello = nintendoOpus(10, 312), world = nintendoOpus(4, 0);
        const QByteArray cueNames = utf("CueName", {{"CueName", 0x0A, true, {"world", "hello"}}, {"CueIndex", 0x02, true, {1, 0}}});
        const QByteArray cues = utf("Cue", {{"ReferenceType", 0x00, true, {1, 2}}, {"ReferenceIndex", 0x02, true, {0, 0}}});
        const QByteArray synths = utf("Synth", {{"ReferenceItems", 0x0B, true, {QByteArray::fromHex("00010001")}}});
        const QByteArray waves = utf("Waveform", {{"MemoryAwbId", 0x02, true, {0, 1}},
                                                  {"StreamAwbId", 0x02, true, {0xFFFF, 0xFFFF}},
                                                  {"Streaming", 0x00, true, {0, 0}}});
        const QByteArray acb = utf("Header", {{"CueNameTable", 0x0B, true, {cueNames}},
                                              {"CueTable", 0x0B, true, {cues}},
                                              {"SynthTable", 0x0B, true, {synths}},
                                              {"WaveformTable", 0x0B, true, {waves}},
                                              {"AwbFile", 0x0B, true, {afs2({hello, world})}}});
        auto doc = open(acb, "voice.acb");
        QVERIFY(doc);
        const auto &sounds = doc->tables.front();
        QCOMPARE(sounds->name(), QString("Sounds"));
        QCOMPARE(sounds->text(0, 0, DisplayMode::Auto), QString("hello.opus"));
        QCOMPARE(sounds->text(1, 0, DisplayMode::Auto), QString("world.opus"));
        QVERIFY2(sounds->description().contains("2 named"), qPrintable(sounds->description()));

        auto archive = sounds->archive();
        QString reader;
        auto sound = open(archive->entryData(0), "hello.opus", &reader);
        QVERIFY(sound);
        QCOMPARE(reader, QString("Nintendo Opus sound"));
        const auto &table = sound->tables.front();
        QVERIFY(table->hasAudio());
        QString error;
        const std::optional<Audio> audio = table->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->sampleRate, 48000);
        QCOMPARE(audio->frameCount(), 10 * 960 - 312);
        qint64 energy = 0;
        for (qsizetype i = 0; i + 1 < audio->pcm.size(); i += 2)
            energy += std::abs(qFromLittleEndian<qint16>(audio->pcm.constData() + i));
        QVERIFY(energy / audio->frameCount() > 1000);

        auto awb = open(afs2({world}), "plain.awb", &reader);
        QVERIFY(awb);
        QCOMPARE(reader, QString("CRI AFS2 archive"));
        QCOMPARE(awb->tables.front()->text(0, 0, DisplayMode::Auto), QString("0000.opus"));
    }
};

QTEST_MAIN(TestCri)
#include "test_cri.moc"
