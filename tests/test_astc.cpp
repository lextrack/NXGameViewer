#include "textures/astc.h"
#include "textures/bntxreader.h"
#include "textures/bntxwriter.h"
#include "sample_bntx.h"
#include "textures/textureencode.h"
#include "textures/texturemips.h"

#include <QRandomGenerator>
#include <QTest>

#include <atomic>
#include <cmath>
#include <thread>

namespace {

Image picture(int width, int height, int seed = 0)
{
    Image image{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    auto *p = reinterpret_cast<uchar *>(image.rgba.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uchar *q = p + (qsizetype(y) * width + x) * 4;
            q[0] = uchar(x * 255 / std::max(1, width - 1));
            q[1] = uchar(y * 255 / std::max(1, height - 1));
            q[2] = uchar(128 + 100 * std::sin((x + seed) * 0.2));
            q[3] = uchar(x < width / 2 ? 255 : 160);
        }
    return image;
}

double meanError(const QByteArray &a, const QByteArray &b)
{
    double sum = 0;
    for (qsizetype i = 0; i < std::min(a.size(), b.size()); ++i)
        sum += std::abs(int(uchar(a[i])) - int(uchar(b[i])));
    return sum / double(std::max<qsizetype>(1, a.size()));
}

}

class TestAstc : public QObject
{
    Q_OBJECT

private slots:
    void describesBlockSizes()
    {
        const PixelFormat f = *astcFormat(10, 8, true);
        QCOMPARE(elementWidth(f), 10);
        QCOMPARE(elementHeight(f), 8);
        QCOMPARE(bytesPerElement(f), 16);
        QVERIFY(isAstc(f) && isSrgb(f));
        QVERIFY(!isSrgb(*astcFormat(10, 8, false)));
        QCOMPARE(elementColumns(f, 21), 3);
        QCOMPARE(elementRows(f, 16), 2);
        QCOMPARE(pixelFormatName(f), QString("ASTC 10×8 sRGB"));
        QCOMPARE(pixelFormatName(*astcFormat(10, 8, false)), QString("ASTC 10×8"));
        QVERIFY(!astcFormat(7, 7, false));
        QCOMPARE(int(*bntxPixelFormat(0x3106)), int(*astcFormat(6, 6, true)));
        QCOMPARE(int(*bntxPixelFormat(0x2D01)), int(*astcFormat(4, 4, false)));
        QVERIFY(!bntxPixelFormat(0x2D05));
        QVERIFY(bntxFormatName(0x3406).startsWith("ASTC 8×8 sRGB"));
    }

    void encodesAndDecodes_data()
    {
        QTest::addColumn<int>("bw");
        QTest::addColumn<int>("bh");
        QTest::addColumn<bool>("srgb");
        QTest::addColumn<double>("limit");
        QTest::newRow("4x4") << 4 << 4 << false << 2.0;
        QTest::newRow("5x4 sRGB") << 5 << 4 << true << 3.0;
        QTest::newRow("6x6 sRGB") << 6 << 6 << true << 4.0;
        QTest::newRow("8x8") << 8 << 8 << false << 6.0;
        QTest::newRow("10x5") << 10 << 5 << false << 6.0;
        QTest::newRow("12x12 sRGB") << 12 << 12 << true << 9.0;
    }

    void encodesAndDecodes()
    {
        QFETCH(int, bw);
        QFETCH(int, bh);
        QFETCH(bool, srgb);
        QFETCH(double, limit);
        const Image image = picture(37, 23);
        QString error;
        const auto blocks = encodeAstc(image.rgba, image.width, image.height, bw, bh, srgb, &error);
        QVERIFY2(blocks, qPrintable(error));
        QCOMPARE(blocks->size(), qsizetype((37 + bw - 1) / bw) * ((23 + bh - 1) / bh) * 16);
        const auto decoded = decodePixels(*astcFormat(bw, bh, srgb), *blocks, 37, 23);
        QVERIFY(decoded);
        QCOMPARE(decoded->size(), image.rgba.size());
        const double error1 = meanError(*decoded, image.rgba);
        QVERIFY2(error1 <= limit, qPrintable(QString::number(error1)));
        QVERIFY(!decodePixels(*astcFormat(bw, bh, srgb), blocks->left(blocks->size() - 1), 37, 23));
    }

    void keepsUnchangedBlocks()
    {
        const PixelFormat f = *astcFormat(6, 6, true);
        const Image image = picture(48, 30);
        QString error;
        const auto first = encodePixels(f, image, {}, &error);
        QVERIFY2(first, qPrintable(error));
        const Image shown{48, 30, *decodePixels(f, first->linear, 48, 30)};
        const auto same = encodePixels(f, shown, first->linear, &error);
        QVERIFY2(same, qPrintable(error));
        QCOMPARE(same->encodedBlocks, qsizetype(0));
        QCOMPARE(same->linear, first->linear);

        Image edited = shown;
        edited.rgba[(7 * 48 + 13) * 4] = char(uchar(edited.rgba[(7 * 48 + 13) * 4]) ^ 0x80);
        edited.rgba[(29 * 48 + 47) * 4 + 1] = char(uchar(edited.rgba[(29 * 48 + 47) * 4 + 1]) ^ 0x80);
        const auto changed = encodePixels(f, edited, first->linear, &error);
        QVERIFY2(changed, qPrintable(error));
        QCOMPARE(changed->encodedBlocks, qsizetype(2));
        qsizetype differing = 0;
        for (qsizetype i = 0; i < changed->linear.size(); i += 16)
            differing += changed->linear.mid(i, 16) != first->linear.mid(i, 16);
        QVERIFY(differing <= 2);
        const auto decoded = *decodePixels(f, changed->linear, 48, 30);
        QVERIFY(std::abs(int(uchar(decoded[(7 * 48 + 13) * 4])) - int(uchar(edited.rgba[(7 * 48 + 13) * 4]))) <= 24);
    }

    void replacesAstcBntxTexturesWithMips()
    {
        const PixelFormat f = *astcFormat(8, 8, true);
        const int width = 64, height = 64;
        std::vector<Image> levels{picture(width, height)};
        while (levels.back().width > 1)
            levels.push_back(downsampleBox(levels.back(), std::max(1, levels.back().width / 2),
                                           std::max(1, levels.back().height / 2), true));
        std::vector<QByteArray> swizzled;
        for (size_t n = 0; n < levels.size(); ++n) {
            const Image &l = levels[n];
            const QByteArray linear = encodePixels(f, l, {}, nullptr)->linear;
            const int rows = elementRows(f, l.height);
            swizzled.push_back(*swizzleBlockLinear(linear, elementColumns(f, l.width), rows, 16,
                                                   n == 0 ? 1 : mipBlockHeight(1, rows)));
        }
        samplebntx::Texture t;
        t.name = "Astc";
        t.format = 0x3406;
        t.width = width;
        t.height = height;
        t.elementBytes = 16;
        t.elementWidth = 8;
        t.elementHeight = 8;
        t.mips = int(levels.size());
        t.linear = encodePixels(f, levels[0], {}, nullptr)->linear;
        t.mipData.assign(swizzled.begin() + 1, swizzled.end());
        const QByteArray file = samplebntx::make({t});

        QString error;
        const auto doc = BntxReader().readBytes(file, "a.bntx", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables[0]->hasImage());
        const Image shown = *doc->tables[0]->image();
        QVERIFY(meanError(shown.rgba, levels[0].rgba) < 6);

        const auto same = replaceBntxImage(file, 0, 0, shown, &error);
        QVERIFY2(same, qPrintable(error));
        QVERIFY(same->file == file);
        QCOMPARE(same->mipLevels, int(levels.size()));

        Image edited = shown;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                std::memcpy(edited.rgba.data() + (qsizetype(y) * width + x) * 4, "\xFF\x00\x00\xFF", 4);
        const auto changed = replaceBntxImage(file, 0, 0, edited, &error);
        QVERIFY2(changed, qPrintable(error));
        QCOMPARE(changed->file.size(), file.size());
        QCOMPARE(changed->encodedBlocks, qsizetype(levels.size()));
        const auto after = *BntxReader().readBytes(changed->file, "a.bntx", &error)->tables[0]->image();
        QVERIFY(uchar(after.rgba[0]) > 230 && uchar(after.rgba[1]) < 25);
        for (int y = 8; y < height; ++y)
            QCOMPARE(after.rgba.mid(qsizetype(y) * width * 4, width * 4), shown.rgba.mid(qsizetype(y) * width * 4, width * 4));
    }

    void decodesFromManyThreads()
    {
        const PixelFormat f = *astcFormat(4, 4, false);
        const Image image = picture(64, 64);
        const QByteArray blocks = encodePixels(f, image, {}, nullptr)->linear;
        const QByteArray expected = *decodePixels(f, blocks, 64, 64);
        std::vector<std::thread> threads;
        std::atomic<int> wrong{0};
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&] {
                for (int k = 0; k < 20; ++k)
                    if (decodePixels(f, blocks, 64, 64) != expected)
                        ++wrong;
            });
        for (std::thread &t : threads)
            t.join();
        QCOMPARE(wrong.load(), 0);
    }

    void brokenBlocksNeverCrash()
    {
        QRandomGenerator rng(23);
        for (int bw : {4, 6, 10, 12})
            for (int round = 0; round < 200; ++round) {
                QByteArray blocks(qsizetype(4) * 4 * 16, '\0');
                for (char &c : blocks)
                    c = char(rng.bounded(256));
                const auto f = *astcFormat(bw, bw == 10 ? 10 : bw, round % 2);
                const auto decoded = decodePixels(f, blocks, bw * 2, bw * 2);
                QVERIFY(decoded);
                QCOMPARE(decoded->size(), qsizetype(bw) * 2 * bw * 2 * 4);
            }
    }
};

QTEST_GUILESS_MAIN(TestAstc)
#include "test_astc.moc"
