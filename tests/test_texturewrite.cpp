#include "textures/textureencode.h"
#include "textures/texturedecode.h"
#include "textures/xtxreader.h"
#include "textures/xtxwriter.h"

#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>

#include <algorithm>
#include <cstdlib>

namespace {

void appendU32(QByteArray &b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

void appendU64(QByteArray &b, quint64 v)
{
    char raw[8];
    qToLittleEndian(v, raw);
    b.append(raw, 8);
}

QByteArray makeXtx(int width, int height, quint32 nvn, const QByteArray &pixels, quint32 mips = 1, quint32 blockHeightLog2 = 0)
{
    QByteArray b("DFvN", 4);
    appendU32(b, 0x10);
    appendU32(b, 1);
    appendU32(b, 1);
    b.append("HBvN", 4);
    appendU32(b, 0x24);
    appendU64(b, 0x78);
    appendU64(b, 0x24);
    appendU32(b, 2);
    appendU32(b, 0);
    appendU32(b, 0);
    QByteArray info;
    appendU64(info, quint64(pixels.size()));
    appendU32(info, 512);
    appendU32(info, quint32(width));
    appendU32(info, quint32(height));
    appendU32(info, 1);
    appendU32(info, 1);
    appendU32(info, nvn);
    appendU32(info, mips);
    appendU32(info, quint32(pixels.size()));
    info.append(QByteArray(17 * 4, '\0'));
    appendU32(info, blockHeightLog2);
    appendU32(info, 0x10007);
    appendU32(info, 0);
    b.append(info);
    b.append("HBvN", 4);
    appendU32(b, 0x24);
    appendU64(b, quint64(pixels.size()));
    appendU64(b, 0x24);
    appendU32(b, 3);
    appendU32(b, 1);
    appendU32(b, 0);
    b.append(pixels);
    return b;
}

Image gradient(int width, int height)
{
    Image image{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    auto *p = reinterpret_cast<uchar *>(image.rgba.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uchar *q = p + (qsizetype(y) * width + x) * 4;
            q[0] = uchar(x * 255 / std::max(1, width - 1));
            q[1] = uchar(y * 255 / std::max(1, height - 1));
            q[2] = uchar((x + y) * 8);
            q[3] = uchar(x < width / 2 ? 255 : 128);
        }
    return image;
}

Image decodeXtx(const QByteArray &file)
{
    QString error;
    const auto doc = XtxReader::fromBytes(file, &error);
    if (!doc)
        return {};
    const auto image = doc->tables.front()->image();
    return image ? *image : Image{};
}

QByteArray bc7Texture(const Image &image, int blockHeight)
{
    const auto encoded = encodePixels(PixelFormat::BC7, image, {}, nullptr);
    return *swizzleBlockLinear(encoded->linear, image.width / 4, image.height / 4, 16, blockHeight);
}

int maxError(const Image &a, const Image &b)
{
    int worst = 0;
    for (qsizetype i = 0; i < a.rgba.size(); ++i)
        worst = std::max(worst, std::abs(int(uchar(a.rgba[i])) - int(uchar(b.rgba[i]))));
    return worst;
}

void swapRects(Image &image, int ax, int bx, int y0, int size)
{
    for (int y = y0; y < y0 + size; ++y) {
        char *row = image.rgba.data() + qsizetype(y) * image.width * 4;
        for (int x = 0; x < size * 4; ++x)
            std::swap(row[ax * 4 + x], row[bx * 4 + x]);
    }
}

}

class TestTextureWrite : public QObject
{
    Q_OBJECT

private slots:
    void swizzleIsTheInverseOfDeswizzle()
    {
        for (int blockHeight : {1, 2, 4, 16}) {
            const int width = 37, height = 29, bpe = 16;
            QByteArray linear(qsizetype(width) * height * bpe, Qt::Uninitialized);
            for (char &c : linear)
                c = char(QRandomGenerator::global()->bounded(256));
            const auto swizzled = swizzleBlockLinear(linear, width, height, bpe, blockHeight);
            QVERIFY(swizzled);
            QCOMPARE(swizzled->size(), blockLinearSize(width, height, bpe, blockHeight));
            QCOMPARE(deswizzleBlockLinear(*swizzled, width, height, bpe, blockHeight).value_or(QByteArray()), linear);
        }
    }

    void replacesRgba8Exactly()
    {
        const QByteArray file = makeXtx(8, 8, 0x25, QByteArray(512, '\0'));
        const Image picture = gradient(8, 8);
        QString error;
        const auto result = replaceXtxImage(file, picture, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->file.size(), file.size());
        QCOMPARE(decodeXtx(result->file).rgba, picture.rgba);
    }

    void movedBc7BlocksAreKeptExactly()
    {
        const Image original = gradient(64, 32);
        const QByteArray file = makeXtx(64, 32, 0x4D, bc7Texture(original, 1));
        Image moved = decodeXtx(file);
        QCOMPARE(moved.width, 64);
        swapRects(moved, 0, 16, 0, 16);
        swapRects(moved, 32, 48, 16, 16);

        QString error;
        const auto result = replaceXtxImage(file, moved, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->encodedBlocks, 0);
        QCOMPARE(result->reusedBlocks, 16 * 8);
        QCOMPARE(decodeXtx(result->file).rgba, moved.rgba);
    }

    void changedBc7BlocksAreEncodedClosely()
    {
        const Image original = gradient(32, 32);
        const QByteArray file = makeXtx(32, 32, 0x4D, bc7Texture(original, 1));
        Image edited = decodeXtx(file);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                uchar *p = reinterpret_cast<uchar *>(edited.rgba.data()) + (y * 32 + x) * 4;
                p[0] = 30;
                p[1] = 200;
                p[2] = 60;
                p[3] = 255;
            }
        QString error;
        const auto result = replaceXtxImage(file, edited, &error);
        QVERIFY2(result, qPrintable(error));
        QCOMPARE(result->encodedBlocks, 4);
        QCOMPARE(result->reusedBlocks, 64 - 4);
        QVERIFY(maxError(decodeXtx(result->file), edited) <= 8);
    }

    void encodesBc1()
    {
        Image picture{16, 16, QByteArray(16 * 16 * 4, '\0')};
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const int t = (x + y) * 255 / 30;
                uchar *p = reinterpret_cast<uchar *>(picture.rgba.data()) + (y * 16 + x) * 4;
                p[0] = uchar(40 + t * 180 / 255);
                p[1] = uchar(200 - t * 150 / 255);
                p[2] = uchar(20 + t * 60 / 255);
                p[3] = 255;
            }
        const auto encoded = encodePixels(PixelFormat::BC1, picture, {}, nullptr);
        QVERIFY(encoded);
        QCOMPARE(encoded->linear.size(), 4 * 4 * 8);
        const auto decoded = decodePixels(PixelFormat::BC1, encoded->linear, 16, 16);
        QVERIFY(decoded);
        QVERIFY(maxError(Image{16, 16, *decoded}, picture) <= 12);
    }

    void refusesWhatItCannotWrite()
    {
        const QByteArray file = makeXtx(8, 8, 0x25, QByteArray(512, '\0'));
        QString error;
        QVERIFY(!replaceXtxImage(file, gradient(16, 8), &error));
        QVERIFY(error.contains("8 × 8"));
        QVERIFY(!replaceXtxImage(makeXtx(8, 8, 0x25, QByteArray(512, '\0'), 2), gradient(8, 8), &error));
        QVERIFY(!replaceXtxImage("not a texture", gradient(8, 8), &error));
        for (qsizetype n = 0; n < file.size(); n += 7)
            replaceXtxImage(file.left(n), gradient(8, 8), &error);
    }
};

QTEST_GUILESS_MAIN(TestTextureWrite)
#include "test_texturewrite.moc"
