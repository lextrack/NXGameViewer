#include "data/falcomtblreader.h"
#include "data/hexreader.h"
#include "data/jsonreader.h"
#include "core/registry.h"
#include "core/simpletable.h"
#include "sample_tbl.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

class ThrowingReader : public FormatReader
{
public:
    QString name() const override { return QStringLiteral("Throws"); }
    int score(const QString &, QByteArrayView) const override { return 1; }
    std::unique_ptr<Document> read(const QString &, QString *) const override { throw std::bad_alloc(); }
};

}

class TestFormats : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString write(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
            qFatal("cannot write test file");
        return path;
    }

    QString openWith(const QString &path, std::unique_ptr<Document> &doc)
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        doc = registry.open(path, &used, &error);
        return used ? used->name() : QString();
    }

private slots:
    void detectsTblByContentNotName()
    {
        std::unique_ptr<Document> doc;
        QCOMPARE(openWith(write("whatever.bin", sample::makeSample()), doc), QString("Falcom TBL"));
        QCOMPARE(doc->tables.size(), size_t(2));
        QCOMPARE(doc->tables[0]->name(), QString("ItemData"));
        QCOMPARE(doc->tables[0]->text(1, 0, DisplayMode::Auto), QString("Shield"));
    }

    void readsJson()
    {
        std::unique_ptr<Document> doc;
        const QString path = write("data.json", R"({"a": 1, "b": {"c": "x"}, "d": [true, null]})");
        QCOMPARE(openWith(path, doc), QString("JSON"));

        const Table &t = *doc->tables.at(0);
        QCOMPARE(t.rowCount(), 7);
        QCOMPARE(t.text(3, 0, DisplayMode::Auto), QString("b.c"));
        QCOMPARE(t.text(3, 2, DisplayMode::Auto), QString("x"));
        QCOMPARE(t.text(5, 0, DisplayMode::Auto), QString("d[0]"));
        QCOMPARE(t.text(5, 2, DisplayMode::Auto), QString("true"));
    }

    void brokenJsonFallsBackToText()
    {
        std::unique_ptr<Document> doc;
        QCOMPARE(openWith(write("broken.json", "{ not json"), doc), QString("Text"));
    }

    void unknownFileOpensAsHex()
    {
        std::unique_ptr<Document> doc;
        QCOMPARE(openWith(write("model.xyz", QByteArray("ABC\x01", 4)), doc), QString("Hex"));
        const Table &t = *doc->tables.at(0);
        QCOMPARE(t.rowCount(), 1);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("41 42 43 01"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("ABC."));
        QCOMPARE(t.rowLabel(0), QString("0x00000000"));
    }

    void hexSplitsRowsOf16()
    {
        HexReader reader;
        QString error;
        const auto doc = reader.read(write("big.bin", QByteArray(40, 'z')), &error);
        QVERIFY(doc);
        QCOMPARE(doc->tables.at(0)->rowCount(), 3);
        QCOMPARE(doc->tables.at(0)->rowLabel(2), QString("0x00000020"));
    }

    void readerExceptionsBecomeErrors()
    {
        QString error;
        QVERIFY(!FormatRegistry::readWith(ThrowingReader(), write("x.bin", "x"), &error));
        QVERIFY2(error.contains("memory"), qPrintable(error));
    }

    void negativeNumbersInHexKeepTheirSign()
    {
        const SimpleTable table(QStringLiteral("t"), {{QStringLiteral("value")}},
                                {{-1}, {qint64(-255)}, {255}, {quint32(0xFFFFFFFF)}});
        QCOMPARE(table.text(0, 0, DisplayMode::Hex), QString("-0x1"));
        QCOMPARE(table.text(1, 0, DisplayMode::Hex), QString("-0xFF"));
        QCOMPARE(table.text(2, 0, DisplayMode::Hex), QString("0xFF"));
        QCOMPARE(table.text(3, 0, DisplayMode::Hex), QString("0xFFFFFFFF"));
        QCOMPARE(table.text(0, 0, DisplayMode::Auto), QString("-1"));
    }

    void anyFileCanBeForcedToHex()
    {
        HexReader reader;
        QString error;
        QVERIFY(reader.read(write("table.tbl", sample::makeSample()), &error));
    }
};

QTEST_MAIN(TestFormats)
#include "test_formats.moc"
