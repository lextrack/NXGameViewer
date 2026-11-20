#include "data/falcomtblreader.h"
#include "data/hexreader.h"
#include "core/registry.h"
#include "sample_tbl.h"
#include "core/textdecode.h"
#include "data/textreader.h"

#include <QFile>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>

#include <random>

namespace {

QByteArray encode(const QString &text, const char *encoding)
{
    QStringEncoder encoder(encoding);
    return encoder.encode(text);
}

const QString kJapanese = QStringLiteral("通常攻撃のダメージを与える。素早く斬りつける技です。");
const QString kKorean = QStringLiteral("일반 공격으로 적에게 피해를 입힌다. 빠르게 베는 기술이다.");
const QString kSimplified = QStringLiteral("这是我们的普通攻击，对敌人造成伤害。他说这个技能很快。");
const QString kTraditional = QStringLiteral("這是我們的普通攻擊，對敵人造成傷害。他說這個技能很快。");

QByteArray randomBytes(qsizetype size)
{
    std::mt19937 rng(1234);
    QByteArray b(size, Qt::Uninitialized);
    for (char &c : b)
        c = char(rng() & 0xFF);
    return b;
}

QByteArray binaryWithStrings(const QList<QByteArray> &strings)
{
    QByteArray b = randomBytes(256);
    for (const QByteArray &s : strings)
        b += QByteArray(4, '\0') + s + QByteArray(4, '\0') + randomBytes(64);
    return b;
}

}

class TestText : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString write(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        return path;
    }

private slots:
    void detectsTextEncodings_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("expected");
        QTest::newRow("utf-8") << kJapanese.toUtf8() << int(TextEncoding::Utf8);
        QTest::newRow("utf-8 bom") << QByteArray("\xEF\xBB\xBF") + QByteArray("abc") << int(TextEncoding::Utf8);
        QTest::newRow("utf-16le bom") << QByteArray("\xFF\xFE") + encode(kKorean, "UTF-16LE") << int(TextEncoding::Utf16LE);
        QTest::newRow("utf-16le") << encode(QStringLiteral("Hello world, this is text"), "UTF-16LE") << int(TextEncoding::Utf16LE);
        QTest::newRow("utf-16be") << encode(QStringLiteral("Hello world, this is text"), "UTF-16BE") << int(TextEncoding::Utf16BE);
        QTest::newRow("shift-jis") << encode(kJapanese, "Shift_JIS") << int(TextEncoding::ShiftJis);
        QTest::newRow("euc-kr") << encode(kKorean, "EUC-KR") << int(TextEncoding::EucKr);
        QTest::newRow("gbk") << encode(kSimplified, "GBK") << int(TextEncoding::Gbk);
        QTest::newRow("big5") << encode(kTraditional, "Big5") << int(TextEncoding::Big5);
        QTest::newRow("windows-1252") << QByteArray("Entaill\xE9 rapide, tr\xE8s fort") << int(TextEncoding::Windows1252);
    }

    void detectsTextEncodings()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, expected);
        QCOMPARE(int(detectTextEncoding(bytes).encoding), expected);
    }

    void decodesEachEncoding()
    {
        QCOMPARE(decodeText(encode(kJapanese, "Shift_JIS"), TextEncoding::ShiftJis), kJapanese);
        QCOMPARE(decodeText(encode(kKorean, "EUC-KR"), TextEncoding::EucKr), kKorean);
        QCOMPARE(decodeText(QByteArray("\x80"), TextEncoding::Windows1252), QString(u"€"));
    }

    void detectsEncodingInsideBinaryData()
    {
        QCOMPARE(int(detectEncodingInBinary(randomBytes(64 * 1024)).encoding), int(TextEncoding::Utf8));
        QVERIFY(!detectEncodingInBinary(randomBytes(64 * 1024)).confident);
        QCOMPARE(int(detectEncodingInBinary(binaryWithStrings({kJapanese.toUtf8(), "Sword"})).encoding),
                 int(TextEncoding::Utf8));
        QCOMPARE(int(detectEncodingInBinary(binaryWithStrings({encode(kJapanese, "Shift_JIS"),
                                                               encode(QStringLiteral("あいうえおのはにを"), "Shift_JIS")}))
                         .encoding),
                 int(TextEncoding::ShiftJis));
        QCOMPARE(int(detectEncodingInBinary(binaryWithStrings({encode(QStringLiteral("Talk to the mayor"), "UTF-16LE"),
                                                               encode(QStringLiteral("Open the door"), "UTF-16LE"),
                                                               encode(QStringLiteral("Welcome to the island"), "UTF-16LE")}))
                         .encoding),
                 int(TextEncoding::Utf16LE));
    }

    void hexViewShowsCharactersAcrossRows()
    {
        QByteArray bytes(14, 'a');
        bytes += QStringLiteral("通常").toUtf8();
        bytes += QByteArray("bc");
        const auto doc = HexReader().read(write("crossing.bin", bytes), nullptr);
        QVERIFY(doc);
        const Table &t = *doc->tables.front();
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("aaaaaaaaaaaaaa通"));
        QCOMPARE(t.text(1, 1, DisplayMode::Auto), QString("常bc"));
        QVERIFY(t.description().contains("UTF-8"));
        QVERIFY(!t.description().contains("detected"));
    }

    void hexViewCanBeReencoded()
    {
        const QByteArray sjis = encode(QStringLiteral("あいう"), "Shift_JIS");
        const auto doc = HexReader().read(write("sjis.bin", sjis), nullptr);
        QVERIFY(doc);
        const auto shown = doc->tables.front()->withTextEncoding(TextEncoding::ShiftJis);
        QVERIFY(shown);
        QCOMPARE(shown->text(0, 1, DisplayMode::Auto), QString("あいう"));
        QVERIFY(shown->description().contains("Shift-JIS"));
    }

    void readsCsvInAnyLanguage()
    {
        QByteArray csv("\xEF\xBB\xBF");
        csv += QStringLiteral("1,ant00,700,CYRANO,さくらじま\r\n2,ant01,\"Сирано, \"\"s\"\"\",\"two\nlines\"\r\n").toUtf8();
        const QString path = write("words.csv", csv);
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("Text"));
        const Table &t = *doc->tables.front();
        QCOMPARE(t.rowCount(), 2);
        QCOMPARE(t.columnCount(), 5);
        QCOMPARE(t.text(0, 4, DisplayMode::Auto), QString("さくらじま"));
        QCOMPARE(t.text(1, 2, DisplayMode::Auto), QString("Сирано, \"s\""));
        QCOMPARE(t.text(1, 3, DisplayMode::Auto), QString("two␊lines"));
        QCOMPARE(t.sortKey(0, 2), QVariant(qint64(700)));
        QVERIFY(t.description().contains("BOM"));
    }

    void readsTextFilesInLegacyEncodings()
    {
        const QByteArray bytes = encode(QStringLiteral("一行目\n二行目のテキスト\n"), "Shift_JIS");
        const auto doc = TextReader::fromBytes(bytes, "notes.txt", nullptr);
        QVERIFY(doc);
        const Table &t = *doc->tables.front();
        QCOMPARE(t.rowCount(), 2);
        QCOMPARE(t.text(1, 0, DisplayMode::Auto), QString("二行目のテキスト"));
        QCOMPARE(t.rowLabel(0), QString("1"));
        QVERIFY(t.description().contains("Shift-JIS"));

        const auto asLatin = t.withTextEncoding(TextEncoding::Windows1252);
        QVERIFY(asLatin);
        QVERIFY(asLatin->text(0, 0, DisplayMode::Auto) != t.text(0, 0, DisplayMode::Auto));
    }

    void binaryIsNotText()
    {
        QString error;
        QVERIFY(!TextReader::fromBytes(randomBytes(4096), "data.txt", &error));
        QVERIFY(!looksLikeText(QByteArray("#TBL\x02\0\0\0", 8)));
        QVERIFY(looksLikeText(QStringLiteral("さくらじま").toUtf8()));
    }

    void tblTextInShiftJis()
    {
        const QByteArray first = encode(QStringLiteral("剣と盾"), "Shift_JIS") + '\0';
        const QByteArray second = encode(QStringLiteral("回復薬"), "Shift_JIS") + '\0';
        QByteArray b(8 + 80 + 16, '\0');
        std::memcpy(b.data(), "#TBL", 4);
        sample::putU32(b, 4, 1);
        sample::putSection(b, 0, "ItemData", 88, 8, 2);
        sample::putU64(b, 88, quint64(b.size()));
        sample::putU64(b, 96, quint64(b.size() + first.size()));
        b += first + second;
        QString error;
        const auto doc = FalcomTblReader::fromBytes(b, &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables.front();
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("剣と盾"));
        QCOMPARE(t.text(1, 0, DisplayMode::Auto), QString("回復薬"));
    }
};

QTEST_GUILESS_MAIN(TestText)
#include "test_text.moc"
