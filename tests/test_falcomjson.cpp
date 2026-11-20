#include "data/falcomjsonreader.h"
#include "core/registry.h"
#include "sample_bjson.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

class TestFalcomJson : public QObject
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

    static std::unique_ptr<Document> read(const QByteArray &bytes, QString *error = nullptr)
    {
        QString e;
        auto doc = FalcomJsonReader::fromBytes(bytes, &e);
        if (error)
            *error = e;
        return doc;
    }

private slots:
    void detectedEvenWithJsonExtension()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        QVERIFY(registry.open(write("scene.json", samplejson::make()), &used, &error));
        QCOMPARE(used->name(), QString("Falcom binary JSON"));
    }

    void flattensToRows()
    {
        const auto doc = read(samplejson::make());
        QVERIFY(doc);
        const Table &t = *doc->tables.at(0);
        QCOMPARE(t.rowCount(), 7);
        QCOMPARE(t.text(0, 0, DisplayMode::Auto), QString("(root)"));
        QCOMPARE(t.text(0, 2, DisplayMode::Auto), QString("{4 keys}"));
        QCOMPARE(t.text(1, 0, DisplayMode::Auto), QString("name"));
        QCOMPARE(t.text(1, 2, DisplayMode::Auto), QString("Hero"));
        QCOMPARE(t.text(2, 2, DisplayMode::Auto), QString("12.5"));
        QCOMPARE(t.text(4, 0, DisplayMode::Auto), QString("tags[0]"));
        QCOMPARE(t.text(4, 2, DisplayMode::Auto), QString("a"));
        QCOMPARE(t.text(5, 1, DisplayMode::Auto), QString("number"));
        QCOMPARE(t.text(6, 2, DisplayMode::Auto), QString("true"));
    }

    void exportsPlainJsonInOriginalOrder()
    {
        const auto doc = read(samplejson::make());
        QVERIFY(doc);
        QVERIFY(doc->tables.at(0)->canExportJson());
        const QByteArray text = doc->tables.at(0)->exportJson();

        QJsonParseError err;
        const QJsonObject o = QJsonDocument::fromJson(text, &err).object();
        QCOMPARE(err.error, QJsonParseError::NoError);
        QCOMPARE(o["name"].toString(), QString("Hero"));
        QCOMPARE(o["hp"].toDouble(), 12.5);
        QCOMPARE(o["tags"].toArray().at(0).toString(), QString("a"));
        QCOMPARE(o["tags"].toArray().at(1).toInt(), 1);
        QCOMPARE(o["alive"].toBool(), true);
        QVERIFY(text.indexOf("\"name\"") < text.indexOf("\"hp\""));
        QVERIFY(text.indexOf("\"hp\"") < text.indexOf("\"tags\""));
        QVERIFY(text.indexOf("\"tags\"") < text.indexOf("\"alive\""));
    }

    void rejectsRootOutsideFile()
    {
        QByteArray b = samplejson::make();
        qToLittleEndian(quint32(b.size() + 10), b.data() + 8);
        QString error;
        QVERIFY(!read(b, &error));
        QVERIFY(error.contains("outside"));
    }

    void rejectsUnknownType()
    {
        samplejson::Offsets o;
        QByteArray b = samplejson::make(&o);
        b[o.aliveNode] = char(0x07);
        QString error;
        QVERIFY(!read(b, &error));
        QVERIFY(error.contains("Unknown"));
    }

    void readsFromDisk()
    {
        QString error;
        QVERIFY(FalcomJsonReader().read(write("test.mi", samplejson::make()), &error));
    }

    void exportKeepsCharactersOutsideTheBmp()
    {
        const char *name = "Hero \xF0\x9F\x98\x80";
        const auto doc = read(samplejson::make(nullptr, name));
        QVERIFY(doc);
        const QByteArray text = doc->tables.at(0)->exportJson();
        QVERIFY(text.contains("\xF0\x9F\x98\x80"));
        QCOMPARE(QJsonDocument::fromJson(text).object()["name"].toString(), QString::fromUtf8(name));
    }

    void stopsAtTheValueLimitBeforeAllocating()
    {
        FalcomJsonLimits limits;
        limits.maxValues = 1000;
        QString error;
        QVERIFY(!FalcomJsonReader::fromBytes(samplejson::makeSelfReferencingList(600), &error, limits));
        QVERIFY2(error.contains("Too many values"), qPrintable(error));
    }

    void survivesLoops()
    {
        samplejson::Offsets o;
        QByteArray b = samplejson::make(&o);
        qToLittleEndian(quint32(o.tagsNode), b.data() + o.tagsNode + 1 + 4 + 4);
        QString error;
        QVERIFY(!read(b, &error));
        QVERIFY(error.contains("nested too deeply"));
    }
};

QTEST_MAIN(TestFalcomJson)
#include "test_falcomjson.moc"
