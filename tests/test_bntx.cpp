#include "textures/bntxreader.h"
#include "textures/bntxwriter.h"
#include "core/registry.h"
#include "sample_bntx.h"
#include "textures/textureencode.h"

#include <QRandomGenerator>
#include <QTest>

namespace {

Image gradient(int width, int height, int seed = 0)
{
    Image image{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    auto *p = reinterpret_cast<uchar *>(image.rgba.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uchar *px = p + (qsizetype(y) * width + x) * 4;
            px[0] = uchar(x * 255 / std::max(1, width - 1));
            px[1] = uchar(y * 255 / std::max(1, height - 1));
            px[2] = uchar((x + y + seed) * 7);
            px[3] = uchar(255 - x * 3 - seed);
        }
    return image;
}

samplebntx::Texture rgbaTexture(const QByteArray &name, const Image &image)
{
    samplebntx::Texture t;
    t.name = name;
    t.width = image.width;
    t.height = image.height;
    t.linear = image.rgba;
    return t;
}

samplebntx::Texture encodedTexture(const QByteArray &name, PixelFormat format, quint32 bntxFormat, const Image &image,
                                   quint32 channels = 0x05040302)
{
    samplebntx::Texture t;
    t.name = name;
    t.format = bntxFormat;
    t.width = image.width;
    t.height = image.height;
    t.channels = channels;
    t.linear = encodePixels(format, image, {}, nullptr)->linear;
    t.elementBytes = bytesPerElement(format);
    t.elementWidth = elementWidth(format);
    t.elementHeight = elementHeight(format);
    return t;
}

std::shared_ptr<const Image> imageOf(const Document &doc, int index)
{
    return doc.tables.at(size_t(index))->image();
}

int maxDifference(const QByteArray &a, const QByteArray &b, int channel)
{
    int worst = 0;
    for (qsizetype i = channel; i < std::min(a.size(), b.size()); i += 4)
        worst = std::max(worst, std::abs(int(uchar(a[i])) - int(uchar(b[i]))));
    return worst;
}

}

class TestBntx : public QObject
{
    Q_OBJECT

private slots:
    void readsTexturesAndAppliesChannels()
    {
        const Image colours = gradient(16, 8);
        Image mask = gradient(32, 16);
        const QByteArray file = samplebntx::make({rgbaTexture("Colours", colours),
                                                  encodedTexture("Mask^s", PixelFormat::BC4, 0x1D01, mask, 0x02010101)});
        QString error;
        const auto doc = BntxReader().readBytes(file, "a.bntx", &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(2));
        QCOMPARE(doc->tables[0]->name(), QString("Colours"));
        QCOMPARE(doc->tables[1]->name(), QString("Mask^s"));
        QCOMPARE(doc->tables[1]->textureSlot()->index, 1);
        QCOMPARE(doc->tables[1]->textureSlot()->container, qsizetype(0));
        QCOMPARE(imageOf(*doc, 0)->rgba, colours.rgba);

        const auto shown = imageOf(*doc, 1);
        QVERIFY(shown);
        QCOMPARE(shown->width, 32);
        for (qsizetype i = 0; i < shown->rgba.size(); i += 4) {
            QCOMPARE(uchar(shown->rgba[i]), uchar(255));
            QCOMPARE(uchar(shown->rgba[i + 2]), uchar(255));
        }
        QVERIFY(maxDifference(QByteArray(shown->rgba).remove(0, 3), mask.rgba, 0) <= 12);

        bool foundChannels = false;
        for (int r = 0; r < doc->tables[1]->rowCount(); ++r)
            if (doc->tables[1]->text(r, 0, DisplayMode::Auto) == "channels")
                foundChannels = doc->tables[1]->text(r, 1, DisplayMode::Auto) == "1 1 1 R";
        QVERIFY(foundChannels);
    }

    void readsSwitch2Textures()
    {
        const Image colours = gradient(32, 16);
        QByteArray file = samplebntx::make({rgbaTexture("Colours", colours)});
        std::memcpy(file.data() + 0x20, "Ounc", 4);
        samplebntx::put32(file, 8, 0x00040101);
        QString error;
        const std::optional<BntxFile> parsed = parseBntx(file, 0, &error);
        QVERIFY2(parsed, qPrintable(error));
        const qsizetype start = parsed->textures.front().mipOffsets.front();
        const qsizetype size = parsed->textures.front().imageSize;
        for (qsizetype group = start; group + 128 <= start + size; group += 128)
            for (int i = 0; i < 32; ++i)
                std::swap(file.data()[group + 32 + i], file.data()[group + 64 + i]);
        const auto doc = BntxReader().readBytes(file, "switch2.bntx", &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(imageOf(*doc, 0)->rgba, colours.rgba);
        QCOMPARE(bntxFormatName(0x3206), QString("ASTC 8×5 sRGB  (BNTX 0x3206)"));

        const auto same = replaceBntxImage(file, 0, 0, colours, &error);
        QVERIFY2(same, qPrintable(error));
        QVERIFY(same->file == file);
        const Image changed = gradient(32, 16, 7);
        const auto replaced = replaceBntxImage(file, 0, 0, changed, &error);
        QVERIFY2(replaced, qPrintable(error));
        const auto after = BntxReader().readBytes(replaced->file, "switch2.bntx", &error);
        QVERIFY2(after, qPrintable(error));
        QCOMPARE(imageOf(*after, 0)->rgba, changed.rgba);
    }

    void channelSelectorsRoundTrip()
    {
        const QByteArray stored("\x10\x20\x30\x40", 4);
        for (quint32 channels : {0x05040302u, 0x02010101u, 0x01000302u, 0x03020202u, 0x02020201u}) {
            QByteArray shown = stored;
            applyChannelSelectors(shown, channels);
            QCOMPARE(unapplyChannelSelectors(shown, stored, channels), stored);
        }
        QByteArray shown = stored;
        applyChannelSelectors(shown, 0x02010101);
        QCOMPARE(shown, QByteArray("\xFF\xFF\xFF\x10", 4));
        QByteArray edited("\x00\x00\x00\x99", 4);
        QCOMPARE(unapplyChannelSelectors(edited, stored, 0x02010101), QByteArray("\x99\x20\x30\x40", 4));
    }

    void findsTexturesInsideOtherFiles_data()
    {
        QTest::addColumn<QByteArray>("magic");
        QTest::addColumn<QString>("fileName");
        QTest::newRow("model") << QByteArray("FRES") << "output.bfres";
        QTest::newRow("font") << QByteArray("FFNT") << "font.bffnt";
        QTest::newRow("effect") << QByteArray("VFXB") << "particles.ptcl";
    }

    void findsTexturesInsideOtherFiles()
    {
        QFETCH(QByteArray, magic);
        QFETCH(QString, fileName);
        const Image colours = gradient(8, 8);
        const QByteArray file = samplebntx::embedIn(magic.constData(), samplebntx::make({rgbaTexture("Inner", colours)}));
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(file, fileName, &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), BntxReader().name());
        QCOMPARE(doc->tables.front()->textureSlot()->container, qsizetype(0x1000));
        QCOMPARE(imageOf(*doc, 0)->rgba, colours.rgba);
        QVERIFY(!BntxReader().readBytes(QByteArray("FRES") + QByteArray(5000, 'x'), "x.bfres", &error));
        QVERIFY(error.contains("No BNTX"));
    }

    void replacesTextures()
    {
        const Image colours = gradient(16, 8), bc7 = gradient(8, 8, 3), bc1 = gradient(12, 8, 5);
        Image mask = gradient(32, 16);
        const QByteArray file = samplebntx::make({rgbaTexture("Colours", colours),
                                                  encodedTexture("Mask", PixelFormat::BC4, 0x1D01, mask, 0x02010101),
                                                  encodedTexture("Seven", PixelFormat::BC7, 0x2006, bc7),
                                                  encodedTexture("One", PixelFormat::BC1, 0x1A01, bc1, 0x04040302)});
        QString error;
        const auto doc = BntxReader().readBytes(file, "a.bntx", &error);
        for (int i = 0; i < 4; ++i) {
            const auto same = replaceBntxImage(file, 0, i, *imageOf(*doc, i), &error);
            QVERIFY2(same, qPrintable(error));
            QCOMPARE(same->encodedBlocks, i == 0 ? qsizetype(16 * 8) : qsizetype(0));
            QVERIFY2(same->file == file, qPrintable(doc->tables[size_t(i)]->name()));
        }

        const Image changed = gradient(16, 8, 99);
        const auto result = replaceBntxImage(file, 0, 0, changed, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->file.size(), file.size());
        const auto after = BntxReader().readBytes(result->file, "a.bntx", &error);
        QCOMPARE(imageOf(*after, 0)->rgba, changed.rgba);
        QCOMPARE(imageOf(*after, 2)->rgba, imageOf(*doc, 2)->rgba);

        Image newMask = *imageOf(*doc, 1);
        for (qsizetype i = 0; i < 4 * 8; i += 4)
            newMask.rgba[i + 3] = char(0x80);
        const auto masked = replaceBntxImage(file, 0, 1, newMask, &error);
        QVERIFY2(masked, qPrintable(error));
        QVERIFY(masked->encodedBlocks > 0);
        QVERIFY(masked->reusedBlocks > 0);
        const auto shown = imageOf(*BntxReader().readBytes(masked->file, "a.bntx", &error), 1);
        QVERIFY(maxDifference(shown->rgba, newMask.rgba, 3) <= 12);
        QCOMPARE(maxDifference(shown->rgba, newMask.rgba, 0), 0);
    }

    void replacesTexturesInsideOtherFiles()
    {
        const Image colours = gradient(8, 8);
        const QByteArray file = samplebntx::embedIn("FRES", samplebntx::make({rgbaTexture("Inner", colours)}));
        QString error;
        const Image changed = gradient(8, 8, 50);
        const auto result = replaceTextureImage(file, {0x1000, 0, "Inner"}, changed, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->file.size(), file.size());
        qsizetype first = -1, last = -1;
        for (qsizetype i = 0; i < file.size(); ++i)
            if (file[i] != result->file[i]) {
                if (first < 0)
                    first = i;
                last = i;
            }
        const auto bntx = parseBntx(file, 0x1000, &error);
        QVERIFY(first >= 0x1000 + bntx->textures[0].mipOffsets[0]);
        QVERIFY(last < 0x1000 + bntx->textures[0].mipOffsets[0] + bntx->textures[0].imageSize);
        const auto doc = BntxReader().readBytes(result->file, "x.bfres", &error);
        QCOMPARE(imageOf(*doc, 0)->rgba, changed.rgba);
    }

    void refusesWhatItCannotReplace()
    {
        samplebntx::Texture mipped = rgbaTexture("Mipped", gradient(8, 8));
        mipped.mips = 3;
        samplebntx::Texture astc = rgbaTexture("Astc", gradient(8, 8));
        astc.format = 0x2D05;
        samplebntx::Texture snorm = rgbaTexture("Normals", gradient(8, 8));
        snorm.format = 0x1E02;
        snorm.channels = 0x01000302;
        snorm.elementBytes = 16;
        snorm.elementWidth = 4;
        snorm.elementHeight = 4;
        snorm.linear = QByteArray(64, '\x11');
        const QByteArray file = samplebntx::make({rgbaTexture("Plain", gradient(8, 8)), mipped, astc, snorm});
        QString error;
        QVERIFY(!replaceBntxImage(file, 0, 0, gradient(16, 8), &error));
        QVERIFY2(error.contains("same size"), qPrintable(error));
        QVERIFY(!replaceBntxImage(file, 0, 1, gradient(8, 8), &error));
        QVERIFY2(error.contains("lies outside") || error.contains("overlaps"), qPrintable(error));
        QVERIFY(!replaceBntxImage(file, 0, 2, gradient(8, 8), &error));
        QVERIFY2(error.contains("ASTC 4×4"), qPrintable(error));
        QVERIFY(!replaceBntxImage(file, 0, 3, gradient(8, 8), &error));
        QVERIFY2(error.contains("not supported"), qPrintable(error));
        QVERIFY(!replaceBntxImage(file, 0, 9, gradient(8, 8), &error));
        QVERIFY(!replaceBntxImage("nothing here", 0, 0, gradient(8, 8), &error));
        QVERIFY(!replaceTextureImage(file, {12, 0, {}}, gradient(8, 8), &error));

        const auto doc = BntxReader().readBytes(file, "a.bntx", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(!doc->tables[2]->hasImage());
        QVERIFY(doc->tables[2]->description().contains("ASTC 4×4"));
        QVERIFY(doc->tables[2]->textureSlot());
    }

    void brokenInputNeverCrashes()
    {
        const QByteArray file = samplebntx::make({rgbaTexture("Plain", gradient(8, 8)),
                                                  encodedTexture("Mask", PixelFormat::BC4, 0x1D01, gradient(16, 16), 0x02010101)});
        const QByteArray embedded = samplebntx::embedIn("FRES", file, 0x100);
        auto tryAll = [](const QByteArray &d) {
            QString error;
            if (const auto doc = BntxReader().readBytes(d, "x", &error))
                for (const auto &t : doc->tables)
                    if (t->hasImage())
                        t->image();
            replaceBntxImage(d, 0, 0, gradient(8, 8), &error);
            replaceBntxImage(d, 0, 1, gradient(16, 16), &error);
            replaceTextureImage(d, {0x100, 1, {}}, gradient(16, 16), &error);
        };
        for (const QByteArray &base : {file, embedded}) {
            for (qsizetype n = 0; n <= base.size(); n += (n < 0x300 ? 1 : 29))
                tryAll(base.left(n));
            QRandomGenerator rng(5);
            for (int round = 0; round < 3000; ++round) {
                QByteArray d = base;
                for (int k = 0; k < 1 + int(rng.bounded(5)); ++k) {
                    const qsizetype at = rng.bounded(int(std::min<qsizetype>(d.size(), 0x400 + (round % 3) * 0x1000)));
                    if (round % 2 && at + 8 <= d.size())
                        qToLittleEndian(quint32(round % 4 ? 0xFFFFFFFF : rng.generate()), d.data() + at);
                    else
                        d[at] = char(rng.bounded(256));
                }
                tryAll(d);
            }
        }
    }
};

QTEST_MAIN(TestBntx)
#include "test_bntx.moc"
