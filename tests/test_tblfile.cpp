#include "data/falcomtblreader.h"
#include "core/records.h"
#include "data/tblfile.h"

#include "sample_tbl.h"

#include <QTest>


class TestTblFile : public QObject
{
    Q_OBJECT

private slots:
    void readsSections()
    {
        TblFile f;
        QVERIFY(f.load(sample::makeSample()));
        QCOMPARE(f.sections().size(), 2);
        QCOMPARE(f.sections()[0].name, QString("ItemData"));
        QCOMPARE(f.sections()[0].entryCount, 2u);
        QCOMPARE(f.sections()[1].name, QString("Other"));
        QCOMPARE(f.sections()[1].entryCount, 3u);
        QCOMPARE(f.stringPoolOffset(), qsizetype(212));
    }

    void guessesColumns()
    {
        TblFile f;
        QVERIFY(f.load(sample::makeSample()));
        const auto &cols = f.sections()[0].columns;
        QCOMPARE(cols.size(), 3);
        QCOMPARE(cols[0].type, ColumnType::String);
        QCOMPARE(cols[1].type, ColumnType::U32);
        QCOMPARE(cols[1].offset, 8);
        QCOMPARE(cols[2].offset, 12);
        QCOMPARE(f.sections()[1].columns.size(), 1);
    }

    void showsValues()
    {
        QString error;
        const auto doc = FalcomTblReader::fromBytes(sample::makeSample(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &items = *doc->tables.at(0);
        const Table &other = *doc->tables.at(1);
        QCOMPARE(items.text(0, 0, DisplayMode::Auto), QString("Sword"));
        QCOMPARE(items.text(1, 0, DisplayMode::Auto), QString("Shield"));
        QCOMPARE(items.text(0, 1, DisplayMode::Auto), QString("42"));
        QCOMPARE(items.text(1, 1, DisplayMode::Auto), QString("-1"));
        QCOMPARE(items.text(0, 2, DisplayMode::Auto), QString("1.5"));
        QCOMPARE(items.text(1, 2, DisplayMode::Auto), QString("0.25"));
        QCOMPARE(other.text(2, 0, DisplayMode::Auto), QString("0xDEADBEEF"));
        QCOMPARE(items.columnName(0), QString("+0x00 text"));
        QCOMPARE(items.columnName(1), QString("+0x08 auto"));
    }

    void hexMode()
    {
        const auto doc = FalcomTblReader::fromBytes(sample::makeSample(), nullptr);
        QVERIFY(doc);
        QCOMPARE(doc->tables.at(0)->text(0, 1, DisplayMode::Hex), QString("0x0000002A"));
        QCOMPARE(doc->tables.at(0)->text(0, 0, DisplayMode::Hex), QString("0x000000D4"));
    }

    void tablesAreRecordSources()
    {
        const auto doc = FalcomTblReader::fromBytes(sample::makeSample(), nullptr);
        QVERIFY(doc);
        const auto records = doc->tables.at(0)->recordSource();
        QVERIFY(records);
        QCOMPARE(records->recordTypeName(), QString("ItemData"));
        QCOMPARE(records->recordSize(), 16);
        QCOMPARE(records->recordCount(), 2);
    }

    void rejectsWrongMagic()
    {
        QByteArray b = sample::makeSample();
        b[0] = 'X';
        TblFile f;
        QString error;
        QVERIFY(!f.load(b, &error));
        QVERIFY(!error.isEmpty());
    }

    void rejectsSectionOutsideFile()
    {
        QByteArray b = sample::makeSample();
        sample::putU32(b, 8 + 80 + 76, 1000);  // "Other" now claims 1000 entries
        TblFile f;
        QVERIFY(!f.load(b));
    }

    void rejectsImplausibleEntrySize()
    {
        QByteArray b = sample::makeSample();
        sample::putU32(b, 8 + 80 + 72, 200u * 1024 * 1024);  // "Other": 200 MB per entry
        sample::putU32(b, 8 + 80 + 76, 0);                   // ...and no entries, so it fits
        TblFile f;
        QString error;
        QVERIFY(!f.load(b, &error));
        QVERIFY2(error.contains("not plausible"), qPrintable(error));
    }

    void rejectsOverlappingSections()
    {
        QByteArray b = sample::makeSample();
        sample::putU32(b, 8 + 80 + 68, 168);  // "Other" now reuses ItemData's bytes
        sample::putU32(b, 8 + 80 + 72, 16);
        sample::putU32(b, 8 + 80 + 76, 2);
        TblFile f;
        QString error;
        QVERIFY(!f.load(b, &error));
        QVERIFY2(error.contains("overlap"), qPrintable(error));
    }

    void rejectsTruncatedHeader()
    {
        TblFile f;
        QVERIFY(!f.load(QByteArray("#TBL\x05\x00\x00\x00", 8)));
    }
};

QTEST_MAIN(TestTblFile)
#include "test_tblfile.moc"
