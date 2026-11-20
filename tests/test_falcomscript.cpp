#include "data/ed9bytecode.h"
#include "data/falcomscriptreader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QtEndian>

namespace {

void putU32(QByteArray &b, qsizetype pos, quint32 v) { qToLittleEndian(v, b.data() + pos); }

constexpr quint32 kTag = 0xC0000000;
constexpr qsizetype kPool = 0x6B;

void putPush(QByteArray &b, qsizetype pos, quint32 value)
{
    b[pos] = 0x00;
    b[pos + 1] = 4;
    putU32(b, pos + 2, value);
}

QByteArray makeScript()
{
    QByteArray pool;
    pool.append('\0');                      // ""          at kPool
    pool.append("Hi\nthere").append('\0');  // at kPool + 1
    pool.append("Main").append('\0');       // at kPool + 10
    pool.append("Sub").append('\0');        // at kPool + 15

    QByteArray b(kPool, '\0');
    b.replace(0, 4, "#scp");
    putU32(b, 4, 0x18);
    putU32(b, 8, 2);
    putU32(b, 12, 0x58);

    // Function entries
    putU32(b, 0x18, 0x58);
    b[0x18 + 4] = 2;  // inputs
    b[0x18 + 5] = 1;  // flags
    b[0x18 + 7] = 1;  // outputs
    putU32(b, 0x18 + 24, 0x11111111);
    putU32(b, 0x18 + 28, kTag | (kPool + 10));
    putU32(b, 0x38, 0x64);
    putU32(b, 0x38 + 24, 0x22222222);
    putU32(b, 0x38 + 28, kTag | (kPool + 15));

    // Bytecode
    putPush(b, 0x58, kTag | (kPool + 1));
    putPush(b, 0x5E, kTag | kPool);
    putPush(b, 0x64, kTag | 0x67);  // "string at 0x67": inside this very instruction
    b[0x6A] = 0x0D;                 // EXIT

    return b + pool;
}

}

class TestFalcomScript : public QObject
{
    Q_OBJECT

private slots:
    void readsStrings()
    {
        QString error;
        const auto doc = FalcomScriptReader::fromBytes(makeScript(), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(3));
        const Table &t = *doc->tables[0];
        QCOMPARE(t.name(), QString("Strings"));
        QCOMPARE(t.rowCount(), 4);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QStringLiteral("0x6B"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString());
        QCOMPARE(t.text(1, 1, DisplayMode::Auto), QStringLiteral("Hi ↵ there"));
        QCOMPARE(t.text(2, 1, DisplayMode::Auto), QString("Main"));
        QCOMPARE(t.text(2, 2, DisplayMode::Auto), QString("function name"));
        QCOMPARE(t.text(1, 2, DisplayMode::Auto), QString());
    }

    void readsFunctions()
    {
        QString error;
        const auto doc = FalcomScriptReader::fromBytes(makeScript(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables[1];
        QCOMPARE(t.rowCount(), 2);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("Main"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("0x58"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("12"));
        QCOMPARE(t.text(0, 3, DisplayMode::Auto), QString("2"));
        QCOMPARE(t.text(0, 4, DisplayMode::Auto), QString("1"));
        QCOMPARE(t.text(0, 5, DisplayMode::Auto), QString("0x1"));
        QCOMPARE(t.text(1, 2, DisplayMode::Auto), QString("7"));
        QCOMPARE(t.text(1, 6, DisplayMode::Auto), QString("0x22222222"));
        QCOMPARE(t.sortKey(0, 2).toUInt(), 12u);
    }

    void disassemblesCode()
    {
        QString error;
        const auto doc = FalcomScriptReader::fromBytes(makeScript(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables[2];
        QCOMPARE(t.name(), QString("Code"));
        QCOMPARE(t.rowCount(), 4);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("Main"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("0x58"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("PUSH"));
        QCOMPARE(t.text(0, 3, DisplayMode::Auto), QString("\"Hi\\nthere\""));
        QCOMPARE(t.text(1, 3, DisplayMode::Auto), QString("\"\""));
        QCOMPARE(t.text(3, 0, DisplayMode::Auto), QString("Sub"));
        QCOMPARE(t.text(3, 2, DisplayMode::Auto), QString("EXIT"));
        QVERIFY(!t.description().contains("not fully"));
    }

    void decodesInstructions()
    {
        QByteArray code = QByteArray::fromHex("0f34120000" "0c0700" "240513" "03" "010c");
        QCOMPARE(ed9::instructionSize(code, 0), 5);
        QCOMPARE(ed9::decode(code, 0).name, QString("JUMPIFFALSE"));
        QCOMPARE(ed9::decode(code, 0).arguments, QString("→ 0x1234"));
        QCOMPARE(ed9::instructionSize(code, 5), 3);
        QCOMPARE(ed9::decode(code, 5).arguments, QString("function 7"));
        QCOMPARE(ed9::instructionSize(code, 8), 4);
        QCOMPARE(ed9::decode(code, 8).arguments, QString("text_13, 3 args"));
        QCOMPARE(ed9::decode(code, 12).arguments, QString("12"));
        QCOMPARE(ed9::instructionSize(QByteArray::fromHex("29"), 0), -1);
        QCOMPARE(ed9::instructionSize(QByteArray::fromHex("0f3412"), 0), -1);
    }

    void decodesTaggedValues()
    {
        const QByteArray none;
        QCOMPARE(ed9::valueText(none, 0x40000000 | 12), QString("12"));
        QCOMPARE(ed9::valueText(none, 0x7FFFFFFF), QString("-1"));
        QCOMPARE(ed9::valueText(none, 0x80000000 | (0x3FC00000 >> 2)), QString("1.5"));
        QCOMPARE(ed9::valueText(none, 0x5), QString("undefined(0x5)"));
    }

    void exportsStringsAsJson()
    {
        QString error;
        const auto doc = FalcomScriptReader::fromBytes(makeScript(), &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables[0]->canExportJson());
        const QJsonArray strings = QJsonDocument::fromJson(doc->tables[0]->exportJson()).array();
        QCOMPARE(strings.size(), 4);
        QCOMPARE(strings[1].toObject()["text"].toString(), QString("Hi\nthere"));
        QCOMPARE(strings[1].toObject()["offset"].toInt(), int(kPool + 1));
    }

    void rejectsBrokenFiles()
    {
        QString error;
        QVERIFY(!FalcomScriptReader::fromBytes("#scp", &error));
        QByteArray tooMany = makeScript();
        putU32(tooMany, 8, 1000);
        QVERIFY(!FalcomScriptReader::fromBytes(tooMany, &error));
        QByteArray badName = makeScript();
        putU32(badName, 0x18 + 28, 0x40000010);
        QVERIFY(!FalcomScriptReader::fromBytes(badName, &error));
        QVERIFY(error.contains("Function 0"));
    }

    void badBytecodeIsReportedNotRead()
    {
        QByteArray script = makeScript();
        script[0x64] = char(0x7F);
        QString error;
        const auto doc = FalcomScriptReader::fromBytes(script, &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables[2]->description().contains("not fully decoded"));
    }

    void truncatedScriptsNeverCrash()
    {
        const QByteArray full = makeScript();
        for (qsizetype n = 0; n <= full.size(); ++n) {
            QString error;
            if (const auto doc = FalcomScriptReader::fromBytes(full.left(n), &error)) {
                const Table &code = *doc->tables[2];
                for (int r = 0; r < code.rowCount(); ++r)
                    code.text(r, 3, DisplayMode::Auto);
            }
        }
    }
};

QTEST_MAIN(TestFalcomScript)
#include "test_falcomscript.moc"
