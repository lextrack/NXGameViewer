#include "core/batchexport.h"
#include "sample_bjson.h"
#include "sample_mdl.h"
#include "sample_sarc.h"
#include "sample_tbl.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList writtenUnder(const ExtractReport &report, const QString &folder)
{
    QStringList written;
    for (const BatchFileResult &r : report.results)
        for (const QString &w : r.written)
            written << QDir(folder).relativeFilePath(w);
    written.sort();
    return written;
}

}

class TestBatch : public QObject
{
    Q_OBJECT

private slots:
    void collectsFilesFromFoldersOnce()
    {
        QTemporaryDir dir;
        QVERIFY(writeFile(dir.filePath("a.tbl"), "x"));
        QVERIFY(writeFile(dir.filePath("sub/b.tbl"), "x"));
        QVERIFY(writeFile(dir.filePath("sub/deeper/c.tbl"), "x"));
        const QStringList files = collectBatchFiles({dir.filePath("sub"), dir.filePath("sub/b.tbl"), dir.filePath("a.tbl"),
                                                     dir.filePath("missing")});
        QCOMPARE(files, (QStringList{dir.filePath("sub/b.tbl"), dir.filePath("sub/deeper/c.tbl"), dir.filePath("a.tbl")}));
    }

    void findsTheCommonParentOfASelection()
    {
        QCOMPARE(commonParentFolder({"/g/a/x.tbl"}), QString("/g/a"));
        QCOMPARE(commonParentFolder({"/g/a/x.tbl", "/g/a/y.tbl"}), QString("/g/a"));
        QCOMPARE(commonParentFolder({"/g/a/x.tbl", "/g/b/c/y.tbl"}), QString("/g"));
        QCOMPARE(commonParentFolder({"/g/a"}), QString("/g"));
        QCOMPARE(commonParentFolder({"/a/x", "/b/y"}), QString("/"));
        QCOMPARE(commonParentFolder({"/out/m/a.glb", "/out/m/a.csv"}), QString("/out/m"));
        QCOMPARE(commonParentFolder({"/out/m/a.glb", "/out/n/b.png"}), QString("/out"));
    }

    void singleFilesGoStraightIntoTheOutputFolder()
    {
        BatchOptions options;
        options.outputFolder = "/out";
        options.baseFolder = commonParentFolder({"/game/asset/model/hero.mdl"});
        QCOMPARE(batchOutputBases({"/game/asset/model/hero.mdl"}, options), QStringList{"/out/hero"});
        options.baseFolder = commonParentFolder({"/game/asset/model"});
        QCOMPARE(batchOutputBases({"/game/asset/model/a/hero.mdl"}, options), QStringList{"/out/model/a/hero"});
    }

    void plansOutputNamesWithoutClashes()
    {
        BatchOptions options;
        options.baseFolder = "/game";
        options.outputFolder = "/out";
        const QStringList files = {"/game/a/x.tbl", "/game/a/x.mdl", "/game/a/y.tbl", "/game/b/y.tbl", "/elsewhere/z.tbl"};
        QCOMPARE(batchOutputBases(files, options),
                 (QStringList{"/out/a/x.tbl", "/out/a/x.mdl", "/out/a/y", "/out/b/y", "/out/z"}));

        options.keepFolders = false;
        QCOMPARE(batchOutputBases(files, options),
                 (QStringList{"/out/x.tbl", "/out/x.mdl", "/out/y.tbl", "/out/y.tbl_2", "/out/z"}));
    }

    void exportsEachKindOfFile()
    {
        QTemporaryDir dir;
        const QString game = dir.filePath("game"), out = dir.filePath("out");
        QVERIFY(writeFile(game + "/models/hero.mdl", sample::makeRichModel()));
        QVERIFY(writeFile(game + "/data/items.tbl", sample::makeSample()));
        QVERIFY(writeFile(game + "/data/hero.json", samplejson::make()));
        QVERIFY(writeFile(game + "/data/junk.bin", QByteArray("\x00\x01\xfe\xff\x80\x00\x13\x9c", 8).repeated(8)));

        BatchOptions options;
        options.kinds = BatchPicture | BatchModel | BatchSound | BatchJson | BatchCsv;
        options.baseFolder = game;
        options.outputFolder = out;
        options.keepUnconverted = false;
        const QStringList files = collectBatchFiles({game});
        FormatRegistry registry;
        const ExtractReport report = runExtraction(registry, diskExtractItems(files, options), options);
        for (const BatchFileResult &r : report.results)
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const QStringList written = writtenUnder(report, out);
        QVERIFY(written.contains("models/hero.glb"));
        QVERIFY(readFile(out + "/models/hero.glb").startsWith("glTF"));
        QVERIFY(written.contains("data/hero.json"));
        QVERIFY(std::any_of(written.begin(), written.end(), [](const QString &w) {
            return w.startsWith("data/items") && w.endsWith(".csv");
        }));
        QVERIFY(!std::any_of(written.begin(), written.end(), [](const QString &w) { return w.startsWith("data/junk"); }));
        QVERIFY(!std::any_of(written.begin(), written.end(), [](const QString &w) { return w.endsWith(".png"); }));
    }

    void keepsExistingFilesUnlessAskedToReplace()
    {
        QTemporaryDir dir;
        QVERIFY(writeFile(dir.filePath("hero.mdl"), sample::makeRichModel()));
        QVERIFY(writeFile(dir.filePath("out/hero.glb"), "old"));
        BatchOptions options;
        options.kinds = BatchModel;
        options.baseFolder = dir.path();
        options.outputFolder = dir.filePath("out");
        FormatRegistry registry;

        ExtractReport report = runExtraction(registry, diskExtractItems({dir.filePath("hero.mdl")}, options), options);
        QCOMPARE(report.results.size(), 1);
        QVERIFY(report.results.front().written.isEmpty());
        QCOMPARE(report.results.front().kept, QStringList{dir.filePath("out/hero.glb")});
        QCOMPARE(readFile(dir.filePath("out/hero.glb")), QByteArray("old"));

        options.replaceExisting = true;
        report = runExtraction(registry, diskExtractItems({dir.filePath("hero.mdl")}, options), options);
        QCOMPARE(writtenUnder(report, dir.filePath("out")), QStringList{"hero.glb"});
        QVERIFY(readFile(dir.filePath("out/hero.glb")).startsWith("glTF"));
    }

    void onlyChosenKindsAreWritten()
    {
        QTemporaryDir dir;
        QVERIFY(writeFile(dir.filePath("hero.mdl"), sample::makeRichModel()));
        BatchOptions options;
        options.kinds = BatchSound | BatchJson;
        options.outputFolder = dir.filePath("out");
        options.keepUnconverted = false;
        FormatRegistry registry;
        const ExtractReport report = runExtraction(registry, diskExtractItems({dir.filePath("hero.mdl")}, options), options);
        QVERIFY(writtenUnder(report, dir.filePath("out")).isEmpty());
        for (const BatchFileResult &r : report.results)
            QVERIFY(r.error.isEmpty());
    }

    void extractionOpensNestedArchivesAndConverts()
    {
        QTemporaryDir dir;
        const QString out = dir.filePath("out");
        const QByteArray inner = samplesarc::make({{"deep/hero.mdl", sample::makeRichModel()}, {"note.bin", "raw"}});
        QVERIFY(writeFile(dir.filePath("game/pack.sarc"),
                          samplesarc::make({{"inner.sarc", inner}, {"single.sarc", samplesarc::make({{"only.mdl", sample::makeRichModel()}})},
                                            {"data/items.tbl", sample::makeSample()}})));
        BatchOptions options;
        options.outputFolder = out;
        options.baseFolder = dir.filePath("game");
        FormatRegistry registry;
        qint64 lastDone = 0, lastTotal = 0;
        const ExtractReport report = runExtraction(
            registry, diskExtractItems({dir.filePath("game/pack.sarc")}, options), options,
            [&](qint64 done, qint64 total) {
                lastDone = done;
                lastTotal = total;
                return true;
            },
            2);
        QVERIFY(!report.canceled);
        QCOMPARE(lastDone, lastTotal);
        QCOMPARE(report.archives, qint64(3));
        const QStringList written = writtenUnder(report, out);
        QVERIFY2(written.contains("pack/inner/deep/hero.glb"), qPrintable(written.join(", ")));
        QVERIFY2(written.contains("pack/inner/note.bin"), qPrintable(written.join(", ")));
        QVERIFY2(written.contains("pack/single.glb"), qPrintable(written.join(", ")));
        QVERIFY2(written.contains("pack/data/items.tbl"), qPrintable(written.join(", ")));
        QVERIFY(!written.contains("pack/inner/deep/hero.mdl"));
        QVERIFY(!written.contains("pack.sarc"));
        QVERIFY(readFile(out + "/pack/inner/deep/hero.glb").startsWith("glTF"));
        QCOMPARE(readFile(out + "/pack/inner/note.bin"), QByteArray("raw"));
    }

    void extractionCanKeepOriginalsOrSkipUnconverted()
    {
        QTemporaryDir dir;
        const QByteArray pack = samplesarc::make({{"hero.mdl", sample::makeRichModel()}, {"note.bin", "raw"}});
        QVERIFY(writeFile(dir.filePath("pack.sarc"), pack));
        FormatRegistry registry;

        BatchOptions options;
        options.outputFolder = dir.filePath("both");
        options.keepOriginals = true;
        const std::unique_ptr<Document> doc = registry.open(dir.filePath("pack.sarc"), nullptr, nullptr);
        QVERIFY(doc);
        const std::shared_ptr<const Archive> archive = doc->archive();
        QVERIFY(archive);
        ExtractReport report = runExtraction(registry, archiveExtractItems(archive, {0, 1}, dir.filePath("pack.sarc"), "pack.sarc", options), options);
        QCOMPARE(writtenUnder(report, options.outputFolder), (QStringList{"hero.glb", "hero.mdl", "note.bin"}));

        options.outputFolder = dir.filePath("converted");
        options.keepOriginals = false;
        options.keepUnconverted = false;
        report = runExtraction(registry, archiveExtractItems(archive, {0, 1}, dir.filePath("pack.sarc"), "pack.sarc", options), options);
        QCOMPARE(writtenUnder(report, options.outputFolder), QStringList{"hero.glb"});

        options.outputFolder = dir.filePath("asis");
        options.kinds = 0;
        options.keepUnconverted = true;
        options.openArchives = false;
        report = runExtraction(registry, diskExtractItems({dir.filePath("pack.sarc")}, options), options);
        QCOMPARE(writtenUnder(report, options.outputFolder), QStringList{"pack.sarc"});
        QCOMPARE(readFile(dir.filePath("asis/pack.sarc")), pack);
    }

    void extractionStopsWhenCanceled()
    {
        QTemporaryDir dir;
        std::vector<samplesarc::File> files;
        for (int i = 0; i < 50; ++i)
            files.push_back({QStringLiteral("f%1.bin").arg(i).toUtf8(), "x"});
        QVERIFY(writeFile(dir.filePath("many.sarc"), samplesarc::make(files)));
        BatchOptions options;
        options.outputFolder = dir.filePath("out");
        FormatRegistry registry;
        const ExtractReport report = runExtraction(
            registry, diskExtractItems({dir.filePath("many.sarc")}, options), options,
            [](qint64 done, qint64) { return done < 3; }, 1);
        QVERIFY(report.canceled);
        QVERIFY(report.items < 51);
    }

    void csvQuotesSpecialCharacters()
    {
        class OneCell : public Table
        {
        public:
            QString name() const override { return "t"; }
            int rowCount() const override { return 1; }
            int columnCount() const override { return 1; }
            QString columnName(int) const override { return "a,b"; }
            QString text(int, int, DisplayMode) const override { return "say \"hi\""; }
        } table;
        QCOMPARE(tableToCsv(table), QByteArray("row,\"a,b\"\n0,\"say \"\"hi\"\"\"\n"));
    }
};

QTEST_GUILESS_MAIN(TestBatch)
#include "test_batch.moc"
