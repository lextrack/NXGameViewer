#include "archives/compress.h"
#include "modding/nestededit.h"
#include "sample_sarc.h"
#include "archives/sarcreader.h"
#include "archives/sarcwriter.h"
#include "archives/zstddictionaries.h"
#include "data/restblreader.h"
#include "modding/restblupdate.h"

#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>

#include <zdict.h>
#include <zstd.h>

namespace {

QByteArray pattern(qsizetype size, int seed)
{
    QByteArray b(size, '\0');
    for (qsizetype i = 0; i < size; ++i)
        b[i] = char((i * 7 + seed) % 251);
    return b;
}

QByteArray trainedDictionary()
{
    QByteArray samples;
    std::vector<size_t> sizes;
    for (int i = 0; i < 400; ++i) {
        const QByteArray s = "pack entry=" + QByteArray::number(i % 41) + " value=" + QByteArray::number(i * 6151 % 997)
                           + " actor=Npc_" + QByteArray::number(i % 11) + " layer=" + QByteArray::number(i % 3);
        samples += s;
        sizes.push_back(size_t(s.size()));
    }
    QByteArray dict(4096, Qt::Uninitialized);
    const size_t n = ZDICT_trainFromBuffer(dict.data(), size_t(dict.size()), samples.constData(), sizes.data(), unsigned(sizes.size()));
    if (ZDICT_isError(n))
        return {};
    dict.resize(qsizetype(n));
    return dict;
}

std::vector<SarcEntry> sampleEntries()
{
    return {
        {"model/body.bin", pattern(300, 1), 0x80},
        {"tex/icon.bin", pattern(5000, 2), 0x1000},
        {"readme.txt", QByteArray("hello"), 0},
        {"", QByteArray("nameless"), 4, 0xABCDEF01},
        {"empty.bin", QByteArray(), 8},
    };
}

quint32 u32At(const QByteArray &b, qsizetype at, bool little)
{
    return little ? qFromLittleEndian<quint32>(b.constData() + at) : qFromBigEndian<quint32>(b.constData() + at);
}

const SarcEntry *find(const SarcContents &c, const QString &name)
{
    for (const SarcEntry &e : c.entries)
        if (sarcEntryName(e) == name)
            return &e;
    return nullptr;
}

QByteArray unpack(const QByteArray &data)
{
    return unpackAll(data, nullptr, nullptr).value_or(QByteArray());
}

}

class TestArchiveWrite : public QObject
{
    Q_OBJECT

private slots:
    void hashesNames()
    {
        QCOMPARE(sarcNameHash("", 0x65), 0u);
        QCOMPARE(sarcNameHash("ab", 0x65), quint32('a' * 0x65 + 'b'));
        QCOMPARE(sarcNameHash("\xC3\xA9", 0x65), quint32(-61 * 0x65 - 87));
    }

    void writesAndReadsBack_data()
    {
        QTest::addColumn<bool>("little");
        QTest::newRow("little endian") << true;
        QTest::newRow("big endian") << false;
    }

    void writesAndReadsBack()
    {
        QFETCH(bool, little);
        const std::vector<SarcEntry> entries = sampleEntries();
        QString error;
        const auto sarc = writeSarc(entries, {little, 0x65, 0x0100}, &error);
        QVERIFY2(sarc, qPrintable(error));
        QCOMPARE(sarc->mid(6, 2), little ? QByteArray("\xFF\xFE", 2) : QByteArray("\xFE\xFF", 2));
        QCOMPARE(u32At(*sarc, 8, little), quint32(sarc->size()));
        const quint32 dataOffset = u32At(*sarc, 0x0C, little);
        QCOMPARE(dataOffset % 0x1000, 0u);

        const auto back = readSarc(*sarc, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->layout.littleEndian, little);
        QCOMPARE(back->entries.size(), entries.size());
        for (size_t i = 1; i < back->entries.size(); ++i)
            QVERIFY(back->entries[i - 1].hash <= back->entries[i].hash);
        for (const SarcEntry &original : entries) {
            const SarcEntry *e = find(*back, sarcEntryName(original));
            QVERIFY2(e, qPrintable(sarcEntryName(original)));
            QCOMPARE(e->data, original.data);
            if (!original.name.isEmpty())
                QCOMPARE(e->hash, sarcNameHash(original.name.toUtf8(), 0x65));
        }
        for (size_t i = 0; i < back->entries.size(); ++i) {
            const qsizetype at = dataOffset + u32At(*sarc, 0x20 + qsizetype(i) * 16 + 8, little);
            const SarcEntry *original = nullptr;
            for (const SarcEntry &o : entries)
                if (sarcEntryName(o) == sarcEntryName(back->entries[i]))
                    original = &o;
            QCOMPARE(at % (original->alignment ? original->alignment : kSarcDefaultAlignment), 0);
        }

        const auto doc = SarcReader().readBytes(*sarc, "x.sarc", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto archive = doc->tables.front()->archive();
        QCOMPARE(archive->entries.size(), entries.size());
        QCOMPARE(archive->entryData(size_t(archive->find("tex/icon.bin"))), entries[1].data);
        QCOMPARE(archive->entryData(size_t(archive->find("abcdef01.bin"))), QByteArray("nameless"));
    }

    void writesHashCollisions()
    {
        const std::vector<SarcEntry> entries = {{"xa", "first"}, {"ya", "second"}, {"za", "third"}};
        QString error;
        const auto sarc = writeSarc(entries, {true, 0, 0x0100}, &error);
        QVERIFY2(sarc, qPrintable(error));
        QSet<quint32> counters;
        for (int i = 0; i < 3; ++i)
            counters << (u32At(*sarc, 0x20 + i * 16 + 4, true) >> 24);
        QCOMPARE(counters, (QSet<quint32>{1, 2, 3}));
        const auto doc = SarcReader().readBytes(*sarc, "x.sarc", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto archive = doc->tables.front()->archive();
        QCOMPARE(archive->entryData(size_t(archive->find("ya"))), QByteArray("second"));
        QCOMPARE(archive->entryData(size_t(archive->find("za"))), QByteArray("third"));
    }

    void refusesWhatItCannotWrite()
    {
        QString error;
        QVERIFY(!writeSarc({{"a", "x", 3}}, {}, &error));
        QVERIFY(error.contains("power of two"));
        QVERIFY(!writeSarc({{QString(QChar(0)) + "a", "x"}}, {}, &error));
        QVERIFY(error.contains("zero byte"));
    }

    void replacesAndKeepsTheRest_data()
    {
        QTest::addColumn<bool>("little");
        QTest::addColumn<qsizetype>("newSize");
        QTest::newRow("LE bigger") << true << qsizetype(9000);
        QTest::newRow("LE smaller") << true << qsizetype(10);
        QTest::newRow("BE bigger") << false << qsizetype(9000);
        QTest::newRow("BE empty") << false << qsizetype(0);
    }

    void replacesAndKeepsTheRest()
    {
        QFETCH(bool, little);
        QFETCH(qsizetype, newSize);
        const std::vector<SarcEntry> entries = sampleEntries();
        QString error;
        const QByteArray sarc = *writeSarc(entries, {little, 0x65, 0x0100}, &error);

        const auto same = replaceSarcEntry(sarc, "model/body.bin", entries[0].data, &error);
        QVERIFY2(same, qPrintable(error));
        QCOMPARE(*same, sarc);

        const QByteArray replacement = pattern(newSize, 9);
        const auto changed = replaceSarcEntry(sarc, "model/body.bin", replacement, &error);
        QVERIFY2(changed, qPrintable(error));
        QCOMPARE(u32At(*changed, 8, little), quint32(changed->size()));
        const quint32 dataOffset = u32At(sarc, 0x0C, little);
        QCOMPARE(changed->left(8), sarc.left(8));
        QCOMPARE(changed->mid(12, 0x20 - 12), sarc.mid(12, 0x20 - 12));
        for (qsizetype i = 0; i < qsizetype(entries.size()); ++i)
            QCOMPARE(changed->mid(0x20 + i * 16, 8), sarc.mid(0x20 + i * 16, 8));
        const qsizetype namesStart = 0x20 + qsizetype(entries.size()) * 16;
        QCOMPARE(changed->mid(namesStart, dataOffset - namesStart), sarc.mid(namesStart, dataOffset - namesStart));

        const auto before = readSarc(sarc, &error);
        const auto after = readSarc(*changed, &error);
        QVERIFY2(after, qPrintable(error));
        for (size_t i = 0; i < after->entries.size(); ++i) {
            const SarcEntry &a = after->entries[i];
            QCOMPARE(a.data, a.name == "model/body.bin" ? replacement : before->entries[i].data);
            const qsizetype at = dataOffset + u32At(*changed, 0x20 + qsizetype(i) * 16 + 8, little);
            const qsizetype wanted = std::min(before->entries[i].alignment, kSarcMaxInferredAlignment);
            QVERIFY2(at % wanted == 0 || a.data.isEmpty(), qPrintable(a.name));
        }
    }

    void replacesEntriesOfHandMadeArchives()
    {
        const QByteArray sarc = samplesarc::make({{"a.txt", "hello"}, {"", "nameless"}, {"b.txt", "bye"}}, false);
        QString error;
        const auto changed = replaceSarcEntry(sarc, "00001001.bin", "a longer nameless file", &error);
        QVERIFY2(changed, qPrintable(error));
        const auto doc = SarcReader().readBytes(*changed, "x.sarc", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto archive = doc->tables.front()->archive();
        QCOMPARE(archive->entryData(size_t(archive->find("00001001.bin"))), QByteArray("a longer nameless file"));
        QCOMPARE(archive->entryData(size_t(archive->find("a.txt"))), QByteArray("hello"));
        QCOMPARE(archive->entryData(size_t(archive->find("b.txt"))), QByteArray("bye"));
    }

    void refusesBadReplacements()
    {
        QString error;
        const QByteArray sarc = *writeSarc(sampleEntries(), {}, &error);
        QVERIFY(!replaceSarcEntry(sarc, "missing.bin", "x", &error));
        QVERIFY(error.contains("missing.bin"));
        QVERIFY(!replaceSarcEntry("not an archive at all", "a", "x", &error));
        QVERIFY(error.contains("Not a SARC"));

        QByteArray overlapping = samplesarc::make({{"a", "0123456789"}, {"b", "abcdefghij"}});
        const quint32 firstStart = qFromLittleEndian<quint32>(overlapping.constData() + 0x20 + 8);
        qToLittleEndian(firstStart + 4, overlapping.data() + 0x20 + 16 + 8);
        QVERIFY(!replaceSarcEntry(overlapping, "a", "x", &error));
        QVERIFY2(error.contains("overlap"), qPrintable(error));
    }

    void compressesYaz0_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("one byte") << QByteArray("x");
        QTest::newRow("short") << QByteArray("abcabcabcabc");
        QTest::newRow("long run") << QByteArray(5000, 'z');
        QTest::newRow("pattern") << pattern(70000, 3);
        QByteArray random(20000, '\0');
        QRandomGenerator rng(7);
        for (char &c : random)
            c = char(rng.bounded(256));
        QTest::newRow("random") << random;
        QByteArray far = random.left(0x1000) + random.left(0x1000) + random.left(0x1001);
        QTest::newRow("matches at the window edge") << far;
    }

    void compressesYaz0()
    {
        QFETCH(QByteArray, data);
        const QByteArray packed = compressYaz0(data, 0x2000);
        QCOMPARE(packed.left(4), QByteArray("Yaz0"));
        QCOMPARE(qFromBigEndian<quint32>(packed.constData() + 4), quint32(data.size()));
        QCOMPARE(qFromBigEndian<quint32>(packed.constData() + 8), 0x2000u);
        QString error;
        const auto back = decompress(packed, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(*back, data);
        if (data == QByteArray(5000, 'z'))
            QVERIFY(packed.size() < 200);
    }

    void keepsCompressionSettings()
    {
        const QByteArray plain = pattern(10000, 5);
        QString error;
        for (bool checksum : {false, true}) {
            const QByteArray packed = *compressZstd(plain, 3, checksum, &error);
            std::vector<CompressionLayer> layers;
            QCOMPARE(unpackAll(packed, &layers, &error).value_or(QByteArray()), plain);
            QCOMPARE(layers.size(), size_t(1));
            QCOMPARE(layers[0].zstdChecksum, checksum);
            const QByteArray again = *packAll(plain, layers, &error);
            std::vector<CompressionLayer> layers2;
            QCOMPARE(unpackAll(again, &layers2, &error).value_or(QByteArray()), plain);
            QCOMPARE(layers2[0].zstdChecksum, checksum);
        }
        const QByteArray twice = *compressZstd(compressYaz0(plain, 0x80), 3, false, &error);
        std::vector<CompressionLayer> layers;
        QCOMPARE(unpackAll(twice, &layers, &error).value_or(QByteArray()), plain);
        QCOMPARE(layers.size(), size_t(2));
        QCOMPARE(int(layers[1].kind), int(Compression::Yaz0));
        QCOMPARE(layers[1].yaz0Alignment, 0x80u);
        QCOMPARE(unpack(*packAll(plain, layers, &error)), plain);
    }

    void refusesUnknownZstdDictionaries()
    {
        const QByteArray frame("\x28\xB5\x2F\xFD\x01\x00\x05\x00\x00\x00", 10);
        QString error;
        QVERIFY(!describeCompression(frame, &error));
        QVERIFY2(error.contains("dictionary"), qPrintable(error));
    }

    void keepsZstdDictionaries()
    {
        const QByteArray dict = trainedDictionary();
        QVERIFY(!dict.isEmpty());
        const unsigned id = ZDICT_getDictID(dict.constData(), size_t(dict.size()));
        QVERIFY(id != 0);
        QString error;
        const QByteArray pack = *compressZstd(*writeSarc({{"zs.zsdic", dict}}, {}, &error), 3, false, &error);
        QVERIFY(addZstdDictionaries(pack, QStringLiteral("test:keepsZstdDictionaries")) >= 0);
        QVERIFY(!zstdDictionaryBytes(id).isEmpty());

        const QByteArray sarc = *writeSarc({{"Actor/a.txt", "pack entry=3 value=12 actor=Npc_4 layer=1"}, {"b.txt", "other"}}, {}, &error);
        const QByteArray stored = *compressZstd(sarc, 3, false, &error, id);
        QCOMPARE(ZSTD_getDictID_fromFrame(stored.constData(), size_t(stored.size())), id);
        std::vector<CompressionLayer> layers;
        QCOMPARE(unpackAll(stored, &layers, &error).value_or(QByteArray()), sarc);
        QCOMPARE(layers.size(), size_t(1));
        QCOMPARE(layers[0].zstdDictionary, id);

        const LeafEdit edit = [](const QByteArray &, QString *) { return std::optional<QByteArray>("changed"); };
        const auto result = editNested(stored, "Actor.pack.zs", {"Actor/a.txt"}, edit, nullptr, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(ZSTD_getDictID_fromFrame(result->constData(), size_t(result->size())), id);
        const auto contents = readSarc(*unpackAll(*result, nullptr, &error), &error);
        QVERIFY2(contents, qPrintable(error));
        bool found = false;
        for (const SarcEntry &e : contents->entries)
            if (sarcEntryName(e) == "Actor/a.txt") {
                QCOMPARE(e.data, QByteArray("changed"));
                found = true;
            }
        QVERIFY(found);
        QVERIFY(!compressZstd(sarc, 3, false, &error, id + 1));
    }

    void raisesResourceSizesForGrownFiles()
    {
        QString error;
        const QByteArray pack = *compressZstd(*writeSarc({{"Actor/Foo.bgyml", QByteArray(100, 'a')}, {"Actor/Bar.bgyml", "bar"}}, {}, &error), 3, false, &error);
        const LeafEdit grow = [](const QByteArray &, QString *) { return std::optional<QByteArray>(QByteArray(300, 'b')); };
        const auto changed = editNested(pack, "Foo.pack.zs", {"Actor/Foo.bgyml"}, grow, nullptr, &error);
        QVERIFY2(changed, qPrintable(error));
        const auto before = unpackedSizes(pack, "Pack/Actor/Foo.pack.zs", {"Actor/Foo.bgyml"}, &error);
        const auto after = unpackedSizes(*changed, "Pack/Actor/Foo.pack.zs", {"Actor/Foo.bgyml"}, &error);
        QVERIFY2(before && after, qPrintable(error));
        QCOMPARE(before->size(), size_t(2));
        QCOMPARE(before->at(0).path, QString("Pack/Actor/Foo.pack"));
        QCOMPARE(before->at(1).path, QString("Actor/Foo.bgyml"));
        QCOMPARE(before->at(1).size, qint64(100));
        QCOMPARE(after->at(1).size, qint64(300));
        const qint64 packGrowth = after->at(0).size - before->at(0).size;
        QVERIFY(packGrowth > 0);

        Restbl table;
        table.nameSize = 32;
        table.hashed = {{restblPathHash("Other/Thing.bgyml"), 64}, {restblPathHash("Pack/Actor/Foo.pack"), 5000}};
        table.named = {{"Actor/Foo.bgyml", 1000}};
        const QByteArray game = *compressZstd(writeRestbl(table), 3, false, &error);
        const auto update = raiseRestbl(game, game, *before, *after, &error);
        QVERIFY2(update, qPrintable(error));
        QCOMPARE(update->raised.size(), size_t(2));
        QVERIFY(update->unlisted.isEmpty());
        const auto written = parseRestbl(*unpackAll(update->file, nullptr, &error), &error);
        QVERIFY2(written, qPrintable(error));
        QCOMPARE(written->named.front().second, 1000u + 224u);
        QCOMPARE(written->hashed[1].second, quint32(5000 + (packGrowth + 31) / 32 * 32));
        QCOMPARE(written->hashed[0].second, 64u);

        const auto again = raiseRestbl(game, update->file, *before, *after, &error);
        QVERIFY2(again, qPrintable(error));
        QVERIFY(again->raised.empty());
        QVERIFY(again->file.isEmpty());

        const std::vector<UnpackedSize> unlistedAfter{{"New/File.bgyml", 50}};
        const auto unlisted = raiseRestbl(game, game, {{"New/File.bgyml", 10}}, unlistedAfter, &error);
        QVERIFY2(unlisted, qPrintable(error));
        QCOMPARE(unlisted->unlisted, QStringList{"New/File.bgyml"});

        QCOMPARE(newestRestblName({"ResourceSizeTable.Product.100.Nin_Ounce_NVN2.rsizetable.zs",
                                   "ResourceSizeTable.Product.120.Nin_Ounce_NVN2.rsizetable.zs", "readme.txt"}),
                 QString("ResourceSizeTable.Product.120.Nin_Ounce_NVN2.rsizetable.zs"));
        QVERIFY(isRestblPath("System/Resource/ResourceSizeTable.Product.100.rsizetable.zs"));
        QCOMPARE(restblKey("Pack/A.pack.zs"), QString("Pack/A.pack"));
    }

    void editsNestedArchives()
    {
        QString error;
        const QByteArray deep = *writeSarc({{"deep/tex.bin", "old texture", 0x100}, {"deep/other.bin", "other"}}, {false}, &error);
        const QByteArray middle =
            *writeSarc({{"a.txt", "first"}, {"inner.szs", compressYaz0(deep, 0x100), 0x80}}, {}, &error);
        const QByteArray outer = *compressZstd(middle, 3, true, &error);

        std::vector<RebuiltLayer> rebuilt;
        QByteArray seen;
        const auto result = editNested(
            outer, "pack.sarc.zs", {"inner.szs", "deep/tex.bin"},
            [&](const QByteArray &leaf, QString *) -> std::optional<QByteArray> {
                seen = leaf;
                return QByteArray("new and longer texture");
            },
            &rebuilt, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(seen, QByteArray("old texture"));

        std::vector<CompressionLayer> layers;
        const auto newMiddle = unpackAll(*result, &layers, &error);
        QCOMPARE(layers.size(), size_t(1));
        QVERIFY(layers[0].zstdChecksum);
        const auto middleContents = readSarc(*newMiddle, &error);
        QCOMPARE(find(*middleContents, "a.txt")->data, QByteArray("first"));
        const QByteArray innerStored = find(*middleContents, "inner.szs")->data;
        std::vector<CompressionLayer> innerLayers;
        const auto newDeep = unpackAll(innerStored, &innerLayers, &error);
        QCOMPARE(int(innerLayers.at(0).kind), int(Compression::Yaz0));
        QCOMPARE(innerLayers[0].yaz0Alignment, 0x100u);
        const auto deepContents = readSarc(*newDeep, &error);
        QVERIFY(!deepContents->layout.littleEndian);
        QCOMPARE(find(*deepContents, "deep/tex.bin")->data, QByteArray("new and longer texture"));
        QCOMPARE(find(*deepContents, "deep/other.bin")->data, QByteArray("other"));

        QCOMPARE(rebuilt.size(), size_t(2));
        QCOMPARE(rebuilt[0].name, QString("pack.sarc.zs"));
        QCOMPARE(rebuilt[0].compression, QStringList{"zstd"});
        QCOMPARE(rebuilt[1].name, QString("inner.szs"));
        QCOMPARE(rebuilt[1].compression, QStringList{"Yaz0"});
        QVERIFY(rebuilt[1].newUnpacked > rebuilt[1].oldUnpacked);
    }

    void nestedEditReportsProblems()
    {
        QString error;
        const QByteArray sarc = *writeSarc({{"a.txt", "first"}}, {}, &error);
        auto keep = [](const QByteArray &leaf, QString *) -> std::optional<QByteArray> { return leaf; };
        QVERIFY(!editNested(sarc, "x.sarc", {"missing"}, keep, nullptr, &error));
        QVERIFY2(error.contains("missing"), qPrintable(error));
        QVERIFY(!editNested(sarc, "x.sarc", {"a.txt", "deeper"}, keep, nullptr, &error));
        QVERIFY2(error.contains("not a SARC"), qPrintable(error));
        auto refuse = [](const QByteArray &, QString *e) -> std::optional<QByteArray> {
            *e = "leaf refused";
            return std::nullopt;
        };
        QVERIFY(!editNested(sarc, "x.sarc", {"a.txt"}, refuse, nullptr, &error));
        QCOMPARE(error, QString("leaf refused"));
        QCOMPARE(editNested(sarc, "x.sarc", {"a.txt"}, keep, nullptr, &error).value_or(QByteArray()), sarc);
    }

    void brokenInputNeverCrashes()
    {
        QString error;
        const QByteArray inner = *writeSarc({{"t.bin", pattern(700, 4), 0x100}}, {}, &error);
        const QByteArray sarc = *writeSarc(
            {{"a.bin", pattern(200, 1), 0x80}, {"b.szs", compressYaz0(inner)}, {"c.zs", *compressZstd(inner, 3, false, &error)}},
            {}, &error);
        const QByteArray packed = compressYaz0(sarc);
        auto keep = [](const QByteArray &leaf, QString *) -> std::optional<QByteArray> { return leaf + "x"; };
        auto tryAll = [&](const QByteArray &d) {
            readSarc(d, &error);
            replaceSarcEntry(d, "a.bin", "zz", &error);
            replaceSarcEntry(d, "b.szs", QByteArray(3000, 'q'), &error);
            editNested(d, "x", {"b.szs", "t.bin"}, keep, nullptr, &error);
            editNested(d, "x", {"c.zs", "t.bin"}, keep, nullptr, &error);
            std::vector<CompressionLayer> layers;
            unpackAll(d, &layers, &error);
        };
        for (const QByteArray &base : {sarc, packed}) {
            for (qsizetype n = 0; n <= base.size(); n += (n < 300 ? 1 : 37))
                tryAll(base.left(n));
            QRandomGenerator rng(11);
            for (int round = 0; round < 1500; ++round) {
                QByteArray d = base;
                for (int k = 0; k < 1 + int(rng.bounded(6)); ++k) {
                    const qsizetype at = rng.bounded(int(d.size()));
                    if (round % 2 && at + 4 <= d.size())
                        qToLittleEndian(quint32(round % 3 ? 0xFFFFFFFF : rng.generate()), d.data() + at);
                    else
                        d[at] = char(rng.bounded(256));
                }
                tryAll(d);
            }
        }
    }
};

QTEST_MAIN(TestArchiveWrite)
#include "test_archivewrite.moc"
