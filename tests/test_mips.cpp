#include "textures/bntxreader.h"
#include "textures/bntxwriter.h"
#include "sample_bntx.h"
#include "textures/textureencode.h"
#include "textures/texturemips.h"
#include "textures/xtxreader.h"
#include "textures/xtxwriter.h"

#include <QRandomGenerator>
#include <QTest>
#include <QtEndian>

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

Image solid(int width, int height, quint32 rgba)
{
    Image image{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    for (qsizetype i = 0; i < image.rgba.size(); i += 4)
        qToLittleEndian(rgba, image.rgba.data() + i);
    return image;
}

Image pattern(int width, int height, int seed)
{
    Image image{width, height, QByteArray(qsizetype(width) * height * 4, '\0')};
    auto *p = reinterpret_cast<uchar *>(image.rgba.data());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uchar *q = p + (qsizetype(y) * width + x) * 4;
            q[0] = uchar(x * 255 / std::max(1, width - 1));
            q[1] = uchar(y * 255 / std::max(1, height - 1));
            q[2] = uchar(seed * 40);
            q[3] = 255;
        }
    return image;
}

std::vector<Image> gameLevels(int width, int height)
{
    std::vector<Image> levels;
    for (int n = 0; std::max(1, width >> n) >= 1; ++n) {
        const int w = std::max(1, width >> n), h = std::max(1, height >> n);
        levels.push_back(n == 0 ? pattern(w, h, 0) : solid(w, h, 0xFF000000u | quint32(n * 0x203040)));
        if (w == 1 && h == 1)
            break;
    }
    return levels;
}

struct Chain
{
    std::vector<QByteArray> swizzled;
    std::vector<qsizetype> offsets;
    QByteArray data;
};

Chain buildChain(PixelFormat format, const std::vector<Image> &levels, int baseBlockHeight, qsizetype alignment)
{
    Chain chain;
    const int bpe = bytesPerElement(format);
    for (const Image &level : levels) {
        const int columns = elementColumns(format, level.width), rows = elementRows(format, level.height);
        const QByteArray linear = encodePixels(format, level, {}, nullptr)->linear;
        const int blockHeight = chain.offsets.empty() ? baseBlockHeight : mipBlockHeight(baseBlockHeight, rows);
        const QByteArray swizzled = *swizzleBlockLinear(linear, columns, rows, bpe, blockHeight);
        chain.data += QByteArray(samplebntx::alignTo(chain.data.size(), alignment) - chain.data.size(), '\0');
        chain.offsets.push_back(chain.data.size());
        chain.data += swizzled;
        chain.swizzled.push_back(swizzled);
    }
    return chain;
}

QByteArray makeXtx(int width, int height, quint32 nvn, const Chain &chain, quint32 blockHeightLog2)
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
    appendU64(info, quint64(chain.data.size()));
    appendU32(info, 512);
    appendU32(info, quint32(width));
    appendU32(info, quint32(height));
    appendU32(info, 1);
    appendU32(info, 1);
    appendU32(info, nvn);
    appendU32(info, quint32(chain.offsets.size()));
    appendU32(info, quint32(chain.data.size()));
    for (int i = 0; i < 17; ++i)
        appendU32(info, i < int(chain.offsets.size()) ? quint32(chain.offsets[size_t(i)]) : 0);
    appendU32(info, blockHeightLog2);
    appendU32(info, 0x10007);
    appendU32(info, 0);
    b.append(info);
    b.append("HBvN", 4);
    appendU32(b, 0x24);
    appendU64(b, quint64(chain.data.size()));
    appendU64(b, 0x24);
    appendU32(b, 3);
    appendU32(b, 1);
    appendU32(b, 0);
    b.append(chain.data);
    return b;
}

qsizetype xtxDataStart(const QByteArray &)
{
    return 0x10 + 0x24 + 0x78 + 0x24;
}

std::vector<Image> decodedLevels(PixelFormat format, int width, int height, int baseBlockHeight, const QByteArray &data,
                                 const std::vector<qsizetype> &offsets)
{
    QString error;
    const auto layout = mipLayout(format, width, height, baseBlockHeight, offsets, data.size(), &error);
    return layout ? decodeMips(format, *layout, data).value_or(std::vector<Image>()) : std::vector<Image>();
}

int maxError(const Image &a, const Image &b)
{
    int worst = 0;
    for (qsizetype i = 0; i < std::min(a.rgba.size(), b.rgba.size()); ++i)
        worst = std::max(worst, std::abs(int(uchar(a.rgba[i])) - int(uchar(b.rgba[i]))));
    return worst;
}

Image invertCorner(Image image, int size)
{
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            for (int c = 0; c < 3; ++c) {
                const qsizetype at = (qsizetype(y) * image.width + x) * 4 + c;
                image.rgba[at] = char(255 - uchar(image.rgba[at]));
            }
    return image;
}

}

class TestMips : public QObject
{
    Q_OBJECT

private slots:
    void followsTheTegraBlockHeightRule()
    {
        QCOMPARE(mipBlockHeight(16, 256), 16);
        QCOMPARE(mipBlockHeight(16, 128), 16);
        QCOMPARE(mipBlockHeight(16, 64), 8);
        QCOMPARE(mipBlockHeight(16, 32), 4);
        QCOMPARE(mipBlockHeight(16, 16), 2);
        QCOMPARE(mipBlockHeight(16, 8), 1);
        QCOMPARE(mipBlockHeight(16, 1), 1);
        QCOMPARE(mipBlockHeight(4, 128), 4);
        QCOMPARE(mipBlockHeight(8, 33), 8);
        QCOMPARE(mipBlockHeight(8, 32), 4);
        QCOMPARE(mipBlockHeight(1, 500), 1);
    }

    void laysOutLevelsLikeTheGame()
    {
        QString error;
        std::vector<qsizetype> offsets{0, 524288, 655360, 688128, 696320, 698368, 698880, 699392, 699904, 700416, 700928};
        const auto levels = mipLayout(PixelFormat::BC1, 1024, 1024, 16, offsets, 701440, &error);
        QVERIFY2(levels, qPrintable(error));
        const qsizetype sizes[] = {524288, 131072, 32768, 8192, 2048, 512, 512, 512, 512, 512, 512};
        for (size_t n = 0; n < levels->size(); ++n) {
            QCOMPARE((*levels)[n].size, sizes[n]);
            QCOMPARE((*levels)[n].width, std::max(1, 1024 >> n));
        }
        QVERIFY(!mipLayout(PixelFormat::BC1, 1024, 1024, 16, offsets, 701439, &error));
        QVERIFY2(error.contains("Mip level 10 lies outside"), qPrintable(error));
        offsets[3] = 655360 + 1000;
        QVERIFY(!mipLayout(PixelFormat::BC1, 1024, 1024, 16, offsets, 701440, &error));
        QVERIFY2(error.contains("overlaps"), qPrintable(error));
    }

    void downsamplesWithABoxFilter()
    {
        Image source{3, 3, QByteArray(9 * 4, '\0')};
        for (int i = 0; i < 9; ++i)
            source.rgba[i * 4] = char(i * 20);
        const Image half = downsampleBox(source, 1, 1, false);
        QCOMPARE(uchar(half.rgba[0]), uchar((0 + 20 + 60 + 80 + 2) / 4));

        Image black_white{2, 1, QByteArray("\x00\x00\x00\xFF\xFF\xFF\xFF\xFF", 8)};
        const Image plain = downsampleBox(black_white, 1, 1, false);
        const Image gamma = downsampleBox(black_white, 1, 1, true);
        QCOMPARE(uchar(plain.rgba[0]), uchar(128));
        QCOMPARE(uchar(gamma.rgba[0]), uchar(188));
        QCOMPARE(uchar(gamma.rgba[3]), uchar(255));
    }

    void keepsTheGamesLevelsWhereNothingChanged()
    {
        const std::vector<Image> game = gameLevels(32, 16);
        std::vector<MipLevel> layout;
        for (const Image &level : game)
            layout.push_back({level.width, level.height});
        const std::vector<Image> same = rebuildMips(game[0], game, layout, false);
        for (size_t n = 0; n < game.size(); ++n)
            QCOMPARE(same[n].rgba, game[n].rgba);

        Image edited = game[0];
        edited.rgba[0] = char(uchar(edited.rgba[0]) ^ 0xFF);
        const std::vector<Image> rebuilt = rebuildMips(edited, game, layout, false);
        for (size_t n = 1; n < game.size(); ++n) {
            QVERIFY(rebuilt[n].rgba.left(4) != game[n].rgba.left(4));
            QCOMPARE(rebuilt[n].rgba.mid(4), game[n].rgba.mid(4));
        }
    }

    void replacesXtxTexturesWithMips_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<quint32>("nvn");
        QTest::addColumn<int>("tolerance");
        QTest::newRow("RGBA8") << int(PixelFormat::RGBA8) << 0x25u << 0;
        QTest::newRow("BC1") << int(PixelFormat::BC1) << 0x42u << 40;
        QTest::newRow("BC7") << int(PixelFormat::BC7) << 0x4Du << 12;
    }

    void replacesXtxTexturesWithMips()
    {
        QFETCH(int, format);
        QFETCH(quint32, nvn);
        QFETCH(int, tolerance);
        const PixelFormat f = PixelFormat(format);
        const int width = 64, height = 48, base = 2;
        const std::vector<Image> game = gameLevels(width, height);
        const Chain chain = buildChain(f, game, base, 1);
        const QByteArray file = makeXtx(width, height, nvn, chain, 1);
        const qsizetype start = xtxDataStart(file);
        const std::vector<Image> original =
            decodedLevels(f, width, height, base, file.mid(start), chain.offsets);
        QCOMPARE(original.size(), game.size());

        QString error;
        const auto same = replaceXtxImage(file, original[0], &error);
        QVERIFY2(same, qPrintable(error));
        QCOMPARE(same->mipLevels, int(game.size()));
        QVERIFY(same->file == file);

        const Image edited = invertCorner(original[0], 8);
        const auto changed = replaceXtxImage(file, edited, &error);
        QVERIFY2(changed, qPrintable(error));
        QCOMPARE(changed->file.size(), file.size());
        const std::vector<Image> after =
            decodedLevels(f, width, height, base, changed->file.mid(start), chain.offsets);
        QVERIFY(maxError(after[0], edited) <= tolerance);
        Image expected = edited;
        for (size_t n = 1; n < game.size(); ++n) {
            const Image box = downsampleBox(expected, after[n].width, after[n].height, false);
            const int corner = std::max(1, 8 >> n);
            for (int y = 0; y < after[n].height; ++y)
                for (int x = 0; x < after[n].width; ++x) {
                    const qsizetype at = (qsizetype(y) * after[n].width + x) * 4;
                    const Image &want = x < corner && y < corner ? box : original[n];
                    for (int c = 0; c < 4; ++c)
                        QVERIFY2(std::abs(int(uchar(after[n].rgba[at + c])) - int(uchar(want.rgba[at + c]))) <= tolerance + 8,
                                 qPrintable(QString("level %1 pixel %2,%3").arg(n).arg(x).arg(y)));
                }
            Image next = original[n];
            for (int y = 0; y < corner && y < next.height; ++y)
                for (int x = 0; x < corner && x < next.width; ++x)
                    std::memcpy(next.rgba.data() + (qsizetype(y) * next.width + x) * 4,
                                box.rgba.constData() + (qsizetype(y) * box.width + x) * 4, 4);
            expected = next;
        }
        if (f != PixelFormat::RGBA8) {
            qsizetype differing = 0;
            for (qsizetype i = 0; i < chain.data.size(); i += bytesPerElement(f))
                differing += file.mid(start + i, bytesPerElement(f)) != changed->file.mid(start + i, bytesPerElement(f));
            QVERIFY2(differing <= 4 + qsizetype(game.size()), qPrintable(QString::number(differing)));
        }
    }

    void replacesBntxTexturesWithMips()
    {
        const int width = 64, height = 32, base = 2;
        const std::vector<Image> game = gameLevels(width, height);
        const Chain chain = buildChain(PixelFormat::BC4, game, base, 0x200);
        samplebntx::Texture t;
        t.name = "Masked";
        t.format = 0x1D01;
        t.width = width;
        t.height = height;
        t.channels = 0x02010101;
        t.elementBytes = 8;
        t.elementWidth = 4;
        t.elementHeight = 4;
        t.blockHeightLog2 = 1;
        t.mips = int(game.size());
        t.linear = encodePixels(PixelFormat::BC4, game[0], {}, nullptr)->linear;
        t.mipData.assign(chain.swizzled.begin() + 1, chain.swizzled.end());
        const QByteArray file = samplebntx::make({t});

        QString error;
        const auto bntx = parseBntx(file, 0, &error);
        QVERIFY2(bntx, qPrintable(error));
        QCOMPARE(bntx->textures[0].mipCount, int(game.size()));
        const auto doc = BntxReader().readBytes(file, "a.bntx", &error);
        const Image shown = *doc->tables[0]->image();

        const auto same = replaceBntxImage(file, 0, 0, shown, &error);
        QVERIFY2(same, qPrintable(error));
        QVERIFY(same->file == file);

        Image edited = shown;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                edited.rgba[(qsizetype(y) * width + x) * 4 + 3] = char(0x20);
        const auto changed = replaceBntxImage(file, 0, 0, edited, &error);
        QVERIFY2(changed, qPrintable(error));
        const qsizetype start = bntx->textures[0].mipOffsets[0];
        std::vector<qsizetype> offsets;
        for (qsizetype at : bntx->textures[0].mipOffsets)
            offsets.push_back(at - start);
        const auto before = decodedLevels(PixelFormat::BC4, width, height, base, file.mid(start, bntx->textures[0].imageSize), offsets);
        const auto after = decodedLevels(PixelFormat::BC4, width, height, base,
                                         changed->file.mid(start, bntx->textures[0].imageSize), offsets);
        QVERIFY(std::abs(int(uchar(after[0].rgba[0])) - 0x20) <= 8);
        auto outsideCornerSame = [](const Image &a, const Image &b) {
            for (int y = 0; y < a.height; ++y)
                for (int x = 0; x < a.width; ++x)
                    if ((x >= 4 || y >= 4) && a.rgba.mid((qsizetype(y) * a.width + x) * 4, 4) != b.rgba.mid((qsizetype(y) * b.width + x) * 4, 4))
                        return false;
            return true;
        };
        for (size_t n = 0; n < after.size(); ++n) {
            QVERIFY2(outsideCornerSame(after[n], before[n]), qPrintable(QString::number(n)));
            QVERIFY2(after[n].rgba.left(1) != before[n].rgba.left(1), qPrintable(QString::number(n)));
        }
    }

    void refusesBrokenMipTables()
    {
        const std::vector<Image> game = gameLevels(16, 16);
        Chain chain = buildChain(PixelFormat::RGBA8, game, 1, 1);
        QByteArray file = makeXtx(16, 16, 0x25, chain, 0);
        QString error;
        QVERIFY(replaceXtxImage(file, game[0], &error));
        const qsizetype offsetsAt = 0x10 + 0x24 + 0x28;
        qToLittleEndian(quint32(10), file.data() + offsetsAt + 4);
        QVERIFY(!replaceXtxImage(file, game[0], &error));
        QVERIFY2(error.contains("overlaps"), qPrintable(error));
        qToLittleEndian(quint32(0x7FFFFFF0), file.data() + offsetsAt + 4);
        QVERIFY(!replaceXtxImage(file, game[0], &error));
        QVERIFY2(error.contains("lies outside"), qPrintable(error));
        qToLittleEndian(quint32(40), file.data() + 0x10 + 0x24 + 0x20);
        QVERIFY(!replaceXtxImage(file, game[0], &error));
        QVERIFY2(error.contains("40 mip levels"), qPrintable(error));
    }

    void brokenInputNeverCrashes()
    {
        const std::vector<Image> game = gameLevels(32, 32);
        const QByteArray xtx = makeXtx(32, 32, 0x42, buildChain(PixelFormat::BC1, game, 1, 1), 0);
        samplebntx::Texture t;
        t.name = "T";
        t.format = 0x0B01;
        t.width = 32;
        t.height = 32;
        t.mips = int(game.size());
        t.linear = game[0].rgba;
        const Chain chain = buildChain(PixelFormat::RGBA8, game, 1, 0x200);
        t.mipData.assign(chain.swizzled.begin() + 1, chain.swizzled.end());
        const QByteArray bntx = samplebntx::make({t});
        const Image picture = pattern(32, 32, 3);
        QRandomGenerator rng(17);
        for (const QByteArray &base : {xtx, bntx}) {
            for (int round = 0; round < 2500; ++round) {
                QByteArray d = base;
                const qsizetype limit = round % 2 ? std::min<qsizetype>(d.size(), 0x200) : d.size();
                for (int k = 0; k < 1 + int(rng.bounded(4)); ++k) {
                    const qsizetype at = rng.bounded(int(limit));
                    if (round % 3 == 0 && at + 4 <= d.size())
                        qToLittleEndian(quint32(round % 5 ? 0xFFFFFFFF : rng.bounded(64)), d.data() + at);
                    else
                        d[at] = char(rng.bounded(256));
                }
                QString error;
                replaceTextureImage(d, {0, 0, {}}, picture, &error);
            }
            for (qsizetype n = 0; n < base.size(); n += 13) {
                QString error;
                replaceTextureImage(base.left(n), {0, 0, {}}, picture, &error);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestMips)
#include "test_mips.moc"
