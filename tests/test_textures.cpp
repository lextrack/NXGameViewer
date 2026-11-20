#include "core/registry.h"
#include "textures/ddsreader.h"
#include "textures/texturedecode.h"
#include "textures/xtxreader.h"

#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <array>
#include <cstring>

namespace {

using Rgba = std::array<int, 4>;

Rgba pixel(const QByteArray &rgba, int width, int x, int y)
{
    const auto *p = reinterpret_cast<const uchar *>(rgba.constData()) + (y * width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

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

// Writes bits from the lowest bit up, as BC7 reads them.
class BitWriter
{
public:
    void write(quint32 value, int count)
    {
        for (int i = 0; i < count; ++i, ++m_pos) {
            if (value >> i & 1)
                m_bytes[m_pos / 8] = char(m_bytes[m_pos / 8] | (1 << (m_pos % 8)));
        }
    }
    QByteArray bytes() const { return m_bytes; }

private:
    QByteArray m_bytes = QByteArray(16, '\0');
    int m_pos = 0;
};

// An XTX file with one 8×8 RGBA8 texture, swizzled into a single GOB (block height 1).
// GOB byte `a` holds the value `a % 256`, so every pixel shows where it was read from.
QByteArray makeXtx(quint32 nvnFormat = 0x25, qsizetype pixelBytes = 512)
{
    QByteArray b("DFvN", 4);
    appendU32(b, 0x10);
    appendU32(b, 1);
    appendU32(b, 1);

    // Texture info block
    b.append("HBvN", 4);
    appendU32(b, 0x24);
    appendU64(b, 0x78);
    appendU64(b, 0x24);
    appendU32(b, 2);
    appendU32(b, 0);
    appendU32(b, 0);
    QByteArray info;
    appendU64(info, 512);  // data size
    appendU32(info, 512);  // alignment
    appendU32(info, 8);    // width
    appendU32(info, 8);    // height
    appendU32(info, 1);    // depth
    appendU32(info, 1);    // target: 2D
    appendU32(info, nvnFormat);
    appendU32(info, 1);    // mips
    appendU32(info, 512);  // slice size
    info.append(QByteArray(17 * 4, '\0'));  // mip offsets
    appendU32(info, 0);    // block height log2: 1 GOB
    appendU32(info, 0x10007);
    appendU32(info, 0);
    Q_ASSERT(info.size() == 0x78);
    b.append(info);

    // Pixel data block
    b.append("HBvN", 4);
    appendU32(b, 0x24);
    appendU64(b, quint64(pixelBytes));
    appendU64(b, 0x24);
    appendU32(b, 3);
    appendU32(b, 1);
    appendU32(b, 0);
    for (qsizetype i = 0; i < pixelBytes; ++i)
        b.append(char(i % 256));
    return b;
}

}

class TestTextures : public QObject
{
    Q_OBJECT

private slots:
    // --- Block-linear layout (Tegra X1 GOB: 64 bytes × 8 rows = 512 bytes) ---------------

    void deswizzleFollowsGobLayout()
    {
        // 16-byte elements, 4 per GOB row. Inside a GOB, known positions:
        //   (x=0,y=1) at byte 16, (0,2) at 64, (1,0) at 32, (2,0) at 256, (3,1) at 256+32+16.
        QByteArray gob(512, '\0');
        for (int i = 0; i < 512; i += 16)
            gob[i] = char(i / 16);  // first byte of each 16-byte piece = its position / 16
        const auto linear = deswizzleBlockLinear(gob, 4, 8, 16, 1);
        QVERIFY(linear);
        auto at = [&](int x, int y) { return int(uchar(linear->at((y * 4 + x) * 16))) * 16; };
        QCOMPARE(at(0, 0), 0);
        QCOMPARE(at(0, 1), 16);
        QCOMPARE(at(0, 2), 64);
        QCOMPARE(at(1, 0), 32);
        QCOMPARE(at(2, 0), 256);
        QCOMPARE(at(3, 1), 256 + 32 + 16);
    }

    void deswizzleStacksGobsByBlockHeight()
    {
        // 2 GOBs wide, 2 GOBs high, block height 2: GOBs are stacked vertically first,
        // so the GOB to the right of the first one starts at 2 × 512.
        QByteArray data(4 * 512, '\0');
        data[512] = 'B';       // GOB below the first
        data[2 * 512] = 'R';   // GOB to the right
        const auto linear = deswizzleBlockLinear(data, 8, 16, 16, 2);
        QVERIFY(linear);
        QCOMPARE(linear->at((8 * 8 + 0) * 16), 'B');  // x=0, y=8
        QCOMPARE(linear->at((0 * 8 + 4) * 16), 'R');  // x=4, y=0
    }

    void deswizzleRejectsShortData()
    {
        QVERIFY(!deswizzleBlockLinear(QByteArray(511, '\0'), 4, 8, 16, 1));
        QVERIFY(!deswizzleBlockLinear(QByteArray(512, '\0'), 4, 8, 16, 3));  // not a power of two
        QCOMPARE(blockLinearSize(5, 9, 16, 1), qsizetype(2 * 2 * 512));  // padded to whole GOBs
    }

    // --- Block compression ----------------------------------------------------------------

    void decodesBc1()
    {
        // colour0 = pure red (0xF800), colour1 = pure blue (0x001F); colour0 > colour1: 4 colours.
        // Row 0 uses indices 0,1,2,3.
        QByteArray block;
        block.append(char(0x00)).append(char(0xF8)).append(char(0x1F)).append(char(0x00));
        block.append(char(0b11100100)).append(3, '\0');
        const auto rgba = decodePixels(PixelFormat::BC1, block, 4, 4);
        QVERIFY(rgba);
        QCOMPARE(pixel(*rgba, 4, 0, 0), (Rgba{255, 0, 0, 255}));
        QCOMPARE(pixel(*rgba, 4, 1, 0), (Rgba{0, 0, 255, 255}));
        QCOMPARE(pixel(*rgba, 4, 2, 0), (Rgba{170, 0, 85, 255}));  // 2/3 red + 1/3 blue
        QCOMPARE(pixel(*rgba, 4, 3, 0), (Rgba{85, 0, 170, 255}));
        QCOMPARE(pixel(*rgba, 4, 0, 1), (Rgba{255, 0, 0, 255}));   // other rows: index 0
    }

    void bc1ThreeColourModeHasTransparency()
    {
        // colour0 <= colour1: index 2 = average, index 3 = transparent.
        QByteArray block;
        block.append(char(0x1F)).append(char(0x00)).append(char(0x00)).append(char(0xF8));
        block.append(char(0b11100100)).append(3, '\0');
        const auto rgba = decodePixels(PixelFormat::BC1, block, 4, 4);
        QVERIFY(rgba);
        QCOMPARE(pixel(*rgba, 4, 2, 0), (Rgba{127, 0, 127, 255}));
        QCOMPARE(pixel(*rgba, 4, 3, 0)[3], 0);
    }

    void decodesBc4AsGrey()
    {
        // Endpoints 255 and 0 (255 > 0: 8 levels). Pixel 0 index 0, pixel 1 index 1, pixel 2 index 2.
        QByteArray block;
        block.append(char(255)).append(char(0));
        const quint64 indices = 0 | (1 << 3) | (2 << 6);
        for (int i = 0; i < 6; ++i)
            block.append(char(indices >> (8 * i)));
        const auto rgba = decodePixels(PixelFormat::BC4, block, 4, 4);
        QVERIFY(rgba);
        QCOMPARE(pixel(*rgba, 4, 0, 0), (Rgba{255, 255, 255, 255}));
        QCOMPARE(pixel(*rgba, 4, 1, 0), (Rgba{0, 0, 0, 255}));
        QCOMPARE(pixel(*rgba, 4, 2, 0), (Rgba{218, 218, 218, 255}));  // (6×255 + 1×0) / 7
    }

    void decodesBc7Mode6()
    {
        // Mode 6: one subset, RGBA endpoints of 7 bits + 1 p-bit each, 4-bit indices.
        // Endpoint 0 = 127 with p-bit 1 -> 255; endpoint 1 = 0 with p-bit 0 -> 0.
        BitWriter w;
        w.write(1 << 6, 7);              // mode 6
        for (int channel = 0; channel < 4; ++channel) {
            w.write(127, 7);             // endpoint 0
            w.write(0, 7);               // endpoint 1
        }
        w.write(1, 1);                   // p-bit endpoint 0
        w.write(0, 1);                   // p-bit endpoint 1
        w.write(0, 3);                   // pixel 0: index 0 (anchor, one bit less)
        w.write(15, 4);                  // pixel 1: index 15
        w.write(8, 4);                   // pixel 2: index 8, weight 34
        const auto rgba = decodePixels(PixelFormat::BC7, w.bytes(), 4, 4);
        QVERIFY(rgba);
        QCOMPARE(pixel(*rgba, 4, 0, 0), (Rgba{255, 255, 255, 255}));
        QCOMPARE(pixel(*rgba, 4, 1, 0), (Rgba{0, 0, 0, 0}));
        QCOMPARE(pixel(*rgba, 4, 2, 0), (Rgba{120, 120, 120, 120}));  // (30×255 + 32) >> 6
    }

    void bc7ReservedModeIsTransparentBlack()
    {
        const auto rgba = decodePixels(PixelFormat::BC7, QByteArray(16, '\0'), 4, 4);
        QVERIFY(rgba);
        QCOMPARE(pixel(*rgba, 4, 3, 3), (Rgba{0, 0, 0, 0}));
    }

    void readsDdsTextures()
    {
        auto header = [](int width, int height, quint32 pfFlags, const char *fourCC, int bits, quint32 r, quint32 g,
                         quint32 b, quint32 a) {
            QByteArray h(128, '\0');
            std::memcpy(h.data(), "DDS ", 4);
            auto put = [&h](int at, quint32 v) { qToLittleEndian(v, h.data() + at); };
            put(4, 124);
            put(12, quint32(height));
            put(16, quint32(width));
            put(28, 1);
            put(76, 32);
            put(80, pfFlags);
            if (fourCC)
                std::memcpy(h.data() + 84, fourCC, 4);
            put(88, quint32(bits));
            put(92, r);
            put(96, g);
            put(100, b);
            put(104, a);
            return h;
        };
        auto decode = [](const QByteArray &file) -> QByteArray {
            QString error;
            auto doc = DdsReader().readBytes(file, "t.dds", &error);
            if (!doc)
                return "error: " + error.toUtf8();
            const auto image = doc->tables.front()->image();
            return image ? image->rgba : QByteArray("no image");
        };

        const QByteArray block = QByteArray::fromHex("00f8e007aaaaaaaa");
        QCOMPARE(decode(header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0) + block),
                 decodePixels(PixelFormat::BC1, block, 4, 4).value());

        QByteArray dx10 = header(4, 4, 0x4, "DX10", 0, 0, 0, 0, 0) + QByteArray(20, '\0');
        qToLittleEndian<quint32>(98, dx10.data() + 128);
        const QByteArray bc7(16, '\x40');
        QCOMPARE(decode(dx10 + bc7), decodePixels(PixelFormat::BC7, bc7, 4, 4).value());

        const QByteArray bgra = QByteArray::fromHex("10203040");
        QCOMPARE(decode(header(1, 1, 0x41, nullptr, 32, 0xff0000, 0xff00, 0xff, 0xff000000u) + bgra),
                 QByteArray::fromHex("30201040"));
        QCOMPARE(decode(header(1, 1, 0x40, nullptr, 24, 0xff0000, 0xff00, 0xff, 0) + QByteArray::fromHex("102030")),
                 QByteArray::fromHex("302010ff"));
        QCOMPARE(decode(header(2, 1, 0x20000, nullptr, 8, 0xff, 0, 0, 0) + QByteArray::fromHex("80ff")),
                 QByteArray::fromHex("808080ffffffffff"));
        QCOMPARE(decode(header(1, 1, 0x2, nullptr, 8, 0, 0, 0, 0xff) + QByteArray::fromHex("7f")),
                 QByteArray::fromHex("0000007f"));
        QCOMPARE(decode(header(1, 1, 0x40, nullptr, 16, 0xf800, 0x07e0, 0x001f, 0) + QByteArray::fromHex("1ff8")),
                 QByteArray::fromHex("ff00ffff"));

        QVERIFY(decode(header(4, 4, 0x4, "DXT5", 0, 0, 0, 0, 0) + QByteArray(8, '\0')).isEmpty() == false);
        QCOMPARE(decode(header(4, 4, 0x4, "DXT5", 0, 0, 0, 0, 0) + QByteArray(8, '\0')), QByteArray("no image"));
        QCOMPARE(decode(header(4, 4, 0x4, "XYZW", 0, 0, 0, 0, 0)), QByteArray("no image"));
        QVERIFY(decode(header(0, 4, 0x4, "DXT1", 0, 0, 0, 0, 0)).startsWith("error"));
        QVERIFY(decode(QByteArray("DDS ") + QByteArray(60, '\0')).startsWith("error"));

        FormatRegistry registry;
        QString error;
        const FormatReader *used = nullptr;
        QVERIFY(registry.openBytes(header(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0) + block, "font.dds", &used, &error));
        QCOMPARE(used->name(), QString("DDS texture"));
    }

    void decodesOddSizes()
    {
        // 5×3 pixels need 2×1 blocks; pixels outside the image are dropped.
        const auto rgba = decodePixels(PixelFormat::BC1, QByteArray(16, '\0'), 5, 3);
        QVERIFY(rgba);
        QCOMPARE(rgba->size(), qsizetype(5 * 3 * 4));
        QVERIFY(!decodePixels(PixelFormat::BC1, QByteArray(8, '\0'), 5, 3));  // too short
    }

    // --- XTX files -------------------------------------------------------------------------

    void readsXtx()
    {
        QString error;
        const auto doc = XtxReader::fromBytes(makeXtx(), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(1));
        const Table &t = *doc->tables[0];
        const auto image = t.image();
        QVERIFY(image);
        QCOMPARE(image->width, 8);
        QCOMPARE(image->height, 8);
        // RGBA8 = 4-byte elements: pixel (0,1) is GOB byte 16, pixel (4,0) is byte 32,
        // pixel (0,2) is byte 64 (see deswizzleFollowsGobLayout).
        QCOMPARE(pixel(image->rgba, 8, 0, 0), (Rgba{0, 1, 2, 3}));
        QCOMPARE(pixel(image->rgba, 8, 1, 0), (Rgba{4, 5, 6, 7}));
        QCOMPARE(pixel(image->rgba, 8, 0, 1), (Rgba{16, 17, 18, 19}));
        QCOMPARE(pixel(image->rgba, 8, 4, 0), (Rgba{32, 33, 34, 35}));
        QCOMPARE(pixel(image->rgba, 8, 0, 2), (Rgba{64, 65, 66, 67}));
        QCOMPARE(t.description(), QString("8 × 8"));
        QCOMPARE(t.text(2, 1, DisplayMode::Auto), QString("RGBA8  (NVN 0x25)"));
    }

    void unknownPixelFormatStillShowsProperties()
    {
        QString error;
        const auto doc = XtxReader::fromBytes(makeXtx(0x77), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table &t = *doc->tables[0];
        QVERIFY(!t.image());
        QVERIFY(t.description().contains("0x77"));
        QCOMPARE(t.text(0, 1, DisplayMode::Auto), QString("8"));
    }

    void shortPixelDataIsReportedNotRead()
    {
        QString error;
        const auto doc = XtxReader::fromBytes(makeXtx(0x25, 100), &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(!doc->tables[0]->image());
        QVERIFY(doc->tables[0]->description().contains("shorter"));
    }

    void slicesComeFromThePixelDataOnly()
    {
        // The texture info sits at 0x34: data size at +0x00, slice size at +0x24.
        auto withSlices = [](qsizetype pixelBytes, quint64 dataSize, quint32 sliceSize) {
            QByteArray b = makeXtx(0x25, pixelBytes);
            qToLittleEndian(dataSize, b.data() + 0x34);
            qToLittleEndian(sliceSize, b.data() + 0x34 + 0x24);
            return b;
        };
        QString error;

        // Two real slices of 512 bytes (8×8 RGBA8): both are shown.
        auto doc = XtxReader::fromBytes(withSlices(1024, 1024, 512), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(2));
        QVERIFY(doc->tables[0]->image());
        QVERIFY(doc->tables[1]->image());

        // Slices smaller than one picture would overlap: one picture only, not 64 copies.
        doc = XtxReader::fromBytes(withSlices(512, 64 * 8, 8), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(1));

        // The header claims more slices than the data holds.
        doc = XtxReader::fromBytes(withSlices(1024, 512 * 64, 512), &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(2));
    }

    void truncatedXtxNeverCrashes()
    {
        const QByteArray full = makeXtx();
        for (qsizetype n = 0; n < full.size(); n += 7) {
            QString error;
            XtxReader::fromBytes(full.left(n), &error);
        }
    }

    void registryPicksXtxByContent()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("texture.bin");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeXtx());
        f.close();
        FormatRegistry registry;
        QCOMPARE(registry.candidates(path).front()->name(), XtxReader().name());
    }
};

QTEST_MAIN(TestTextures)
#include "test_textures.moc"
