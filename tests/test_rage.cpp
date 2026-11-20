#include "core/registry.h"
#include "rage/awcreader.h"
#include "rage/rageresource.h"
#include "rage/rageanimation.h"
#include "rage/ragemodelreader.h"
#include "models/animationsearch.h"
#include "rage/ragestringtablereader.h"
#include "rage/ragetexturereader.h"
#include "archives/rpf6reader.h"
#include "textures/ddsreader.h"
#include "textures/texturedecode.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <qfloat16.h>
#include <QtEndian>

#include <cstring>
#include <vector>

namespace {

void put32(QByteArray &b, qsizetype at, quint32 v)
{
    qToLittleEndian(v, b.data() + at);
}

void put16(QByteArray &b, qsizetype at, quint16 v)
{
    qToLittleEndian(v, b.data() + at);
}

const QByteArray kDxt1Mip0 = QByteArray::fromHex("00f8e007aaaaaaaa1f00e00755555555e0071f00ffffffff001ff8000f0f0f0f");
const QByteArray kDxt1Mip1 = QByteArray::fromHex("ffff0000aaaaaaaa");
const QByteArray kL8 = QByteArray::fromHex("00204080a0c0e0ff");

QByteArray dictionary(bool extended)
{
    QByteArray body(0x2000, '\0');
    put32(body, 0x18, 0x50000100);
    put16(body, 0x1C, 3);
    put16(body, 0x1E, 3);
    const quint32 textures[3] = {0x200, 0x260, 0x2C0};
    for (int i = 0; i < 3; ++i)
        put32(body, 0x100 + 4 * i, 0x50000000 | textures[i]);
    auto texture = [&](quint32 at, quint32 name, int w, int h, quint32 code, int mips, quint32 data) {
        put32(body, at + 24, name);
        put16(body, at + 32, quint16(w));
        put16(body, at + 34, quint16(h));
        put32(body, at + 36, code);
        body[at + 43] = char(mips);
        put32(body, at + 76, data);
    };
    std::memcpy(body.data() + 0x400, "rock_d.dds\0dust.dds\0ui.crn\0", 27);
    texture(0x200, 0x50000400, 8, 8, qFromLittleEndian<quint32>("DXT1"), 2, 0x50000800);
    texture(0x260, 0x5000040B, 4, 2, 50, 1, 0x60000010);
    texture(0x2C0, 0x50000414, 16, 16, qFromLittleEndian<quint32>("CRND"), 1, 0x60000100);
    body.replace(0x800, kDxt1Mip0.size() + kDxt1Mip1.size(), kDxt1Mip0 + kDxt1Mip1);
    body.replace(0x1000 + 0x10, kL8.size(), kL8);

    QByteArray header(extended ? 16 : 12, '\0');
    std::memcpy(header.data(), extended ? "RSC\x85" : "RSC\x05", 4);
    put32(header, 4, 10);
    if (extended) {
        put32(header, 8, 0x80000000u);
        put32(header, 12, 1u | (1u << 14));
    } else {
        put32(header, 8, 1u | (4u << 11) | (1u << 15) | (4u << 26));
    }
    return header + body;
}

QByteArray withEmbeddedDictionary()
{
    QByteArray file = dictionary(false);
    put32(file, 4, 138);
    const int b = 12;
    put32(file, b + 0x18, 0);
    put16(file, b + 0x1C, 0);
    put16(file, b + 0x1E, 0);
    auto dict = [&](int at, quint32 keys, quint32 values, quint16 count) {
        put32(file, b + at + 12, 1);
        put32(file, b + at + 0x10, keys);
        put16(file, b + at + 0x14, count);
        put16(file, b + at + 0x16, count);
        put32(file, b + at + 0x18, values);
        put16(file, b + at + 0x1C, count);
        put16(file, b + at + 0x1E, count);
    };
    dict(0x40, 0x50000180, 0x50000100, 3);
    dict(0x80, 0x50000180, 0x500001A0, 1);
    put32(file, b + 0x1A0, 0x50000700);
    std::memcpy(file.data() + b + 0x500, "memory:$0bb18840,16512,0:tile_5.dds", 36);
    put32(file, b + 0x200 + 24, 0x50000500);
    return file;
}

void putFloat(QByteArray &b, qsizetype at, float v)
{
    quint32 bits;
    std::memcpy(&bits, &v, 4);
    put32(b, at, bits);
}

QByteArray modelResource(quint32 type)
{
    QByteArray body(0x2000, '\0');
    auto p = [](quint32 offset) { return 0x50000000u | offset; };
    put32(body, 0x08, p(0x100));
    put32(body, 0x100 + 0x08, p(0x400));
    put32(body, 0x100 + 0x0C, p(0x600));
    put32(body, 0x100 + 0x44, p(0x200));
    put32(body, 0x200, p(0x210));
    put16(body, 0x204, 2);
    put16(body, 0x206, 2);
    put32(body, 0x210, p(0x240));
    put32(body, 0x214, p(0x280));
    put32(body, 0x240 + 4, p(0x300));
    put16(body, 0x240 + 8, 1);
    put32(body, 0x240 + 0x10, p(0x2F0));
    body[0x240 + 0x19] = 1;
    put32(body, 0x280 + 4, p(0x304));
    put16(body, 0x280 + 8, 1);
    put32(body, 0x280 + 0x10, p(0x2F0));
    body[0x280 + 0x17] = 1;
    put32(body, 0x300, p(0x800));
    put32(body, 0x304, p(0x900));

    put32(body, 0x800 + 0x0C, p(0xA00));
    put32(body, 0x800 + 0x1C, p(0xB00));
    put32(body, 0x800 + 0x2C, 3);
    put16(body, 0x800 + 0x34, 3);
    body[0x800 + 0x36] = 3;
    put32(body, 0x800 + 0x38, p(0xC00));
    put16(body, 0x800 + 0x3C, 28);
    put16(body, 0x800 + 0x3E, 2);
    put16(body, 0xA00 + 4, 3);
    put32(body, 0xA00 + 0x10, p(0x1000));
    put32(body, 0xA00 + 0x18, p(0xA80));
    put32(body, 0xA80, 0x4F);
    body[0xA80 + 4] = 28;
    qToLittleEndian(quint64(6) | (quint64(9) << 4) | (quint64(9) << 8) | (quint64(10) << 12) | (quint64(1) << 24),
                    body.data() + 0xA80 + 8);
    put32(body, 0xB00 + 4, 3);
    put32(body, 0xB00 + 8, p(0x1100));
    put16(body, 0x1100, 0);
    put16(body, 0x1102, 1);
    put16(body, 0x1104, 2);
    put16(body, 0xC00, 1);
    put16(body, 0xC02, 0);
    const float corners[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int v = 0; v < 3; ++v) {
        const int at = 0x1000 + 28 * v;
        for (int k = 0; k < 3; ++k)
            putFloat(body, at + 4 * k, corners[v][k]);
        body[at + 12] = char(255);
        body[at + 16] = char(v == 1 ? 1 : 0);
        put32(body, at + 20, 511u << 20);
        const qfloat16 u(0.25f), w(0.75f);
        std::memcpy(body.data() + at + 24, &u, 2);
        std::memcpy(body.data() + at + 26, &w, 2);
    }

    put32(body, 0x900 + 0x0C, p(0xD00));
    put32(body, 0x900 + 0x1C, p(0xE00));
    put32(body, 0x900 + 0x2C, 5);
    put16(body, 0x900 + 0x34, 4);
    body[0x900 + 0x36] = 4;
    put16(body, 0x900 + 0x3C, 12);
    put16(body, 0xD00 + 4, 4);
    put32(body, 0xD00 + 0x10, p(0x1200));
    put32(body, 0xD00 + 0x18, p(0xD80));
    put32(body, 0xD80, 1);
    body[0xD80 + 4] = 12;
    body[0xD80 + 8] = 6;
    put32(body, 0xE00 + 4, 5);
    put32(body, 0xE00 + 8, p(0x1300));
    const quint16 strip[5] = {0, 1, 2, 3, 0xFFFF};
    for (int i = 0; i < 5; ++i)
        put16(body, 0x1300 + 2 * i, strip[i]);
    for (int v = 0; v < 4; ++v)
        putFloat(body, 0x1200 + 12 * v, float(v));

    put32(body, 0x400 + 8, p(0x410));
    put16(body, 0x400 + 0xC, 1);
    put32(body, 0x410, p(0x440));
    put32(body, 0x440, p(0x480));
    body[0x440 + 8] = 2;
    put32(body, 0x480 + 4, p(0x500));
    put32(body, 0x488 + 4, p(0x540));
    put32(body, 0x490, joaat("texturesampler"));
    put32(body, 0x494, joaat("bumpsampler"));
    put32(body, 0x500 + 24, p(0x580));
    put32(body, 0x540 + 24, p(0x5C0));
    std::memcpy(body.data() + 0x580, "memory:$0bb,1,0:skin_d.dds", 27);
    std::memcpy(body.data() + 0x5C0, "skin_n.dds", 11);

    put32(body, 0x600, p(0x1400));
    put16(body, 0x600 + 0x18, 2);
    put32(body, 0x1400, p(0x1600));
    putFloat(body, 0x1400 + 0x24, 1);
    putFloat(body, 0x1400 + 0x4C, 1);
    put32(body, 0x14E0, p(0x1610));
    put32(body, 0x14E0 + 0x10, p(0x1400));
    putFloat(body, 0x14E0 + 0x20, 2);
    putFloat(body, 0x14E0 + 0x48, 0.70710678f);
    putFloat(body, 0x14E0 + 0x4C, 0.70710678f);
    std::memcpy(body.data() + 0x1600, "root\0\0\0\0\0\0\0\0\0\0\0\0arm", 19);

    if (type == RageModelReader::kFragmentType) {
        put32(body, 0x08, 0);
        put32(body, 0xB0, p(0x1700));
        put32(body, 0xB4, p(0x100));
        std::memcpy(body.data() + 0x1700, "t:/rdr2/assets/entity/cart01x", 30);
    }
    QByteArray header(12, '\0');
    std::memcpy(header.data(), "RSC\x05", 4);
    put32(header, 4, type);
    put32(header, 8, 2u | (4u << 11));
    return header + body;
}

QByteArray animationSet()
{
    QByteArray body(0x1000, '\0');
    auto p = [](quint32 offset) { return 0x50000000u | offset; };
    put32(body, 0x14, p(0x40));
    std::memcpy(body.data() + 0x38, "test.ast", 9);
    put32(body, 0x40 + 0x14, p(0x80));
    put16(body, 0x40 + 0x18, 1);
    put32(body, 0x80, p(0x90));
    put32(body, 0x90 + 4, p(0xA0));
    put32(body, 0xA0 + 4, 1);
    put32(body, 0xA0 + 8, p(0xF00));
    std::memcpy(body.data() + 0xF00, "Missions\\test\\wave", 19);
    put32(body, 0xA0 + 0x28, p(0x100));
    putFloat(body, 0xA0 + 0x2C, 1.0f / 30);
    putFloat(body, 0xA0 + 0x30, 0.1f);
    put16(body, 0x100 + 8, 4);
    put16(body, 0x100 + 0x0A, 2);
    putFloat(body, 0x100 + 0x0C, 0.1f);
    put32(body, 0x100 + 0x14, p(0x140));
    put16(body, 0x100 + 0x18, 2);
    put32(body, 0x140, p(0x150));
    put32(body, 0x144, p(0x170));
    const quint32 chunkLists[2] = {0x190, 0x1A0};
    const quint32 chunks[2][3] = {{0x200, 0x220, 0x240}, {0x260, 0x280, 0x2A0}};
    const quint32 rotation[2] = {0x300, 0x340}, scaled[2] = {0x380, 0x3C0}, rotationBits[2] = {0x3A0, 0x3E0};
    const quint32 quantized[2] = {0x400, 0x440}, quantizedBits[2] = {0x480, 0x490};
    const quint32 rotationValues[2] = {0u | (3u << 4), 5u | (6u << 4)};
    const quint32 translationValues[2] = {0u | (2u << 8), 4u | (6u << 8)};
    for (int b = 0; b < 2; ++b) {
        const quint32 block = b == 0 ? 0x150 : 0x170;
        put32(body, block + 4, p(chunkLists[b]));
        put16(body, block + 8, 3);
        for (int c = 0; c < 3; ++c)
            put32(body, chunkLists[b] + 4 * c, p(chunks[b][c]));
        const quint32 rot = chunks[b][0], move = chunks[b][1], other = chunks[b][2];
        body[rot] = 1;
        body[rot + 1] = 1;
        put16(body, rot + 2, 10);
        put32(body, rot + 4, p(rotation[b]));
        body[rotation[b] + 5] = 14;
        put32(body, rotation[b] + 8 + 4, p(scaled[b]));
        put32(body, rotation[b] + 0x20, 3);
        putFloat(body, scaled[b], 0.1f);
        put32(body, scaled[b] + 4, p(rotationBits[b]));
        put32(body, scaled[b] + 8, 4);
        put32(body, scaled[b] + 12, 2);
        put32(body, rotationBits[b], rotationValues[b]);
        body[move] = 0;
        body[move + 1] = 4;
        put16(body, move + 2, 20);
        put32(body, move + 4, p(quantized[b]));
        put32(body, move + 8, p(0x4A0));
        put32(body, move + 12, p(0x4C0));
        body[quantized[b] + 5] = 6;
        put32(body, quantized[b] + 8, p(quantizedBits[b]));
        put32(body, quantized[b] + 12, 8);
        put32(body, quantized[b] + 16, 2);
        putFloat(body, quantized[b] + 0x14, 0.5f);
        putFloat(body, quantized[b] + 0x18, 1.0f);
        put32(body, quantizedBits[b], translationValues[b]);
        body[other] = 1;
        body[other + 1] = 1;
        put16(body, other + 2, 99);
        put32(body, other + 4, p(0x500));
    }
    body[0x4A0 + 5] = 4;
    putFloat(body, 0x4A0 + 8, 2.5f);
    body[0x4C0 + 5] = 4;
    putFloat(body, 0x4C0 + 8, 7.0f);
    body[0x500 + 5] = 9;
    put32(body, 0x500 + 8, p(0x520));
    putFloat(body, 0x520 + 12, 1.0f);
    QByteArray header(12, '\0');
    std::memcpy(header.data(), "RSC\x05", 4);
    put32(header, 4, 6);
    put32(header, 8, 1u | (4u << 11));
    return header + body;
}

Model3D animatedModel()
{
    Model3D model;
    ModelNode root;
    root.name = "root";
    root.boneId = 10;
    ModelNode arm;
    arm.name = "arm";
    arm.boneId = 20;
    arm.parent = 0;
    model.nodes = {root, arm};
    return model;
}

QByteArray dspHeader()
{
    QByteArray h(0x60, '\0');
    put16(h, 0x1C, 2048);
    return h;
}

QByteArray dspFrame(int nibble)
{
    QByteArray f(8, char((nibble << 4) | nibble));
    f[0] = 0;
    return f;
}

quint64 awcTag(quint8 type, quint32 size, quint32 offset)
{
    return (quint64(type) << 56) | (quint64(size) << 28) | offset;
}

QByteArray awc(const std::vector<quint32> &infos, const std::vector<std::vector<quint64>> &tags, const QByteArray &payload,
               int payloadAt)
{
    QByteArray out(payloadAt, '\0');
    std::memcpy(out.data(), "ADAT", 4);
    put32(out, 4, 0xFF000001u);
    put32(out, 8, quint32(infos.size()));
    put32(out, 12, quint32(payloadAt));
    qsizetype at = 0x10;
    for (quint32 info : infos) {
        put32(out, at, info);
        at += 4;
    }
    for (const auto &list : tags)
        for (quint64 tag : list) {
            qToLittleEndian(tag, out.data() + at);
            at += 8;
        }
    return out + payload;
}

QByteArray stringTable()
{
    QByteArray body(0x1000, '\0');
    put32(body, 0x08, 0x50000020);
    put32(body, 0x20, 3);
    put32(body, 0x24, 0x50000040);
    put16(body, 0x28, 3);
    put16(body, 0x2A, 3);
    put32(body, 0x2C, 3);
    put32(body, 0x40, 0x50000100);
    put32(body, 0x48, 0x50000120);
    const quint32 hashes[3] = {0x11111111, 0x22222222, 0x33333333};
    const quint32 entries[3] = {0x100, 0x110, 0x120};
    const quint32 nexts[3] = {0x50000110, 0, 0};
    const char *texts[3] = {"Hello\nthere", "Adi\xc3\xb3s", "C"};
    for (int i = 0; i < 3; ++i) {
        put32(body, entries[i], hashes[i]);
        put32(body, entries[i] + 4, 0x50000200 + 0x10 * i);
        put32(body, entries[i] + 8, nexts[i]);
        put32(body, 0x200 + 0x10 * i, hashes[i]);
        put32(body, 0x204 + 0x10 * i, 0x50000300 + 0x20 * i);
        std::memcpy(body.data() + 0x300 + 0x20 * i, texts[i], std::strlen(texts[i]));
    }
    QByteArray header(12, '\0');
    std::memcpy(header.data(), "RSC\x05", 4);
    put32(header, 4, 1);
    put32(header, 8, 1u | (4u << 11));
    return header + body;
}

struct LanguageString
{
    QByteArray name;
    QString text;
    QByteArray note = {};
};

QByteArray languageTable(const std::vector<std::vector<LanguageString>> &languages, const std::vector<int> &aliasOf,
                         const QList<QByteArray> &identifiers)
{
    const int count = int(aliasOf.size());
    QByteArray out(4 + 4 * count, '\0');
    put32(out, 0, quint32(count));
    QByteArray word(4, '\0');
    auto append32 = [&](quint32 v) {
        put32(word, 0, v);
        out += word;
    };
    append32(0x200);
    append32(quint32(identifiers.size()));
    for (const QByteArray &id : identifiers) {
        append32(quint32(id.size()));
        out += id + '\0';
    }
    std::vector<quint32> positions;
    for (int i = 0; i < count; ++i) {
        if (aliasOf[i] >= 0) {
            positions.push_back(positions[size_t(aliasOf[i])]);
            continue;
        }
        if (aliasOf[i] == -2) {
            positions.push_back(0);
            continue;
        }
        positions.push_back(quint32(out.size()));
        const auto &strings = languages[size_t(i)];
        append32(quint32(strings.size()));
        for (const LanguageString &s : strings) {
            append32(joaat(s.name.toLower()));
            out += QByteArray(s.note.isEmpty() ? "\x01\x00" : "\x04\x00", 2);
            append32(quint32(s.note.size()));
            out += s.note;
            const QString text = s.text + QChar(u'\0');
            append32(quint32(text.size()));
            for (QChar ch : text) {
                QByteArray unit(2, '\0');
                put16(unit, 0, ch.unicode());
                out += unit;
            }
            out += QByteArray::fromHex("0000803f0000803f0000");
        }
    }
    for (int i = 0; i < count; ++i)
        put32(out, 4 + 4 * i, aliasOf[i] == -2 ? quint32(out.size()) : positions[size_t(i)]);
    return out;
}

}

class TestRage : public QObject
{
    Q_OBJECT

private slots:
    void readsResourceHeaders()
    {
        QString error;
        const auto plain = readRageResource(dictionary(false), &error);
        QVERIFY2(plain, qPrintable(error));
        QCOMPARE(plain->type, 10u);
        QCOMPARE(plain->virtualSize, qint64(0x1000));
        QCOMPARE(plain->physicalSize, qint64(0x1000));
        QCOMPARE(plain->offsetOf(0x50000010).value_or(-1), qint64(0x10));
        QCOMPARE(plain->offsetOf(0x60000010).value_or(-1), qint64(0x1010));
        QVERIFY(!plain->offsetOf(0x50001000));
        QVERIFY(!plain->offsetOf(0x70000000));
        QCOMPARE(plain->cstring(0x50000400), QString("rock_d.dds"));

        const auto extended = readRageResource(dictionary(true), &error);
        QVERIFY2(extended, qPrintable(error));
        QCOMPARE(extended->virtualSize, qint64(0x1000));
        QCOMPARE(extended->physicalSize, qint64(0x1000));

        QVERIFY(!readRageResource(dictionary(false).left(0x800), &error));
        QVERIFY(!readRageResource("RSC", &error));
    }

    void readsTextureDictionaries()
    {
        for (bool extended : {false, true}) {
            QString error;
            FormatRegistry registry;
            const FormatReader *used = nullptr;
            auto doc = registry.openBytes(dictionary(extended), "rock.wtd", &used, &error);
            QVERIFY2(doc, qPrintable(error));
            QCOMPARE(used->name(), QString("RAGE texture dictionary"));
            QCOMPARE(doc->tables.size(), size_t(4));
            const auto &list = doc->tables.front();
            QCOMPARE(list->rowCount(), 3);
            QCOMPARE(list->text(0, 0, DisplayMode::Auto), QString("rock_d.dds"));
            QCOMPARE(list->text(0, 3, DisplayMode::Auto), QString("2"));
            QCOMPARE(list->text(1, 2, DisplayMode::Auto), QString("L8"));
            QVERIFY(list->description().contains("cannot be shown"));

            const auto rock = doc->tables[1]->image();
            QVERIFY(rock);
            QCOMPARE(rock->rgba, decodePixels(PixelFormat::BC1, kDxt1Mip0, 8, 8).value());
            const auto dust = doc->tables[2]->image();
            QVERIFY(dust);
            QCOMPARE(dust->rgba.mid(4, 4), QByteArray::fromHex("202020ff"));
            QVERIFY(!doc->tables[3]->hasImage());

            const auto archive = list->archive();
            QCOMPARE(archive->entries[0].name, QString("rock_d.dds"));
            const QByteArray dds = archive->entryData(0);
            QCOMPARE(dds.mid(128), kDxt1Mip0 + kDxt1Mip1);
            auto reopened = DdsReader().readBytes(dds, "rock_d.dds", &error);
            QVERIFY2(reopened, qPrintable(error));
            QCOMPARE(reopened->tables.front()->image()->rgba, rock->rgba);
            auto greyDds = DdsReader().readBytes(archive->entryData(1), "dust.dds", &error);
            QVERIFY(greyDds);
            QCOMPARE(greyDds->tables.front()->image()->rgba, dust->rgba);
            QVERIFY(archive->entryData(2).isEmpty());
        }
    }

    void decodesAwcSoundPacks()
    {
        QByteArray sfx(0x14, '\0');
        put32(sfx, 0, 28);
        put16(sfx, 8, 24000);
        sfx[0x13] = 0x0C;
        const QByteArray data = dspHeader() + dspFrame(1) + dspFrame(1);
        const int base = 0x100;
        QByteArray payload = sfx + data;
        const QByteArray file = awc({(2u << 29) | 0x1234567, (1u << 29) | 0x7654321},
                                    {{awcTag(0xFA, 0x14, base), awcTag(0x55, quint32(data.size()), base + 0x14)},
                                     {awcTag(0xFA, 0x14, base)}},
                                    payload, base);
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(file, "voice.awc", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE audio container"));
        const auto &table = doc->tables.front();
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->text(0, 0, DisplayMode::Auto), QString("01234567.wav"));
        QCOMPARE(table->text(0, 4, DisplayMode::Auto), QString("DSP ADPCM"));
        QVERIFY(table->hasAudio());
        const std::optional<Audio> audio = table->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->sampleRate, 24000);
        QCOMPARE(audio->frameCount(), qint64(28));
        QCOMPARE(qFromLittleEndian<qint16>(audio->pcm.constData() + 2 * 27), qint16(28));
        const QByteArray wav = table->archive()->entryData(0);
        QVERIFY(wav.startsWith("RIFF"));
    }

    void eachSoundOfAPackCanBePlayed()
    {
        QByteArray first(0x14, '\0'), second(0x14, '\0');
        put32(first, 0, 28);
        put16(first, 8, 24000);
        first[0x13] = 0x0C;
        put32(second, 0, 14);
        put16(second, 8, 32000);
        second[0x13] = 0x0C;
        const QByteArray data = dspHeader() + dspFrame(1) + dspFrame(1);
        const int base = 0x100;
        const int secondAt = base + 0x14 + int(data.size());
        const QByteArray payload = first + data + second + data;
        const QByteArray file = awc({(2u << 29) | 0x1111111, (2u << 29) | 0x2222222},
                                    {{awcTag(0xFA, 0x14, base), awcTag(0x55, quint32(data.size()), base + 0x14)},
                                     {awcTag(0xFA, 0x14, quint32(secondAt)),
                                      awcTag(0x55, quint32(data.size()), quint32(secondAt + 0x14))}},
                                    payload, base);
        QString error;
        FormatRegistry registry;
        auto doc = registry.openBytes(file, "pack.awc", nullptr, &error);
        QVERIFY2(doc, qPrintable(error));
        const auto &table = doc->tables.front();
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(!table->hasAudio());
        QVERIFY(table->hasRowAudio());
        const std::optional<Audio> one = table->decodeRowAudio(1, &error);
        QVERIFY2(one, qPrintable(error));
        QCOMPARE(one->sampleRate, 32000);
        QCOMPARE(one->frameCount(), qint64(14));
        QVERIFY(!table->decodeRowAudio(2, &error));

        const FormatReader *used = nullptr;
        auto entry = registry.openEntry(*table->archive(), 0, &used, &error);
        QVERIFY2(entry, qPrintable(error));
        QCOMPARE(used->name(), QString("WAV sound (PCM)"));
        QVERIFY(entry->tables.front()->hasAudio());
        const std::optional<Audio> played = entry->tables.front()->decodeAudio(&error);
        QVERIFY2(played, qPrintable(error));
        QCOMPARE(played->sampleRate, 24000);
        QCOMPARE(played->frameCount(), qint64(28));
    }

    void decodesAwcMusicAcrossBlocks()
    {
        const int channels = 2;
        QByteArray header(0x0C + 0x10 * channels, '\0');
        const int blockSize = 0x10 * channels + 0x70 * channels + 16;
        put32(header, 0, 2);
        put32(header, 4, quint32(blockSize));
        put32(header, 8, channels);
        for (int ch = 0; ch < channels; ++ch) {
            put32(header, 0x0C + 0x10 * ch + 4, 28);
            put16(header, 0x0C + 0x10 * ch + 0x0A, 32000);
            header[0x0C + 0x10 * ch + 0x0C] = 0x10;
        }
        QByteArray blocks;
        for (int b = 0; b < 2; ++b) {
            QByteArray block(0x10 * channels + 0x70 * channels, '\0');
            for (int ch = 0; ch < channels; ++ch) {
                put32(block, 0x10 * ch + 4, 1);
                put32(block, 0x10 * ch + 0x0C, 14);
                block.replace(0x10 * channels + 0x70 * ch + 0x10, 0x60, dspHeader());
            }
            blocks += block + dspFrame(1) + dspFrame(2);
        }
        const int base = 0x80;
        const QByteArray file = awc({(2u << 29), 0x111, 0x222},
                                    {{awcTag(0x48, quint32(header.size()), base),
                                      awcTag(0x55, quint32(blocks.size()), base + quint32(header.size()))}},
                                    header + blocks, base);
        QString error;
        auto doc = AwcReader().readBytes(file, "theme.awc", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto &table = doc->tables.front();
        QCOMPARE(table->text(0, 0, DisplayMode::Auto), QString("theme.wav"));
        QVERIFY(table->description().contains("music"));
        const std::optional<Audio> audio = table->decodeAudio(&error);
        QVERIFY2(audio, qPrintable(error));
        QCOMPARE(audio->channels, 2);
        QCOMPARE(audio->sampleRate, 32000);
        QCOMPARE(audio->frameCount(), qint64(28));
        auto sample = [&](int frame, int ch) { return qFromLittleEndian<qint16>(audio->pcm.constData() + (frame * 2 + ch) * 2); };
        QCOMPARE(sample(0, 0), qint16(1));
        QCOMPARE(sample(0, 1), qint16(2));
        QCOMPARE(sample(13, 0), qint16(14));
        QCOMPARE(sample(14, 0), qint16(15));
        QCOMPARE(sample(14, 1), qint16(30));
        QCOMPARE(sample(27, 1), qint16(56));
    }

    void rejectsDamagedAwc()
    {
        QString error;
        QVERIFY(!AwcReader().readBytes("ADAT", "x.awc", &error));
        QByteArray badFlags = awc({(0u << 29) | 1}, {{}}, {}, 0x40);
        put32(badFlags, 4, 0x12345678);
        QVERIFY(!AwcReader().readBytes(badFlags, "x.awc", &error));
        QByteArray encrypted = awc({(0u << 29) | 1}, {{}}, {}, 0x40);
        put32(encrypted, 4, 0xFF080001u);
        QVERIFY(!AwcReader().readBytes(encrypted, "x.awc", &error));
        QVERIFY(error.contains("encrypted"));
        QByteArray cut = awc({(3u << 29) | 1}, {{1, 2, 3}}, {}, 0x40).left(0x14);
        QVERIFY(!AwcReader().readBytes(cut, "x.awc", &error));
    }

    void rejectsDamagedDictionaries()
    {
        QString error;
        QByteArray wrongType = dictionary(false);
        put32(wrongType, 4, 138);
        QVERIFY(!RageTextureDictionaryReader().readBytes(wrongType, "x.wtd", &error));
        QByteArray badValues = dictionary(false);
        put32(badValues, 12 + 0x18, 0x50FFFFFF);
        QVERIFY(!RageTextureDictionaryReader().readBytes(badValues, "x.wtd", &error));
        QByteArray badTexture = dictionary(false);
        put32(badTexture, 12 + 0x100, 0x5000FFF0);
        auto doc = RageTextureDictionaryReader().readBytes(badTexture, "x.wtd", &error);
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->tables.front()->description().contains("damaged"));
    }
    void findsTexturesEmbeddedInOtherResources()
    {
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(withEmbeddedDictionary(), "wagon.wft", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE texture dictionary"));
        const auto &list = doc->tables.front();
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(list->text(0, 0, DisplayMode::Auto), QString("tile_5.dds"));
        QCOMPARE(list->text(1, 0, DisplayMode::Auto), QString("dust.dds"));
        QVERIFY(list->description().contains("type 138"));
        QCOMPARE(doc->tables[1]->image()->rgba, decodePixels(PixelFormat::BC1, kDxt1Mip0, 8, 8).value());

        QByteArray none = dictionary(false);
        put32(none, 4, 138);
        put32(none, 12 + 0x18, 0);
        QVERIFY(!RageTextureDictionaryReader().readBytes(none, "empty.wft", &error));
        QVERIFY(error.contains("no textures"));
        QVERIFY(RageTextureDictionaryReader().score("x.wft", none) > 0);
        QVERIFY(RageTextureDictionaryReader().score("x.wft", none) < 10);
    }

    void readsRageModels()
    {
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(modelResource(RageModelReader::kFragmentDrawableType), "farmer.wfd", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE model"));
        QCOMPARE(doc->tables.size(), size_t(1));
        const auto &table = doc->tables.front();
        QVERIFY(table->hasModel());
        QVERIFY(table->description().contains("2 texture"));
        const std::shared_ptr<const Model3D> model = table->model();
        QVERIFY(model);
        QCOMPARE(model->nodes.size(), size_t(2));
        QCOMPARE(model->nodes[0].name, QString("root"));
        QCOMPARE(model->nodes[1].name, QString("arm"));
        QCOMPARE(model->nodes[1].parent, 0);
        QCOMPARE(model->nodes[0].translation[1], 1.0f);
        const Matrix4 arm = model->worldMatrix(1);
        QCOMPARE(arm[12], 2.0f);
        QCOMPARE(arm[13], 1.0f);

        QCOMPARE(model->meshes.size(), size_t(2));
        QCOMPARE(model->meshes[0].joints, (std::vector<int>{1, 0}));
        QCOMPARE(model->meshes[1].node, 1);
        QVERIFY(model->meshes[1].joints.empty());

        QCOMPARE(model->primitives.size(), size_t(2));
        const ModelPrimitive &skinned = model->primitives[0];
        QCOMPARE(skinned.vertexCount(), qsizetype(3));
        QCOMPARE(skinned.indices, (std::vector<quint32>{0, 1, 2}));
        QCOMPARE(skinned.positions[3], 1.0f);
        QVERIFY(std::abs(skinned.normals[2] - 1.0f) < 1e-6f);
        QCOMPARE(skinned.uv0[0], 0.25f);
        QCOMPARE(skinned.uv0[1], 0.25f);
        QCOMPARE(skinned.joints[4], quint16(1));
        QCOMPARE(skinned.weights[0], 1.0f);
        QCOMPARE(skinned.weights[1], 0.0f);

        const ModelPrimitive &strip = model->primitives[1];
        QCOMPARE(strip.indices, (std::vector<quint32>{0, 1, 2, 2, 1, 3}));
        QVERIFY(strip.uv0.empty());

        QCOMPARE(model->materials.size(), size_t(1));
        QCOMPARE(skinned.material, 0);
        QCOMPARE(strip.material, 0);
        const ModelMaterial &material = model->materials[0];
        QCOMPARE(model->textures.at(size_t(material.baseColorTexture)).name, QString("skin_d.dds"));
        QCOMPARE(model->textures.at(size_t(material.normalTexture)).name, QString("skin_n.dds"));

        auto fragment = registry.openBytes(modelResource(RageModelReader::kFragmentType), "cart.wft", &used, &error);
        QVERIFY2(fragment, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE model"));
        QVERIFY(fragment->tables.front()->model()->meshes[0].name.startsWith("cart01x"));
    }

    void readsWideAlignedVertexLayouts()
    {
        QByteArray file = modelResource(RageModelReader::kFragmentDrawableType);
        const int b = 12;
        put16(file, b + 0x900 + 0x3C, 48);
        put32(file, b + 0xD80, 0x51);
        file[b + 0xD80 + 4] = 48;
        qToLittleEndian(quint64(7) | (quint64(9) << 16) | (quint64(7) << 24), file.data() + b + 0xD80 + 8);
        for (int v = 0; v < 4; ++v) {
            const int at = b + 0x1200 + 48 * v;
            std::memset(file.data() + at, 0, 48);
            putFloat(file, at, float(v));
            file[at + 16] = char(0x30);
            file[at + 18] = char(0x10);
            putFloat(file, at + 32, 0.5f);
        }
        QString error;
        auto doc = RageModelReader().readBytes(file, "cloth.wft", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QCOMPARE(model->primitives.size(), size_t(2));
        const ModelPrimitive &cloth = model->primitives[1];
        QCOMPARE(cloth.positions[9], 3.0f);
        QCOMPARE(cloth.uv0.size(), size_t(8));
        QCOMPARE(cloth.uv0[6], 0.5f);
        QCOMPARE(cloth.uv0[7], 1.0f);
        QCOMPARE(cloth.colors.size(), size_t(16));
        QCOMPARE(cloth.colors[0], quint8(0x10));
        QCOMPARE(cloth.colors[2], quint8(0x30));
    }

    void rejectsDamagedRageModels()
    {
        QString error;
        QByteArray badLayout = modelResource(RageModelReader::kFragmentDrawableType);
        badLayout[12 + 0xA80 + 4] = 24;
        auto doc = RageModelReader().readBytes(badLayout, "x.wfd", &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.front()->model()->meshes.size(), size_t(1));
        QVERIFY(doc->tables.front()->text(8, 0, DisplayMode::Auto).startsWith("parts"));

        QByteArray noLod = modelResource(RageModelReader::kFragmentDrawableType);
        put32(noLod, 12 + 0x100 + 0x44, 0);
        QVERIFY(!RageModelReader().readBytes(noLod, "x.wfd", &error));
        QByteArray loop = modelResource(RageModelReader::kFragmentDrawableType);
        put32(loop, 12 + 0x1400 + 0x10, 0x500014E0);
        doc = RageModelReader().readBytes(loop, "x.wfd", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QVERIFY(model->nodes[0].parent == -1 || model->nodes[1].parent == -1);
        QByteArray texture = dictionary(false);
        QCOMPARE(RageModelReader().score("x.wtd", texture), 0);
        QVERIFY(!RageModelReader().readBytes(stringTable(), "x.sst", &error));
    }

    void findsExtendedObjectStart()
    {
        QByteArray file(16 + 0x3000, '\0');
        std::memcpy(file.data(), "RSC\x85", 4);
        put32(file, 4, 133);
        put32(file, 8, 0x80000000u);
        put32(file, 12, 3u);
        QString error;
        const auto r = readRageResource(file, &error);
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->virtualSize, qint64(0x3000));
        QCOMPARE(r->objectStart, qint64(0x2000));
    }

    void readsRageAnimationClips()
    {
        QString error;
        const auto clips = rageAnimationClips(animationSet(), &error);
        QVERIFY2(clips, qPrintable(error));
        QCOMPARE(clips->size(), size_t(1));
        QCOMPARE(clips->front().name, QString("Missions/test/wave"));
        QCOMPARE(clips->front().frames, 3);
        QCOMPARE(clips->front().tracks, 3);
        QCOMPARE(clips->front().boneIds, (std::vector<int>{10, 20, 99}));

        const Model3D model = animatedModel();
        const auto animation = readRageAnimationClip(animationSet(), "Missions/test/wave", model, &error);
        QVERIFY2(animation, qPrintable(error));
        QCOMPARE(animation->name, QString("wave"));
        QCOMPARE(animation->channels.size(), size_t(2));
        const AnimationChannel &rotation = animation->channels[0];
        QCOMPARE(rotation.node, 0);
        QCOMPARE(rotation.path, AnimationPath::Rotation);
        QCOMPARE(rotation.times.size(), size_t(3));
        QVERIFY(std::abs(rotation.times[2] - 2.0f / 30) < 1e-6f);
        const float xs[3] = {0.3f, 0.5f, 0.6f};
        for (int k = 0; k < 3; ++k) {
            QVERIFY(std::abs(rotation.values[size_t(4 * k)] - xs[k]) < 1e-5f);
            QVERIFY(std::abs(rotation.values[size_t(4 * k + 3)] - std::sqrt(1 - xs[k] * xs[k])) < 1e-5f);
        }
        const AnimationChannel &move = animation->channels[1];
        QCOMPARE(move.node, 1);
        QCOMPARE(move.path, AnimationPath::Translation);
        QCOMPARE(move.values, (std::vector<float>{2, 2.5f, 7, 3, 2.5f, 7, 4, 2.5f, 7}));

        QVERIFY(!readRageAnimationClip(animationSet(), "missing", model, &error));
        QVERIFY(!readRageAnimationClip(animationSet(), "Missions/test/wave", Model3D{}, &error));

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(animationSet(), "test.was", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE animation set"));
        QCOMPARE(doc->tables.front()->text(0, 0, DisplayMode::Auto), QString("Missions/test/wave"));
        QCOMPARE(doc->tables.front()->text(0, 1, DisplayMode::Auto), QString("3"));
    }

    void exportsRageClipsThroughAnimationSearch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("test.was");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(animationSet());
        file.close();
        const Model3D model = animatedModel();
        const std::vector<AnimationFile> found = inspectAnimationFile(path, model);
        QCOMPARE(found.size(), size_t(1));
        QCOMPARE(found.front().matchedBones, 2);
        QCOMPARE(found.front().bones, 3);
        QCOMPARE(found.front().frames, 3);
        QStringList problems;
        const std::vector<ModelAnimation> loaded = loadAnimations(found, model, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
        QCOMPARE(loaded.size(), size_t(1));
        QCOMPARE(loaded.front().channels.size(), size_t(2));
        QVERIFY(inspectAnimationFile(path, Model3D{}).empty());
    }

    void rejectsDamagedAnimationSets()
    {
        QString error;
        QByteArray badChannel = animationSet();
        badChannel[12 + 0x300 + 5] = 19;
        const auto clips = rageAnimationClips(badChannel, &error);
        QVERIFY(clips);
        QVERIFY(!readRageAnimationClip(badChannel, "Missions/test/wave", animatedModel(), &error));
        QByteArray loop = animationSet();
        put32(loop, 12 + 0x90 + 8, 0x50000090);
        QVERIFY(!rageAnimationClips(loop, &error));
        QByteArray noFrames = animationSet();
        put16(noFrames, 12 + 0x100 + 8, 0);
        QVERIFY(rageAnimationClips(noFrames, &error)->empty());
        QVERIFY(!RageAnimationReader().readBytes(noFrames, "x.was", &error));
        QVERIFY(!rageAnimationClips(dictionary(false), &error));
    }

    void readsStringTableResources()
    {
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(stringTable(), "subtitle_r4-en.sst", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE string table"));
        const auto &table = doc->tables.front();
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->text(0, 0, DisplayMode::Auto), QString("0x11111111"));
        QCOMPARE(table->text(0, 1, DisplayMode::Auto), QString("Hello ↵ there"));
        QCOMPARE(table->text(1, 0, DisplayMode::Auto), QString("0x22222222"));
        QCOMPARE(table->text(1, 1, DisplayMode::Auto), QString::fromUtf8("Adiós"));
        QCOMPARE(table->text(2, 0, DisplayMode::Auto), QString("0x33333333"));
        QVERIFY(table->description().contains("English"));
        const QJsonObject json = QJsonDocument::fromJson(table->exportJson()).object();
        QCOMPARE(json.value("0x11111111").toString(), QString("Hello\nthere"));

        QVERIFY(RageStringTableReader().score("dialogue.wst", stringTable()) == 100);
        QVERIFY(RageStringTableReader().score("unknown.bin", stringTable()) > 0);
        QByteArray drawable = stringTable();
        put32(drawable, 12 + 0x0C, 0x50000800);
        QCOMPARE(RageStringTableReader().score("model.wfd", drawable), 0);
    }

    void rejectsDamagedStringTableResources()
    {
        QString error;
        QByteArray loop = stringTable();
        put32(loop, 12 + 0x110 + 8, 0x50000100);
        QVERIFY(!RageStringTableReader().readBytes(loop, "x.sst", &error));
        QByteArray badText = stringTable();
        put32(badText, 12 + 0x204, 0x50FFFFFF);
        QVERIFY(!RageStringTableReader().readBytes(badText, "x.sst", &error));
        QByteArray badSlots = stringTable();
        put16(badSlots, 12 + 0x28, 7);
        QVERIFY(!RageStringTableReader().readBytes(badSlots, "x.sst", &error));
        QVERIFY(!RageStringTableReader().readBytes(stringTable().left(0x400), "x.sst", &error));
    }

    void readsLanguageTables()
    {
        const std::vector<std::vector<LanguageString>> languages{
            {{"GREETING", "Howdy\nfriend"}, {"FAREWELL", "So long"}},
            {{"FAREWELL", QString::fromUtf8("Adiós")}, {"GREETING", "Hola"}, {"EXTRA", "Sin nombre"}},
            {},
            {{"GREETING", QString::fromUtf8("Привет"), "MISC"}},
            {},
        };
        const QByteArray file = languageTable(languages, {-1, -1, 1, -1, -2}, {"GREETING", "FAREWELL"});
        QString error;
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        auto doc = registry.openBytes(file, "global.strtbl", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("RAGE multi-language string table"));
        const auto &table = doc->tables.front();
        QCOMPARE(table->columnCount(), 5);
        QCOMPARE(table->columnName(1), QString("note"));
        QCOMPARE(table->columnName(2), QString("English"));
        QCOMPARE(table->columnName(3), QString("Spanish"));
        QCOMPARE(table->columnName(4), QString("German"));
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->text(0, 0, DisplayMode::Auto), QString("GREETING"));
        QCOMPARE(table->text(0, 1, DisplayMode::Auto), QString("MISC"));
        QCOMPARE(table->text(0, 2, DisplayMode::Auto), QString("Howdy ↵ friend"));
        QCOMPARE(table->text(0, 3, DisplayMode::Auto), QString("Hola"));
        QCOMPARE(table->text(0, 4, DisplayMode::Auto), QString::fromUtf8("Привет"));
        QCOMPARE(table->text(1, 3, DisplayMode::Auto), QString::fromUtf8("Adiós"));
        QCOMPARE(table->text(1, 4, DisplayMode::Auto), QString());
        QCOMPARE(table->text(2, 0, DisplayMode::Auto), QStringLiteral("0x%1").arg(joaat("extra"), 8, 16, QLatin1Char('0')));
        QVERIFY(table->description().contains("in 3 language"));
        const QJsonObject json = QJsonDocument::fromJson(table->exportJson()).object();
        QCOMPARE(json.value("English").toObject().value("GREETING").toString(), QString("Howdy\nfriend"));
        QCOMPARE(json.value("Spanish").toObject().value("FAREWELL").toString(), QString::fromUtf8("Adiós"));

        QCOMPARE(RageLanguageTableReader().score("data.bin", file), 10);
        QCOMPARE(RageLanguageTableReader().score("data.bin", QByteArray("RSC\x05\x01\x00\x00\x00", 8)), 0);
    }

    void rejectsDamagedLanguageTables()
    {
        const QByteArray file = languageTable({{{"A", "a"}}}, {-1}, {"A"});
        QString error;
        QVERIFY(!RageLanguageTableReader().readBytes(file.left(file.size() - 4), "x.strtbl", &error));
        QByteArray badId = file;
        put32(badId, 4 + 4 + 8, 0x7FFFFFFF);
        QVERIFY(!RageLanguageTableReader().readBytes(badId, "x.strtbl", &error));
        QByteArray badPosition = file;
        put32(badPosition, 4, 12);
        QVERIFY(!RageLanguageTableReader().readBytes(badPosition, "x.strtbl", &error));
        QByteArray badVersion = file;
        put32(badVersion, 8, 7);
        QVERIFY(!RageLanguageTableReader().readBytes(badVersion, "x.strtbl", &error));
    }
};

QTEST_GUILESS_MAIN(TestRage)
#include "test_rage.moc"
