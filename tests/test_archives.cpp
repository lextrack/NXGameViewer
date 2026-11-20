#include "core/contentkind.h"
#include "archives/decompress.h"
#include "core/registry.h"
#include "sample_sarc.h"
#include "archives/sarcreader.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <zstd.h>
#include <zdict.h>
#include "archives/sarcwriter.h"
#include "archives/zstddictionaries.h"
#include <QDir>

namespace {

QByteArray zstdCompress(const QByteArray &data)
{
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(data.size()))), Qt::Uninitialized);
    const size_t n = ZSTD_compress(out.data(), size_t(out.size()), data.constData(), size_t(data.size()), 3);
    out.resize(qsizetype(n));
    return out;
}

QByteArray trainDictionary(const QByteArray &flavour)
{
    QByteArray samples;
    std::vector<size_t> sizes;
    for (int i = 0; i < 400; ++i) {
        const QByteArray s = flavour + " actor=" + QByteArray::number(i % 37) + " param=" + QByteArray::number(i * 7919 % 1000)
                           + " name=Npc_" + QByteArray::number(i % 13) + "_Child flags=" + QByteArray::number(i % 5);
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

QByteArray compressWithDictionary(const QByteArray &data, const QByteArray &dict)
{
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(data.size()))), Qt::Uninitialized);
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    const size_t n = ZSTD_compress_usingDict(cctx, out.data(), size_t(out.size()), data.constData(), size_t(data.size()),
                                             dict.constData(), size_t(dict.size()), 3);
    ZSTD_freeCCtx(cctx);
    out.resize(qsizetype(n));
    return out;
}

QByteArray zsDicPack(const QByteArray &dict)
{
    QString error;
    return zstdCompress(*writeSarc({{QStringLiteral("zs.zsdic"), dict, 0, 0}}, SarcLayout{}, &error));
}

QByteArray yaz0Header(quint32 size)
{
    QByteArray out("Yaz0", 4);
    char raw[4];
    qToBigEndian(size, raw);
    out.append(raw, 4);
    out.append(QByteArray(8, '\0'));
    return out;
}

QByteArray sampleArchive()
{
    return samplesarc::make({{"a/first.txt", "hello"}, {"second.bin", QByteArray("\x01\x02\x03", 3)}, {"", "nameless"}});
}

}

class TestArchives : public QObject
{
    Q_OBJECT

private slots:
    void chosenFormatsSeeUnpackedData()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("pack.zs");
        const QByteArray sarc = samplesarc::make({{"a.txt", "hello"}});
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zstdCompress(sarc));
        f.close();
        FormatRegistry registry;
        QString error;
        const auto archive = FormatRegistry::openWith(*registry.byName("SARC archive"), path, &error);
        QVERIFY2(archive, qPrintable(error));
        QVERIFY(archive->tables.front()->archive());
        const auto raw = FormatRegistry::openWith(*registry.byName("Hex"), path, &error);
        QVERIFY2(raw, qPrintable(error));
        QVERIFY(raw->tables.front()->text(0, 0, DisplayMode::Auto).startsWith("28 B5 2F FD"));
        const auto bytes = FormatRegistry::openBytesWith(*registry.byName("SARC archive"), zstdCompress(sarc), "x.zs", &error);
        QVERIFY2(bytes, qPrintable(error));
        QVERIFY(!FormatRegistry::openWith(*registry.byName("BYML data"), path, &error));
        QVERIFY(!error.isEmpty());
    }

    void sortsContentIntoCategories()
    {
        QCOMPARE(contentCategory("SARC...."), ContentCategory::Archive);
        QCOMPARE(contentCategory(QByteArray::fromHex("28b52ffd")), ContentCategory::Compressed);
        QCOMPARE(contentCategory("Yaz0...."), ContentCategory::Compressed);
        QCOMPARE(contentCategory("FRES    "), ContentCategory::Model);
        QCOMPARE(contentCategory("MDL \x05"), ContentCategory::Model);
        QCOMPARE(contentCategory("BNTX...."), ContentCategory::Texture);
        QCOMPARE(contentCategory("\x89PNG...."), ContentCategory::Image);
        QCOMPARE(contentCategory("BWAV...."), ContentCategory::Sound);
        QCOMPARE(contentCategory("BARS...."), ContentCategory::Sound);
        QCOMPARE(contentCategory("MsgStdBn"), ContentCategory::Text);
        QCOMPARE(contentCategory("YB\x04\x00"), ContentCategory::Data);
        QCOMPARE(contentCategory(QByteArray(12, '\x01') + "VSCB"), ContentCategory::Data);
        QCOMPARE(contentCategory("hello"), ContentCategory::Other);
        QCOMPARE(describeContent(QByteArray(12, '\x01') + "VSCB"), QString("BCSV table"));
        QCOMPARE(describeContent("FRES    "), QString("BFRES model"));
    }

    void decompressesYaz0()
    {
        QByteArray data = yaz0Header(9);
        data += char(0xE0);
        data += "abc";
        data += char(0x40);
        data += char(0x02);
        QString error;
        QCOMPARE(decompress(data, &error).value_or(QByteArray()), QByteArray("abcabcabc"));

        QByteArray longRun = yaz0Header(1 + 0x12 + 5);
        longRun += char(0x80);
        longRun += 'z';
        longRun += char(0x00);
        longRun += char(0x00);
        longRun += char(0x05);
        QCOMPARE(decompress(longRun, &error).value_or(QByteArray()), QByteArray(1 + 0x12 + 5, 'z'));
    }

    void rejectsDamagedYaz0()
    {
        QByteArray data = yaz0Header(4);
        data += char(0x00);
        data += char(0x00);
        data += char(0x10);
        QString error;
        QVERIFY(!decompress(data, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!decompress(yaz0Header(100) + QByteArray(1, char(0xFF)), &error));
    }

    void decompressesZstd()
    {
        const QByteArray original = QByteArray("Animal data ").repeated(5000);
        QString error;
        QCOMPARE(decompress(zstdCompress(original), &error).value_or(QByteArray()), original);
        const QByteArray packed = zstdCompress(original);
        QVERIFY(!decompress(packed.left(packed.size() / 2), &error));
        QVERIFY(!error.isEmpty());
    }

    void decompressesZstdThatFillsTheLastOutputChunk()
    {
        const qsizetype chunk = qsizetype(ZSTD_DStreamOutSize());
        for (qsizetype size : {chunk, 2 * chunk, 3 * chunk, 2 * chunk + 1, 2 * chunk - 1}) {
            QByteArray original(size, Qt::Uninitialized);
            for (qsizetype i = 0; i < size; ++i)
                original[i] = char((i * 7 + i / 1000) & 0xFF);
            QString error;
            const std::optional<QByteArray> out = decompress(zstdCompress(original), &error);
            QVERIFY2(out, qPrintable(QStringLiteral("%1 bytes: %2").arg(size).arg(error)));
            QCOMPARE(out->size(), size);
            QCOMPARE(*out, original);
        }
        const QByteArray twoFrames = zstdCompress(QByteArray(int(chunk), 'a')) + zstdCompress("tail");
        QString error;
        QCOMPARE(decompress(twoFrames, &error).value_or(QByteArray()), QByteArray(int(chunk), 'a') + "tail");
    }

    void namesDecompressedFiles()
    {
        QCOMPARE(decompressedName("Dialog_EUen.sarc.zs"), QString("Dialog_EUen.sarc"));
        QCOMPARE(decompressedName("Park.Nin_NX_NVN.szs"), QString("Park.Nin_NX_NVN.sarc"));
        QCOMPARE(decompressedName("plain.bin"), QString("plain.bin"));
    }

    void readsSarcInBothByteOrders()
    {
        for (bool little : {true, false}) {
            const QByteArray bytes = samplesarc::make({{"a/first.txt", "hello"}, {"second.bin", "xyz"}, {"", "nameless"}}, little);
            QString error;
            const auto doc = SarcReader().readBytes(bytes, "test.sarc", &error);
            QVERIFY2(doc, qPrintable(error));
            const auto archive = doc->tables.front()->archive();
            QVERIFY(archive);
            QCOMPARE(archive->entries.size(), size_t(3));
            QCOMPARE(archive->entries[0].name, QString("a/first.txt"));
            QCOMPARE(archive->entryData(0), QByteArray("hello"));
            QCOMPARE(archive->entryData(1), QByteArray("xyz"));
            QCOMPARE(archive->entries[2].name, QString("00001002.bin"));
            QCOMPARE(archive->entryData(2), QByteArray("nameless"));
            QCOMPARE(archive->find("second.bin"), 1);
            QCOMPARE(doc->tables.front()->rowCount(), 3);
        }
    }

    void truncatedSarcNeverCrashes()
    {
        const QByteArray full = sampleArchive();
        for (qsizetype n = 0; n < full.size(); ++n) {
            QString error;
            if (const auto doc = SarcReader().readBytes(full.left(n), "x", &error))
                if (const auto archive = doc->tables.front()->archive())
                    for (size_t i = 0; i < archive->entries.size(); ++i)
                        QVERIFY(archive->entries[i].offset + archive->entries[i].size <= n);
        }
    }

    void opensCompressedArchivesFromDisk()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("pack.sarc.zs");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(zstdCompress(sampleArchive()));
        f.close();

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("SARC archive"));
        QCOMPARE(doc->container, QString("zstd"));
        const auto archive = doc->tables.front()->archive();
        QVERIFY(archive);

        const auto inner = registry.openBytes(archive->entryData(0), archive->entries[0].name, &used, &error);
        QVERIFY2(inner, qPrintable(error));
        QCOMPARE(used->name(), QString("Text"));
    }

    void usesTheGamesZstdDictionaries()
    {
        const QByteArray dict = trainDictionary("disk");
        QVERIFY(!dict.isEmpty());
        QVERIFY(ZDICT_getDictID(dict.constData(), size_t(dict.size())) != 0);
        const QByteArray text = "disk actor=12 param=345 name=Npc_3_Child flags=2 and a little more text";
        const QByteArray packed = compressWithDictionary(text, dict);
        QVERIFY(needsUnknownZstdDictionary(packed));
        QString error;
        QVERIFY(!decompress(packed, &error));
        QVERIFY2(error.contains("ZsDic.pack.zs"), qPrintable(error));

        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("romfs/Pack") && QDir(dir.path()).mkpath("romfs/Event/Deep"));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(dir.filePath("romfs/Pack/ZsDic.pack.zs"), zsDicPack(dict)));
        const QString file = dir.filePath("romfs/Event/Deep/flow.txt.zs");
        QVERIFY(write(file, packed));
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        const auto doc = registry.open(file, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("Text"));
        QCOMPARE(doc->container, QString("zstd"));
        QCOMPARE(*decompress(packed, &error), text);
        QVERIFY(zstdDictionaryRoots().contains(QDir(dir.filePath("romfs")).absolutePath()));

        const QByteArray other = trainDictionary("packaged");
        const QByteArray inner = compressWithDictionary("packaged actor=1 param=2 name=Npc_4_Child flags=0", other);
        QVERIFY(needsUnknownZstdDictionary(inner));
        const auto sarc = writeSarc({{QStringLiteral("Game [0400AAAA00000000]/romfs/Pack/ZsDic.pack.zs"), zsDicPack(other), 0, 0},
                                     {QStringLiteral("Game [0400AAAA00000000]/romfs/Event/flow.txt.zs"), inner, 0, 0}},
                                    SarcLayout{}, &error);
        QVERIFY(sarc);
        const auto outer = registry.openBytes(*sarc, "game.sarc", &used, &error);
        QVERIFY2(outer, qPrintable(error));
        const auto archive = outer->tables.front()->archive();
        size_t index = 0;
        for (size_t i = 0; i < archive->entries.size(); ++i)
            if (archive->entries[i].name.endsWith("flow.txt.zs"))
                index = i;
        const auto entry = registry.openEntry(*archive, index, &used, &error);
        QVERIFY2(entry, qPrintable(error));
        QCOMPARE(used->name(), QString("Text"));
        QVERIFY(!needsUnknownZstdDictionary(inner));
    }

    void opensNestedCompressionInMemory()
    {
        FormatRegistry registry;
        QByteArray stored = yaz0Header(quint32(sampleArchive().size()));
        const QByteArray plain = sampleArchive();
        for (qsizetype i = 0; i < plain.size(); i += 8) {
            stored += char(0xFF);
            stored += plain.mid(i, 8);
        }
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(zstdCompress(stored), "deep.szs.zs", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("SARC archive"));
        QCOMPARE(doc->container, QString("zstd, Yaz0"));
    }
};

QTEST_GUILESS_MAIN(TestArchives)
#include "test_archives.moc"
