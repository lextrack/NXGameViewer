#include "archives/compress.h"
#include "archives/rpf6reader.h"
#include "core/crypto.h"
#include "core/registry.h"
#include "archives/sarcreader.h"
#include "sample_rpf6.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <zlib.h>


using namespace samplerpf6;

class TestRpf6 : public QObject
{
    Q_OBJECT

private slots:
    void init() { Rpf6Reader::setKeys({}); }

    void aesMatchesTheStandard()
    {
        const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        const QByteArray block = crypto::ecbEncrypt(key, QByteArray::fromHex("00112233445566778899aabbccddeeff"));
        QCOMPARE(block.toHex(), QByteArray("8ea2b7ca516745bfeafc49904b496089"));
        const QByteArray plain = QByteArray::fromHex("00112233445566778899aabbccddeeff") + "tail";
        QCOMPARE(rpf6Decrypt(key, rpf6Encrypt(key, plain)), plain);
        QCOMPARE(rpf6Encrypt(key, plain).right(4), QByteArray("tail"));
        QCOMPARE(joaat("Hello"), joaat("hello"));
        QCOMPARE(joaat(""), 0u);
    }

    void readsEncryptedArchivesOnceTheKeyIsKnown()
    {
        const Sample s = makeArchive();
        QTemporaryDir dir;
        const QString path = dir.filePath("content.rpf");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(s.file);
        f.close();

        QString error;
        QVERIFY(!Rpf6Reader().read(path, &error));
        QVERIFY2(error.contains("encrypted"), qPrintable(error));
        QVERIFY(Rpf6Reader::needsKey(path));

        Rpf6Reader::setKeys({QByteArray(32, 'x'), testKey()});
        QVERIFY(!Rpf6Reader::needsKey(path));
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE RPF6 archive"));
        const auto archive = doc->tables.front()->archive();
        QVERIFY(archive);
        QCOMPARE(archive->entries.size(), size_t(6));
        QCOMPARE(archive->entryData(size_t(archive->find("plain.txt"))), s.plain);
        QCOMPARE(archive->entryData(size_t(archive->find("packed.bin"))), s.packed);
        QCOMPARE(archive->entryData(size_t(archive->find("sub/zlib.dat"))), s.zlibbed);
        QCOMPARE(archive->entryData(size_t(archive->find("sub/config.dat"))), s.deflated);
        const QByteArray resource = archive->entryData(size_t(archive->find("sub/thing.wtd")));
        QVERIFY(resource.startsWith(QByteArrayView("RSC\x05", 4)));
        QCOMPARE(qFromLittleEndian<quint32>(resource.constData() + 8), 0xC0000123u);
        QCOMPARE(resource.mid(12), s.resource);
        QCOMPARE(archive->entryData(size_t(archive->find("sub/logic.wsc"))), s.script);

        auto inMemory = Rpf6Reader().readBytes(s.file, "content.rpf", &error);
        QVERIFY2(inMemory, qPrintable(error));
        QCOMPARE(inMemory->tables.front()->archive()->entryData(1), archive->entryData(1));
    }

    void opensHugeEntriesInPieces()
    {
        const Sample s = makeArchive(false);
        Archive outer;
        const qint64 huge = kMaxEntryInMemory + 1;
        outer.entries = {{"big.rpf", 0, huge}, {"huge.bin", 0, huge}};
        bool loaded = false;
        outer.loader = [&loaded](size_t) {
            loaded = true;
            return QByteArray();
        };
        outer.stream = [&s, huge](size_t index) -> StoredReader {
            return [&s, huge, index](qint64 at, qint64 length) {
                if (at < 0 || length < 0 || at > huge - length)
                    return QByteArray();
                QByteArray out(length, index == 0 ? '\0' : 'H');
                if (index == 0 && at < s.file.size())
                    out.replace(0, std::min(length, s.file.size() - at), s.file.mid(at, length));
                return out;
            };
        };
        QVERIFY(outer.entryData(0).isEmpty());

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        auto doc = registry.openEntry(outer, 0, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), Rpf6Reader().name());
        const auto inner = doc->tables.front()->archive();
        QCOMPARE(inner->entryData(size_t(inner->find("plain.txt"))), s.plain);

        auto hex = registry.openEntry(outer, 1, &used, &error);
        QVERIFY2(hex, qPrintable(error));
        QCOMPARE(used->name(), QString("Hex"));
        QVERIFY(hex->tables.front()->description().contains("showing the first"));
        QCOMPARE(hex->tables.front()->text(0, 1, DisplayMode::Auto).left(4), QString("HHHH"));

        QVERIFY(!FormatRegistry::openEntryWith(SarcReader(), outer, 0, &error));
        QVERIFY2(error.contains("Extract"), qPrintable(error));
        QVERIFY(!loaded);
    }

    void readsPlainArchives()
    {
        const Sample s = makeArchive(false);
        QString error;
        auto doc = Rpf6Reader().readBytes(s.file, "plain.rpf", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto archive = doc->tables.front()->archive();
        QCOMPARE(archive->entryData(size_t(archive->find("plain.txt"))), s.plain);
    }

    void findsKeysStoredAsBytes()
    {
        const Sample s = makeArchive();
        const QByteArray first = s.file.mid(16, 16);
        QByteArray data(0x3000, '\0');
        for (int i = 0; i < data.size(); ++i)
            data[i] = char((i * 31) & 0x0F);
        data.replace(0x1234, 32, testKey());
        QString error;
        const auto key = findRpf6Key(nso(QByteArray(0x100, '\0'), QByteArray(0x200, '\0'), data), first, &error);
        QVERIFY2(key, qPrintable(error));
        QCOMPARE(*key, testKey());

        QVERIFY(!findRpf6Key(nso(QByteArray(0x100, '\0'), QByteArray(0x200, 'a'), QByteArray(0x400, 'b')), first, &error));
        QVERIFY(!error.isEmpty());
    }

    void findsKeysBuiltByTheKeySetup()
    {
        const Sample s = makeArchive();
        const QByteArray key = testKey();
        QByteArray constants;
        for (int i = 0; i < 8; ++i) {
            char word[8];
            qToLittleEndian<quint64>(qFromBigEndian<quint32>(key.constData() + i * 4), word);
            constants.append(word, 8);
        }
        QByteArray ro(0x1000, '\0');
        ro.replace(0x40, 64, constants);
        const quint32 pageDelta = 1;
        const quint32 adrp = 0x90000000u | ((pageDelta & 3) << 29) | ((pageDelta >> 2) << 5) | 9;
        auto ldrq = [](int rt, int rn, int index) { return 0x3dc00000u | (quint32(index) << 10) | (quint32(rn) << 5) | quint32(rt); };
        auto stpq = [](int rt, int rt2, int rn, int imm) {
            return 0xad000000u | ((quint32(imm) & 0x7f) << 15) | (quint32(rt2) << 10) | (quint32(rn) << 5) | quint32(rt);
        };
        QByteArray text = instructions({0xd503201f, 0x7100003f, 0x12800048, 0x1a810108, 0x31000d1f, 0xb9000008,
                                        0x54000001, adrp, ldrq(0, 9, 4), ldrq(1, 9, 5), ldrq(2, 9, 6), ldrq(3, 9, 7),
                                        0x91000000u | (8 << 10) | 8, stpq(0, 1, 8, 0), stpq(2, 3, 8, 2), 0x14000000,
                                        0xd65f03c0});
        text.resize(0x1000, '\0');
        QString error;
        const auto found = findRpf6Key(nso(text, ro, QByteArray(0x100, '\0')), s.file.mid(16, 16), &error);
        QVERIFY2(found, qPrintable(error));
        QCOMPARE(*found, key);
    }

    void rejectsDamagedArchives()
    {
        Sample s = makeArchive(false);
        QString error;
        QByteArray cut = s.file.left(40);
        QVERIFY(!Rpf6Reader().readBytes(cut, "x.rpf", &error));
        QByteArray loop = s.file;
        putBe(loop, 16 + 3 * 20 + 8, 0x80000000u);
        QVERIFY(!Rpf6Reader().readBytes(loop, "x.rpf", &error));
        QByteArray everything = s.file;
        putBe(everything, 16 + 3 * 20 + 8, 0x80000001u);
        putBe(everything, 16 + 3 * 20 + 12, 7);
        QVERIFY(!Rpf6Reader().readBytes(everything, "x.rpf", &error));
        QByteArray huge = s.file;
        putBe(huge, 4, 0x7FFFFFFF);
        QVERIFY(!Rpf6Reader().readBytes(huge, "x.rpf", &error));
    }
};

QTEST_GUILESS_MAIN(TestRpf6)
#include "test_rpf6.moc"
