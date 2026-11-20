#include "core/records.h"
#include "core/schema.h"

#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <bit>

namespace {

// 12-byte records, built by hand: u16, i16, f32, u32 pointer-or-number
class FakeRecords : public RecordSource
{
public:
    FakeRecords()
    {
        m_bytes = QByteArray(24, '\0');
        put<quint16>(0, 500);
        put<qint16>(2, -3);
        put<quint32>(4, std::bit_cast<quint32>(2.5f));
        put<quint32>(8, 7);
        put<quint16>(12, 65535);
        put<qint16>(14, 100);
        put<quint32>(16, std::bit_cast<quint32>(-0.5f));
        put<quint32>(20, 9);
    }

    QString recordTypeName() const override { return QStringLiteral("Fake"); }
    int recordSize() const override { return 12; }
    int recordCount() const override { return 2; }
    QByteArrayView record(int row) const override { return QByteArrayView(m_bytes).sliced(row * 12, 12); }
    std::vector<FieldDef> guessFields() const override
    {
        return {{0, FieldType::Auto32, {}, false}, {4, FieldType::Auto32, {}, false}, {8, FieldType::Auto32, {}, false}};
    }

private:
    template <typename T>
    void put(int at, T v) { qToLittleEndian(v, m_bytes.data() + at); }

    QByteArray m_bytes;
};

std::vector<FieldDef> user(std::initializer_list<FieldDef> fields)
{
    std::vector<FieldDef> v(fields);
    for (FieldDef &f : v)
        f.userDefined = true;
    return v;
}

}

class TestRecords : public QObject
{
    Q_OBJECT

private slots:
    void layoutUsesGuessesWhenNothingIsDefined()
    {
        const auto layout = buildLayout(12, {}, FakeRecords().guessFields());
        QCOMPARE(layout.size(), size_t(3));
        QCOMPARE(layout[1].offset, 4);
    }

    void smallerTypeSplitsAColumn()
    {
        // The user says +0x00 is a u16: the guessed 4-byte column there can't be used any more.
        const auto layout = buildLayout(12, user({{0, FieldType::U16, "hp"}}), FakeRecords().guessFields());
        QCOMPARE(layout.size(), size_t(5));
        QCOMPARE(layout[0].name, QString("hp"));
        QVERIFY(layout[0].userDefined);
        QCOMPARE(layout[1].offset, 2);  // leftover bytes become u8 columns
        QCOMPARE(layout[1].type, FieldType::U8);
        QCOMPARE(layout[2].offset, 3);
        QCOMPARE(layout[3].offset, 4);  // the guess at +0x04 still fits
        QCOMPARE(layout[3].type, FieldType::Auto32);
    }

    void largerTypeSwallowsGuesses()
    {
        const auto layout = buildLayout(12, user({{0, FieldType::U64, "big"}}), FakeRecords().guessFields());
        QCOMPARE(layout.size(), size_t(2));
        QCOMPARE(layout[1].offset, 8);
    }

    void layoutCoversEveryByteOnce()
    {
        const auto layout = buildLayout(12, user({{3, FieldType::U32, {}}, {1, FieldType::U16, {}}}),
                                        FakeRecords().guessFields());
        int pos = 0;
        for (const FieldDef &f : layout) {
            QCOMPARE(f.offset, pos);
            pos = f.end();
        }
        QCOMPARE(pos, 12);
    }

    void invalidUserFieldsAreIgnored()
    {
        // Past the end, and overlapping an earlier field.
        const auto layout = buildLayout(12, user({{10, FieldType::U32, "x"}, {0, FieldType::U32, "a"}, {2, FieldType::U16, "b"}}), {});
        for (const FieldDef &f : layout)
            QVERIFY(f.name != "x" && f.name != "b");
    }

    void formatsEachType()
    {
        RecordTable t(std::make_shared<FakeRecords>(),
                      user({{0, FieldType::U16, "u16"}, {2, FieldType::I16, "i16"}, {4, FieldType::F32, "f32"},
                            {8, FieldType::U8, "u8"}}));
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("500"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("-3"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("2.5"));
        QCOMPARE(t.text(0, 3, DisplayMode::Auto), QString("7"));
        QCOMPARE(t.text(1, 0, DisplayMode::Auto), QString("65535"));
        QCOMPARE(t.text(1, 2, DisplayMode::Auto), QString("-0.5"));
        QCOMPARE(t.text(0, 1, DisplayMode::Hex), QString("0xFFFD"));
        QCOMPARE(t.columnName(0), QString("u16"));
        QCOMPARE(t.columnName(4), QString("+0x09 u8"));
        QCOMPARE(t.sortKey(0, 1).toDouble(), -3.0);
    }

    void textIgnoredWhenFormatHasNoTextPointers()
    {
        RecordTable t(std::make_shared<FakeRecords>(), user({{0, FieldType::Text, "name"}}));
        QVERIFY(t.columnName(0) != QString("name"));
    }

    void storeMatchesByFormatTableAndSize()
    {
        SchemaStore s;
        s.setField("Falcom TBL", "Items", 12, {0, FieldType::U16, "hp", true});
        QCOMPARE(s.fields("Falcom TBL", "Items", 12).size(), size_t(1));
        QVERIFY(s.fields("Falcom TBL", "Items", 16).empty());  // same name, other layout
        QVERIFY(s.fields("Other", "Items", 12).empty());
    }

    void storeReplacesOverlappingFields()
    {
        SchemaStore s;
        s.setField("F", "T", 12, {0, FieldType::U16, "a", true});
        s.setField("F", "T", 12, {2, FieldType::U16, "b", true});
        s.setField("F", "T", 12, {0, FieldType::U32, "ab", true});
        const auto fields = s.fields("F", "T", 12);
        QCOMPARE(fields.size(), size_t(1));
        QCOMPARE(fields[0].name, QString("ab"));
        QVERIFY(!s.setField("F", "T", 12, {10, FieldType::U32, "too far", true}));
    }

    void storeRemovesFields()
    {
        SchemaStore s;
        s.setField("F", "T", 12, {0, FieldType::U16, "a", true});
        s.setField("F", "T", 12, {4, FieldType::U16, "b", true});
        s.removeField("F", "T", 12, 0);
        QCOMPARE(s.fields("F", "T", 12).size(), size_t(1));
        s.clearTable("F", "T", 12);
        QVERIFY(s.fields("F", "T", 12).empty());
        QCOMPARE(s.tableCount(), 0);
    }

    void storeSavesAndLoads()
    {
        QTemporaryDir dir;
        SchemaStore s;
        s.setPath(dir.filePath("sub/My Game.json"));
        s.setGameName("My Game");
        s.setField("Falcom TBL", "Items", 12, {8, FieldType::F32, "weight", true});
        s.setField("Falcom TBL", "Items", 12, {0, FieldType::Text, "name", true});
        QString error;
        QVERIFY2(s.save(&error), qPrintable(error));

        SchemaStore loaded;
        QStringList warnings;
        QVERIFY(loaded.load(dir.filePath("sub/My Game.json"), &error, &warnings));
        QVERIFY(warnings.isEmpty());
        QCOMPARE(loaded.gameName(), QString("My Game"));
        const auto fields = loaded.fields("Falcom TBL", "Items", 12);
        QCOMPARE(fields.size(), size_t(2));
        QCOMPARE(fields[0].name, QString("name"));  // sorted by offset
        QCOMPARE(fields[1].type, FieldType::F32);
    }

    void missingFileIsAnEmptyStore()
    {
        SchemaStore s;
        QString error;
        QVERIFY(s.load("/nonexistent/folder/game.json", &error));
        QCOMPARE(s.tableCount(), 0);
    }

    void brokenEntriesAreSkippedWithWarnings()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("g.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"tables": [
            {"format": "F", "table": "T", "recordSize": 8, "fields": [
                {"offset": 0, "type": "u32", "name": "ok"},
                {"offset": 6, "type": "u32", "name": "past the end"},
                {"offset": 4, "type": "banana", "name": "bad type"}]},
            {"table": "no format"}]})");
        f.close();

        SchemaStore s;
        QString error;
        QStringList warnings;
        QVERIFY(s.load(f.fileName(), &error, &warnings));
        QCOMPARE(s.fields("F", "T", 8).size(), size_t(1));
        QCOMPARE(warnings.size(), 3);
    }

    void notJsonIsAnError()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("g.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json at all");
        f.close();
        SchemaStore s;
        QString error;
        QVERIFY(!s.load(f.fileName(), &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_MAIN(TestRecords)
#include "test_records.moc"
