#include "data/bcsvreader.h"
#include "data/bymlreader.h"
#include "data/msbtreader.h"
#include "data/restblreader.h"
#include "data/ainbreader.h"
#include "data/evflreader.h"
#include "data/asbreader.h"
#include "data/baevreader.h"
#include "data/terrainreader.h"
#include "archives/compress.h"
#include "textures/texturedecode.h"
#include "data/bymlwriter.h"
#include "data/msbtwriter.h"
#include "core/records.h"
#include "core/registry.h"
#include "core/schema.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

namespace {

class Bytes
{
public:
    explicit Bytes(bool little = true) : m_little(little) {}
    QByteArray b;

    qsizetype pos() const { return b.size(); }
    void align(int n)
    {
        while (b.size() % n)
            b.append('\0');
    }
    void raw(const QByteArray &d) { b += d; }
    void u8(quint8 v) { b.append(char(v)); }
    void u16(quint16 v)
    {
        char p[2];
        m_little ? qToLittleEndian(v, p) : qToBigEndian(v, p);
        b.append(p, 2);
    }
    void u24(quint32 v)
    {
        if (m_little) {
            u8(v & 0xFF), u8((v >> 8) & 0xFF), u8((v >> 16) & 0xFF);
        } else {
            u8((v >> 16) & 0xFF), u8((v >> 8) & 0xFF), u8(v & 0xFF);
        }
    }
    void u32(quint32 v)
    {
        char p[4];
        m_little ? qToLittleEndian(v, p) : qToBigEndian(v, p);
        b.append(p, 4);
    }
    void u64(quint64 v)
    {
        char p[8];
        m_little ? qToLittleEndian(v, p) : qToBigEndian(v, p);
        b.append(p, 8);
    }
    void put32(qsizetype at, quint32 v)
    {
        m_little ? qToLittleEndian(v, b.data() + at) : qToBigEndian(v, b.data() + at);
    }

private:
    bool m_little;
};

void stringTable(Bytes &w, const QStringList &strings)
{
    const qsizetype start = w.pos();
    w.u8(0xC2);
    w.u24(quint32(strings.size()));
    const qsizetype offsets = w.pos();
    for (int i = 0; i <= strings.size(); ++i)
        w.u32(0);
    for (int i = 0; i < strings.size(); ++i) {
        w.put32(offsets + i * 4, quint32(w.pos() - start));
        w.raw(strings[i].toUtf8());
        w.u8(0);
    }
    w.put32(offsets + strings.size() * 4, quint32(w.pos() - start));
    w.align(4);
}

QByteArray makeByml(bool little, bool cycle = false)
{
    Bytes w(little);
    w.raw(little ? "YB" : "BY");
    w.u16(2);
    w.u32(0x10);
    w.u32(0);
    w.u32(0);
    stringTable(w, {"a", "b", "list"});
    w.put32(8, quint32(w.pos()));
    stringTable(w, {"hello"});
    const qsizetype big = w.pos();
    w.u64(quint64(1) << 40);
    const qsizetype array = w.pos();
    w.u8(0xC0);
    w.u24(3);
    w.u8(0xD2);
    w.u8(0xD0);
    w.u8(cycle ? 0xC0 : 0xD5);
    w.align(4);
    float f = 1.5f;
    quint32 bits;
    std::memcpy(&bits, &f, 4);
    w.u32(bits);
    w.u32(1);
    w.u32(quint32(cycle ? array : big));
    const qsizetype root = w.pos();
    w.u8(0xC1);
    w.u24(3);
    w.u24(0), w.u8(0xD1), w.u32(quint32(-5));
    w.u24(1), w.u8(0xA0), w.u32(0);
    w.u24(2), w.u8(0xC0), w.u32(quint32(array));
    w.put32(12, quint32(root));
    return w.b;
}

QByteArray makeMsbt()
{
    Bytes w;
    w.raw("MsgStdBn");
    w.u8(0xFF), w.u8(0xFE);
    w.u16(0);
    w.u8(1), w.u8(3);
    w.u16(3);
    w.u16(0);
    w.u32(0);
    w.raw(QByteArray(10, '\0'));

    auto section = [&](const char *magic, const QByteArray &body) {
        w.raw(magic);
        w.u32(quint32(body.size()));
        w.raw(QByteArray(8, '\0'));
        w.raw(body);
        while (w.b.size() % 16)
            w.b.append(char(0xAB));
    };
    Bytes lbl;
    lbl.u32(1);
    lbl.u32(2);
    lbl.u32(12);
    lbl.u8(5), lbl.raw("Hello"), lbl.u32(1);
    lbl.u8(3), lbl.raw("Bye"), lbl.u32(0);
    section("LBL1", lbl.b);

    Bytes atr;
    atr.u32(2), atr.u32(1), atr.u8(7), atr.u8(9);
    section("ATR1", atr.b);

    auto utf16 = [](Bytes &out, const QString &s) {
        for (QChar c : s)
            out.u16(c.unicode());
    };
    Bytes txt;
    txt.u32(2), txt.u32(12), txt.u32(0);
    utf16(txt, "See you");
    txt.u16(0);
    txt.put32(8, quint32(txt.pos()));
    utf16(txt, "Hi ");
    txt.u16(0x0E), txt.u16(0), txt.u16(3), txt.u16(2), txt.u16(0x0102);
    utf16(txt, "there");
    txt.u16(0x0F), txt.u16(0), txt.u16(3);
    txt.u16(0);
    section("TXT2", txt.b);
    return w.b;
}

QByteArray makeBcsv()
{
    Bytes w;
    w.u32(2);
    w.u32(24);
    w.u16(3);
    w.u8(1), w.u8(1);
    w.raw("VSCB");
    w.u32(30000), w.u32(0), w.u32(0);
    w.u32(BcsvReader::crc32("UniqueID u16")), w.u32(4);
    w.u32(BcsvReader::crc32("Label")), w.u32(6);
    w.u32(BcsvReader::crc32("Flag u8")), w.u32(22);
    const qsizetype data = w.pos();
    for (int r = 0; r < 2; ++r) {
        w.u32(quint32(data + r * 24));
        w.u16(quint16(100 + r));
        QByteArray label = r == 0 ? QByteArray("Spring") : QByteArray("春");
        label.resize(16, '\0');
        w.raw(label);
        w.u8(quint8(r));
        w.u8(0);
    }
    return w.b;
}

QString cell(const Table &t, int row, const QString &column)
{
    for (int c = 0; c < t.columnCount(); ++c)
        if (t.columnName(c) == column)
            return t.text(row, c, DisplayMode::Auto);
    return QStringLiteral("<no column %1>").arg(column);
}

QByteArray makeAinb()
{
    QByteArray b(0x300, '\0');
    auto put32 = [&b](qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); };
    auto put16 = [&b](qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); };
    QByteArray pool("\0", 1);
    auto str = [&pool](const QByteArray &s) {
        const quint32 at = quint32(pool.size());
        pool += s + '\0';
        return at;
    };
    b.replace(0, 4, "AIB ");
    put32(0x04, 0x408);
    put32(0x0C, 1);
    put32(0x10, 2);
    put32(0x20, 0x104);
    put32(0x24, 0x300);
    put32(0x2C, 0x29C);
    put32(0x30, 0x2F0);
    put32(0x34, 0x2C0);
    put32(0x38, 0x2F0);
    put32(0x4C, 0x2F0);
    put32(0x5C, 0x2F0);
    put32(0x60, str("AI"));
    put32(0x74, str("Root"));
    put16(0x74 + 0x14, 1);
    put32(0x8C + 8, str("Attack"));
    put32(0x8C + 0x14, 0x148);
    put16(0xC8, 2);
    put32(0xC8 + 0x14, 0x1EC);
    put16(0x104 + 4 * 8, 1);
    put32(0x104 + 56, str("IsAngry") | 0x80000000u);
    put32(0x104 + 56 + 4, str(""));
    put32(0x104 + 64, 1);
    put32(0x148 + 4, 1);
    b[0x1EC + 0x90 + 2 * 2] = 1;
    put32(0x290, 0x294);
    put32(0x294, 0);
    put32(0x298, str("Go"));
    put32(0x29C, 0x2B4);
    for (int i = 1; i < 6; ++i)
        put32(0x29C + i * 4, 0x2C0);
    put32(0x2B4, str("Speed"));
    put32(0x2BC, 5);
    for (int i = 0; i < 12; ++i)
        put32(0x2C0 + i * 4, 0x2F0);
    put32(0x2F0, 1);
    put32(0x2F4, str("Child.module"));
    put32(0x2F8, str("AI"));
    put32(0x2FC, 1);
    return b + pool;
}

QByteArray makeEventFlow()
{
    QByteArray b(0x48, '\0');
    auto alloc = [&b](qsizetype size) {
        while (b.size() % 8)
            b.append('\0');
        const qsizetype at = b.size();
        b.append(QByteArray(size, '\0'));
        return at;
    };
    auto p64 = [&b](qsizetype at, quint64 v) { qToLittleEndian(v, b.data() + at); };
    auto p32 = [&b](qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); };
    auto p16 = [&b](qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); };
    auto str = [&](const QByteArray &text) {
        const qsizetype at = alloc(2 + text.size() + 1);
        p16(at, quint16(text.size()));
        b.replace(at + 2, text.size(), text);
        return quint64(at);
    };
    b.replace(0, 8, QByteArray("BFEVFL\0\0", 8));
    p16(8, 0x400);
    p16(0x0C, 0xFEFF);
    p16(0x20, 1);
    const qsizetype list = alloc(8);
    p64(0x28, quint64(list));
    const qsizetype chart = alloc(0x48);
    p64(list, quint64(chart));
    b.replace(chart, 4, "EVFL");
    p16(chart + 0x10, 1);
    p16(chart + 0x12, 1);
    p16(chart + 0x14, 1);
    p16(chart + 0x16, 2);
    p16(chart + 0x18, 1);
    p64(chart + 0x20, str("Talk_Npc"));
    const qsizetype actor = alloc(0x38);
    const qsizetype events = alloc(0x60);
    p64(chart + 0x28, quint64(actor));
    p64(chart + 0x30, quint64(events));
    p64(actor, str("Npc"));
    const qsizetype actions = alloc(8), queries = alloc(8);
    p64(actions, str("Talk"));
    p64(queries, str("IsHappy"));
    p64(actor + 0x18, quint64(actions));
    p64(actor + 0x20, quint64(queries));
    p16(actor + 0x30, 1);
    p16(actor + 0x32, 1);
    p64(events, str("Ask"));
    b[events + 8] = 1;
    p16(events + 0x0A, 2);
    const qsizetype cases = alloc(16);
    p32(cases, 0);
    p16(cases + 4, 1);
    p32(cases + 8, 1);
    p16(cases + 12, 1);
    p64(events + 0x18, quint64(cases));
    const qsizetype say = events + 0x30;
    p64(say, str("Say"));
    p16(say + 0x0A, 0xFFFF);
    const qsizetype params = alloc(0x18), dic = alloc(0x28), item = alloc(0x18);
    b[params] = 1;
    p16(params + 2, 1);
    p64(params + 8, quint64(dic));
    p64(params + 0x10, quint64(item));
    b.replace(dic, 4, "DIC ");
    p32(dic + 4, 1);
    p64(dic + 0x20, str("Text"));
    b[item] = 5;
    p64(item + 0x10, str("Hi"));
    p64(say + 0x10, quint64(params));
    const qsizetype entries = alloc(0x28), entry = alloc(0x20);
    b.replace(entries, 4, "DIC ");
    p32(entries + 4, 1);
    p64(entries + 0x20, str("Start"));
    p64(chart + 0x38, quint64(entries));
    p64(chart + 0x40, quint64(entry));
    return b;
}

QByteArray makeAsb()
{
    QByteArray b(0x150, '\0');
    auto put32 = [&b](qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); };
    auto put16 = [&b](qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); };
    const QByteArray pool("Npc.root\0Wait\0Npc_Wait_Loop\0", 28);
    b.replace(0, 4, "ASB ");
    put32(0x04, 0x41B);
    put32(0x08, 0);
    put32(0x0C, 1);
    put32(0x10, 0x80);
    put32(0x14, 2);
    put32(0x18, 0xB0);
    put32(0x3C, 0x150);
    put32(0x80, 9);
    put16(0x80 + 0x2C, 1);
    put16(0xB0, 4);
    put16(0xD4, 3);
    put32(0xD4 + 8, 0x100);
    put32(0x104, 14);
    return b + pool;
}

QByteArray makeBaev()
{
    QByteArray b(0x200, '\0');
    auto p64 = [&b](qsizetype at, quint64 v) { qToLittleEndian(v, b.data() + at); };
    auto p32 = [&b](qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); };
    auto array = [&](qsizetype at, quint64 to, quint32 count, quint32 stride) {
        p64(at, to);
        p32(at + 8, count);
        p32(at + 12, stride);
    };
    b.replace(0, 4, "BFFH");
    array(0x10, 0xA8, 1, 0x28);
    b.replace(0x28, 23, "Nintendo.AnimationEvent");
    b.replace(0xA8, 4, "BFSI");
    p64(0xB8, 0xD0);
    b[0xD8] = 0;
    b[0xD9] = 1;
    b[0xDA] = 2;
    array(0xD0 + 0x18, 0x110, 1, 0x18);
    array(0xD0 + 0x28, 0x130, 1, 0x18);
    p32(0x110, BaevReader::fnv1a("Walk"));
    array(0x118, 0x128, 1, 4);
    array(0x130, 0x150, 1, 0x30);
    p64(0x150, 0x1E0);
    array(0x158, 0x180, 1, 0x18);
    array(0x180, 0x1A0, 1, 8);
    const float frame = 12.f;
    quint32 bits;
    std::memcpy(&bits, &frame, 4);
    p32(0x190, bits);
    p64(0x1A0, 0x1B0);
    p32(0x1B0, 5);
    p64(0x1B8, 0x1E8);
    b.replace(0x1E0, 5, QByteArray("Step\0", 5));
    b.replace(0x1E8, 5, QByteArray("Foot\0", 5));
    return b;
}

QByteArray makeTerrainTile(const std::vector<QByteArray> &layers, int grid)
{
    QByteArray tiled;
    for (const QByteArray &linear : layers) {
        QByteArray swizzled = *swizzleBlockLinear(linear, grid, grid, 4, defaultBlockHeight(grid));
        swapGobSectors(swizzled);
        tiled += swizzled;
    }
    QByteArray payload(0x34, '\0');
    payload[0] = char(grid >> 8);
    payload[1] = char(grid & 0xFF);
    payload[2] = 2;
    payload[3] = char(layers.size());
    payload[8] = char(0x81);
    qToLittleEndian(qint32(8), payload.data() + 0x2C);
    qToLittleEndian(qint32(tiled.size()), payload.data() + 0x30);
    payload += tiled;
    QString error;
    const QByteArray packed = *compressZstd(payload, 3, false, &error);
    QByteArray file(0x14, '\0');
    file.replace(0, 4, "TARB");
    qToLittleEndian(quint16(0x14), file.data() + 0x0A);
    qToLittleEndian(quint32(payload.size()), file.data() + 0x0C);
    qToLittleEndian(quint32(packed.size()), file.data() + 0x10);
    return file + packed;
}

QStringList bymlRows(const QByteArray &data)
{
    QString error;
    const auto doc = BymlReader().readBytes(data, "Data.byml", &error);
    if (!doc)
        return {"ERROR " + error};
    const Table &t = *doc->tables.front();
    QStringList rows;
    for (int r = 0; r < t.rowCount(); ++r)
        rows << t.text(r, 0, DisplayMode::Auto) + '|' + t.text(r, 1, DisplayMode::Auto) + '|' + t.text(r, 2, DisplayMode::Auto);
    return rows;
}

}

class TestGameData : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { BcsvReader::setHashNames({}); }

    void readsBymlInBothByteOrders()
    {
        for (bool little : {true, false}) {
            QString error;
            const auto json = BymlReader::toJson(makeByml(little), &error);
            QVERIFY2(json, qPrintable(error));
            const QJsonObject o = json->toObject();
            QCOMPARE(o["a"].toInt(), -5);
            QCOMPARE(o["b"].toString(), QString("hello"));
            const QJsonArray list = o["list"].toArray();
            QCOMPARE(list.size(), 3);
            QCOMPARE(list[0].toDouble(), 1.5);
            QCOMPARE(list[1].toBool(), true);
            QCOMPARE(list[2].toInteger(), qint64(1) << 40);
        }
    }

    void showsBymlAsRows()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(makeByml(true), "Data.byml", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), BymlReader().name());
        const Table &t = *doc->tables.front();
        QStringList paths;
        for (int r = 0; r < t.rowCount(); ++r)
            paths << t.text(r, 0, DisplayMode::Auto) + '=' + t.text(r, 1, DisplayMode::Auto);
        QVERIFY(paths.contains("a=int"));
        QVERIFY(paths.contains("list[2]=uint64"));
        QVERIFY(t.canExportJson());
        QCOMPARE(QJsonDocument::fromJson(t.exportJson()).object()["b"].toString(), QString("hello"));
    }

    void hexModeShowsIntegersInHex()
    {
        const auto doc = BymlReader().readBytes(makeByml(true), "Data.byml", nullptr);
        QVERIFY(doc);
        const Table &t = *doc->tables.front();
        QString intHex, floatHex;
        for (int r = 0; r < t.rowCount(); ++r) {
            if (t.text(r, 0, DisplayMode::Auto) == "a")
                intHex = t.text(r, 2, DisplayMode::Hex);
            if (t.text(r, 0, DisplayMode::Auto) == "list[0]")
                floatHex = t.text(r, 2, DisplayMode::Hex);
        }
        QCOMPARE(intHex, QString("0xFFFFFFFB"));
        QCOMPARE(floatHex, QString("1.5"));
    }

    void stopsOnCyclesAndDamage()
    {
        QString error;
        QVERIFY(!BymlReader::toJson(makeByml(true, true), &error));
        QVERIFY(error.contains("deeply"));
        const QByteArray data = makeByml(false);
        for (qsizetype n = 16; n < data.size(); ++n)
            BymlReader().readBytes(data.left(n), "cut.byml", &error);
    }

    void readsMsbtMessagesLabelsAndTags()
    {
        QString error;
        const auto messages = MsbtReader::parse(makeMsbt(), &error);
        QVERIFY2(messages, qPrintable(error));
        QCOMPARE(messages->size(), size_t(2));
        QCOMPARE(messages->at(0).label, QString("Bye"));
        QCOMPARE(messages->at(0).text, QString("See you"));
        QCOMPARE(messages->at(1).label, QString("Hello"));
        QCOMPARE(messages->at(1).text, QString("Hi [0:3 0201]there[/0:3]"));
        QCOMPARE(messages->at(1).attribute, QByteArray("\x09", 1));

        const auto doc = MsbtReader().readBytes(makeMsbt(), "Talk.msbt", &error);
        QVERIFY(doc);
        const QJsonObject json = QJsonDocument::fromJson(doc->tables.front()->exportJson()).object();
        QCOMPARE(json["Bye"].toString(), QString("See you"));
        const QByteArray data = makeMsbt();
        for (qsizetype n = 0; n < data.size(); n += 3)
            MsbtReader::parse(data.left(n), &error);
    }

    void writesEditedBymlWithTheGamesTypes()
    {
        for (bool little : {true, false}) {
            const QByteArray original = makeByml(little);
            QString error;
            QJsonObject o = BymlReader::toJson(original, &error)->toObject();
            const auto same = bymlFromJson(o, original, &error);
            QVERIFY2(same, qPrintable(error));
            QCOMPARE(same->left(2), original.left(2));
            QCOMPARE(bymlRows(*same), bymlRows(original));

            o["a"] = 7;
            QJsonArray list = o["list"].toArray();
            list[0] = 2;
            list[2] = QString("18446744073709551615");
            list.append(QString("new"));
            o["list"] = list;
            o["c"] = 4.5;
            const auto edited = bymlFromJson(o, original, &error);
            QVERIFY2(edited, qPrintable(error));
            const QStringList rows = bymlRows(*edited);
            QVERIFY2(rows.contains("a|int|7"), qPrintable(rows.join("; ")));
            QVERIFY2(rows.contains("list[0]|float|2"), qPrintable(rows.join("; ")));
            QVERIFY2(rows.contains("list[2]|uint64|18446744073709551615"), qPrintable(rows.join("; ")));
            QVERIFY2(rows.contains("list[3]|string|new"), qPrintable(rows.join("; ")));
            QVERIFY2(rows.contains("c|float|4.5"), qPrintable(rows.join("; ")));
            QVERIFY2(rows.contains("b|string|hello"), qPrintable(rows.join("; ")));

            QJsonObject wrong = o;
            wrong["a"] = 2.5;
            QVERIFY(!bymlFromJson(wrong, original, &error));
            QVERIFY2(error.startsWith("a:"), qPrintable(error));
            wrong = o;
            wrong["list"] = QString("not a list");
            QVERIFY(!bymlFromJson(wrong, original, &error));
            QVERIFY2(error.startsWith("list:"), qPrintable(error));
        }
    }

    void writesEditedMsbtText()
    {
        QByteArray original = makeMsbt();
        qToLittleEndian(quint32(original.size()), original.data() + 0x12);
        QString error;
        const auto doc = MsbtReader().readBytes(original, "Talk.msbt", &error);
        QJsonObject json = QJsonDocument::fromJson(doc->tables.front()->exportJson()).object();
        const auto same = msbtFromJson(json, original, &error);
        QVERIFY2(same, qPrintable(error));
        QCOMPARE(*same, original);

        json["Bye"] = QStringLiteral("Farewell [page]and [0:3 0a0b]good luck[/0:3] ñ");
        const auto edited = msbtFromJson(json, original, &error);
        QVERIFY2(edited, qPrintable(error));
        const auto messages = MsbtReader::parse(*edited, &error);
        QVERIFY2(messages, qPrintable(error));
        QCOMPARE(messages->at(0).text, json["Bye"].toString());
        QCOMPARE(messages->at(1).text, QString("Hi [0:3 0201]there[/0:3]"));
        QCOMPARE(messages->at(1).attribute, QByteArray("\x09", 1));
        QCOMPARE(qFromLittleEndian<quint32>(edited->constData() + 0x12), quint32(edited->size()));
        QCOMPARE(edited->size() % 16, 0);

        json["Unknown"] = QStringLiteral("x");
        QVERIFY(!msbtFromJson(json, original, &error));
        QVERIFY2(error.contains("Unknown"), qPrintable(error));
        json.remove("Unknown");
        json["Bye"] = QStringLiteral("[1:2 abc]");
        QVERIFY(!msbtFromJson(json, original, &error));
    }

    void readsAiNodes()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(makeAinb(), "Enemy.root.ainb", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), AinbReader().name());
        QCOMPARE(doc->tables.size(), size_t(4));
        const Table &nodes = *doc->tables[0];
        QCOMPARE(nodes.rowCount(), 2);
        QCOMPARE(nodes.text(0, 1, DisplayMode::Auto), QString("Attack"));
        QCOMPARE(nodes.text(0, 2, DisplayMode::Auto), QString("Speed=5"));
        QCOMPARE(nodes.text(1, 1, DisplayMode::Auto), QString("Sequential"));
        QCOMPARE(nodes.text(1, 5, DisplayMode::Auto), QString("child → #0 Attack (Go)"));
        QVERIFY(!nodes.description().contains("layout"));
        const Table &commands = *doc->tables[1];
        QCOMPARE(commands.text(0, 0, DisplayMode::Auto), QString("Root"));
        QCOMPARE(commands.text(0, 1, DisplayMode::Auto), QString("#1 Sequential"));
        const Table &globals = *doc->tables[2];
        QCOMPARE(globals.text(0, 0, DisplayMode::Auto), QString("IsAngry"));
        QCOMPARE(globals.text(0, 1, DisplayMode::Auto), QString("bool"));
        QCOMPARE(globals.text(0, 2, DisplayMode::Auto), QString("true"));
        QCOMPARE(doc->tables[3]->text(0, 0, DisplayMode::Auto), QString("Child.module"));
        const QJsonObject json = QJsonDocument::fromJson(nodes.exportJson()).object();
        QCOMPARE(json["nodes"].toArray()[0].toObject()["parameters"].toObject()["Speed"].toInt(), 5);
        const QByteArray data = makeAinb();
        for (qsizetype n = 0; n < data.size(); n += 7)
            AinbReader().readBytes(data.left(n), "cut.ainb", &error);
    }

    void readsEventFlows()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(makeEventFlow(), "Talk_Npc.bfevfl", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), EvflReader().name());
        const Table &events = *doc->tables[0];
        QCOMPARE(events.rowCount(), 2);
        QCOMPARE(events.text(0, 2, DisplayMode::Auto), QString("switch"));
        QCOMPARE(events.text(0, 3, DisplayMode::Auto), QString("Npc"));
        QCOMPARE(events.text(0, 4, DisplayMode::Auto), QString("IsHappy"));
        QCOMPARE(events.text(0, 6, DisplayMode::Auto), QString("0 → #1 Say; 1 → #1 Say"));
        QCOMPARE(events.text(1, 4, DisplayMode::Auto), QString("Talk"));
        QCOMPARE(events.text(1, 5, DisplayMode::Auto), QString("Text=\"Hi\""));
        QCOMPARE(events.text(1, 6, DisplayMode::Auto), QString("end"));
        const Table &entries = *doc->tables[1];
        QCOMPARE(entries.text(0, 0, DisplayMode::Auto), QString("Start"));
        QCOMPARE(entries.text(0, 1, DisplayMode::Auto), QString("#0 Ask"));
        const QJsonObject json = QJsonDocument::fromJson(events.exportJson()).object();
        QCOMPARE(json["events"].toArray()[1].toObject()["params"].toObject()["Text"].toString(), QString("Hi"));
        const QByteArray data = makeEventFlow();
        for (qsizetype n = 0; n < data.size(); n += 5)
            EvflReader().readBytes(data.left(n), "cut.bfevfl", &error);
    }

    void readsAnimationSequences()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(makeAsb(), "Npc.root.asb", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), AsbReader().name());
        const Table &commands = *doc->tables[0];
        QCOMPARE(commands.text(0, 0, DisplayMode::Auto), QString("Wait"));
        QCOMPARE(commands.text(0, 1, DisplayMode::Auto), QString("#1 SkeletalAnimation (Npc_Wait_Loop)"));
        const Table &animations = *doc->tables[1];
        QCOMPARE(animations.rowCount(), 1);
        QCOMPARE(animations.text(0, 0, DisplayMode::Auto), QString("Npc_Wait_Loop"));
        const Table &nodes = *doc->tables[2];
        QCOMPARE(nodes.text(0, 1, DisplayMode::Auto), QString("State"));
        const QByteArray data = makeAsb();
        for (qsizetype n = 0; n < data.size(); n += 9)
            AsbReader().readBytes(data.left(n), "cut.asb", &error);
    }

    void readsAnimationEvents()
    {
        QCOMPARE(BaevReader::fnv1a("C01-Enemy_DungeonBoss_Ganon-A-0"), 0x615bb373u);
        QString error;
        const QHash<quint32, QString> names{{BaevReader::fnv1a("Walk"), "Walk"}};
        const auto named = BaevReader::readWithNames(makeBaev(), names, &error);
        QVERIFY2(named, qPrintable(error));
        const Table &t = *named->tables.front();
        QCOMPARE(t.rowCount(), 1);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("Walk"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("Step"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("trigger"));
        QCOMPARE(t.text(0, 3, DisplayMode::Auto), QString("12"));
        QCOMPARE(t.text(0, 5, DisplayMode::Auto), QString("\"Foot\""));
        QVERIFY(t.description().contains("2.1.0"));

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        const auto doc = registry.openBytes(makeBaev(), "Npc.root.baev", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), BaevReader().name());
        QCOMPARE(doc->tables.front()->text(0, 0, DisplayMode::Auto), QStringLiteral("0x%1").arg(BaevReader::fnv1a("Walk"), 8, 16, QLatin1Char('0')));
        const QByteArray data = makeBaev();
        for (qsizetype n = 0; n < data.size(); n += 11)
            BaevReader::readWithNames(data.left(n), {}, &error);
    }

    void readsTerrainTiles()
    {
        const int grid = 8;
        QByteArray heights(grid * grid * 4, '\0');
        for (int i = 0; i < grid * grid; ++i) {
            const float h = float(i) / 100.f;
            quint32 bits;
            std::memcpy(&bits, &h, 4);
            qToLittleEndian(bits, heights.data() + i * 4);
        }
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(makeTerrainTile({heights}, grid), "0100000000.hght", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), TerrainReader().name());
        const Table &t = *doc->tables.front();
        QCOMPARE(t.name(), QString("Height map"));
        const auto image = t.image();
        QVERIFY(image);
        QCOMPARE(image->width, grid);
        QCOMPARE(uchar(image->rgba[0]), uchar(0));
        QCOMPARE(uchar(image->rgba[(grid * grid - 1) * 4]), uchar(255));
        QVERIFY(t.text(4, 1, DisplayMode::Auto).startsWith("0 – 0.63"));
        const QJsonObject json = QJsonDocument::fromJson(t.exportJson()).object();
        QCOMPARE(json["heights"].toArray().size(), grid * grid);

        QByteArray indices(grid * grid * 4, char(127)), weights(grid * grid * 4, '\0');
        for (int i = 0; i < grid * grid; ++i) {
            indices[i * 4] = char(i < 32 ? 3 : 9);
            weights[i * 4] = char(255);
        }
        const auto mate = registry.openBytes(makeTerrainTile({indices, weights}, grid), "0100000000.adult.mate", &used, &error);
        QVERIFY2(mate, qPrintable(error));
        QCOMPARE(mate->tables.size(), size_t(1));
        QCOMPARE(mate->tables.front()->text(4, 1, DisplayMode::Auto), QString("3, 9"));
        QVERIFY(mate->tables.front()->image());
        QVERIFY(!parseTerrainTile(QByteArray("TARB"), &error));
    }

    void readsResourceSizeTables()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("romfs/Pack") && QDir(dir.path()).mkpath("romfs/System/Resource")
                && QDir(dir.path()).mkpath("romfs/Actor"));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(dir.filePath("romfs/Pack/ZsDic.pack.zs"), "not a pack"));
        QVERIFY(write(dir.filePath("romfs/Actor/Link.bgyml.zs"), "x"));

        QByteArray table("RESTBL", 6);
        auto u32 = [&table](quint32 v) {
            char b[4];
            qToLittleEndian(v, b);
            table.append(b, 4);
        };
        u32(1);
        u32(16);
        u32(2);
        u32(1);
        u32(restblPathHash("Actor/Link.bgyml"));
        u32(1234);
        u32(0xDEADBEEF);
        u32(99);
        QByteArray name("Tex/Long.bntx");
        name.resize(16, '\0');
        table += name;
        u32(5678);
        const QString path = dir.filePath("romfs/System/Resource/ResourceSizeTable.Product.rsizetable");
        QVERIFY(write(path, table));

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.open(path, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), RestblReader().name());
        const Table &t = *doc->tables.front();
        QCOMPARE(t.rowCount(), 3);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("Actor/Link.bgyml"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("1234"));
        QCOMPARE(t.text(1, 0, DisplayMode::Auto), QString());
        QCOMPARE(t.text(2, 0, DisplayMode::Auto), QString("Tex/Long.bntx"));
        QCOMPARE(t.text(2, 2, DisplayMode::Auto), QString("5678"));
        QVERIFY(!parseRestbl(table.left(table.size() - 1), &error));
    }

    void computesStandardCrc32()
    {
        QCOMPARE(BcsvReader::crc32("123456789"), 0xCBF43926u);
        QCOMPARE(BcsvReader::crc32("UniqueID u16"), 0x54706054u);
    }

    void readsBcsvWithHashedColumns()
    {
        QString error;
        const auto doc = BcsvReader().readBytes(makeBcsv(), "Season.bcsv", &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables.front();
        QCOMPARE(t.name(), QString("Season"));
        QCOMPARE(t.rowCount(), 2);
        const QString uniqueHash = QStringLiteral("0x54706054");
        QCOMPARE(cell(t, 1, uniqueHash), QString("101"));
        QCOMPARE(cell(t, 0, "(record offset)"), QString::number(0x1C + 24));
        const QString labelHash = QStringLiteral("0x") + QString::number(BcsvReader::crc32("Label"), 16).toUpper();
        QCOMPARE(cell(t, 0, labelHash), QString("Spring"));
        QCOMPARE(cell(t, 1, labelHash), QString("春"));
    }

    void namesColumnsFromAHashList()
    {
        BcsvReader::setHashNames({"# comment", "UniqueID u16", "Label", "Flag"});
        const auto doc = BcsvReader().readBytes(makeBcsv(), "Season.bcsv", nullptr);
        QVERIFY(doc);
        const auto &t = static_cast<const RecordTable &>(*doc->tables.front());
        QCOMPARE(cell(t, 1, "UniqueID"), QString("101"));
        QCOMPARE(cell(t, 0, "Label"), QString("Spring"));
        QCOMPARE(cell(t, 1, "Flag"), QString("1"));
        for (const FieldDef &f : t.layout())
            if (f.name == "UniqueID")
                QCOMPARE(f.type, FieldType::U16);
        QVERIFY(t.description().contains("3 of 3"));
    }

    void savesInlineTextColumns()
    {
        QTemporaryDir dir;
        SchemaStore store;
        store.setPath(dir.filePath("game.json"));
        FieldDef label{6, FieldType::Chars, "Label", true, 16};
        QVERIFY(store.setField("BCSV table", "Season", 24, label));
        QVERIFY(store.save(nullptr));
        SchemaStore loaded;
        QVERIFY(loaded.load(dir.filePath("game.json"), nullptr));
        const auto fields = loaded.fields("BCSV table", "Season", 24);
        QCOMPARE(fields.size(), size_t(1));
        QCOMPARE(fields[0].length(), 16);
        QCOMPARE(fields[0].end(), 22);
    }
};

QTEST_GUILESS_MAIN(TestGameData)
#include "test_gamedata.moc"
