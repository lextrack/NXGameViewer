#pragma once

#include "sample_bntx.h"

#include <QByteArray>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <vector>
#include <numbers>

namespace samplebfres {

class Builder
{
public:
    QByteArray b;

    qsizetype reserve(qsizetype size, qsizetype align = 8)
    {
        while (b.size() % align)
            b.append('\0');
        const qsizetype at = b.size();
        b.append(QByteArray(size, '\0'));
        return at;
    }
    void u8(qsizetype at, quint8 v) { b[at] = char(v); }
    void u16(qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); }
    void s16(qsizetype at, qint16 v) { qToLittleEndian(v, b.data() + at); }
    void u32(qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); }
    void u64(qsizetype at, quint64 v) { qToLittleEndian(v, b.data() + at); }
    void f32(qsizetype at, float v)
    {
        quint32 bits;
        std::memcpy(&bits, &v, 4);
        u32(at, bits);
    }
    void magic(qsizetype at, const char *m) { std::memcpy(b.data() + at, m, 4); }
    qsizetype str(const QByteArray &s)
    {
        const qsizetype at = reserve(2 + s.size() + 1, 2);
        u16(at, quint16(s.size()));
        std::memcpy(b.data() + at + 2, s.constData(), size_t(s.size()));
        return at;
    }
    qsizetype dict(const std::vector<QByteArray> &names)
    {
        std::vector<qsizetype> strings;
        for (const QByteArray &n : names)
            strings.push_back(str(n));
        const qsizetype at = reserve(0x18 + qsizetype(names.size()) * 0x10);
        u32(at + 4, quint32(names.size()));
        for (size_t i = 0; i < names.size(); ++i)
            u64(at + 0x18 + qsizetype(i) * 0x10 + 8, quint64(strings[i]));
        return at;
    }
};

inline QByteArray make(bool withClip = true, bool withTexture = true, const QByteArray &onlyTexture = {})
{
    Builder w;
    w.reserve(0x100);
    std::memcpy(w.b.data(), "FRES    ", 8);
    w.u32(0x08, 0x00090000);
    w.u8(0x0C, 0xFF);
    w.u8(0x0D, 0xFE);
    w.u32(0x20, quint32(w.str("sample")));

    const qsizetype rootName = w.str("Root"), armName = w.str("Arm");
    const qsizetype bones = w.reserve(0x60 * 2);
    auto bone = [&](int i, qsizetype name, int parent, float tx, float ty, float tz, float rz) {
        const qsizetype at = bones + i * 0x60;
        w.u64(at, quint64(name));
        w.s16(at + 0x2A, qint16(parent));
        w.u32(at + 0x34, 0x1001);
        w.f32(at + 0x38, 1);
        w.f32(at + 0x3C, 1);
        w.f32(at + 0x40, 1);
        w.f32(at + 0x4C, rz);
        w.f32(at + 0x50, 1);
        w.f32(at + 0x54, tx);
        w.f32(at + 0x58, ty);
        w.f32(at + 0x5C, tz);
    };
    bone(0, rootName, -1, 0, 0, 0, 0);
    bone(1, armName, 0, 0, 1, 0, float(std::numbers::pi / 2));
    const qsizetype palette = w.reserve(4);
    w.u16(palette, 0);
    w.u16(palette + 2, 1);
    const qsizetype skeleton = w.reserve(0x40);
    w.magic(skeleton, "FSKL");
    w.u32(skeleton + 4, 0x1000);
    w.u64(skeleton + 0x10, quint64(bones));
    w.u64(skeleton + 0x18, quint64(palette));
    w.u16(skeleton + 0x38, 2);
    w.u16(skeleton + 0x3C, 2);

    const qsizetype poolData = w.reserve(96, 0x100);
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float uvs[] = {0, 0, 1, 0, 0, 1};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c)
            w.f32(poolData + i * 20 + c * 4, positions[i * 3 + c]);
        w.f32(poolData + i * 20 + 12, uvs[i * 2]);
        w.f32(poolData + i * 20 + 16, uvs[i * 2 + 1]);
    }
    const qsizetype indexStart = 64;
    w.u16(poolData + indexStart, 0);
    w.u16(poolData + indexStart + 2, 1);
    w.u16(poolData + indexStart + 4, 2);
    const qsizetype pool = w.reserve(0x10);
    w.u32(pool + 4, 96);
    w.u64(pool + 8, quint64(poolData));
    w.u64(0xB0, quint64(pool));

    const qsizetype attributes = w.reserve(0x20);
    w.u64(attributes, quint64(w.str("_p0")));
    w.u32(attributes + 8, 0x1805);
    w.u64(attributes + 0x10, quint64(w.str("_u0")));
    w.u32(attributes + 0x18, 0x1705);
    w.u16(attributes + 0x1C, 12);
    const qsizetype bufferInfo = w.reserve(0x10);
    w.u32(bufferInfo, 60);
    const qsizetype bufferState = w.reserve(0x10);
    w.u32(bufferState, 20);
    const qsizetype vertices = w.reserve(0x58);
    w.magic(vertices, "FVTX");
    w.u64(vertices + 0x08, quint64(attributes));
    w.u64(vertices + 0x30, quint64(bufferInfo));
    w.u64(vertices + 0x38, quint64(bufferState));
    w.u32(vertices + 0x48, 0);
    w.u8(vertices + 0x4C, 2);
    w.u8(vertices + 0x4D, 1);
    w.u32(vertices + 0x50, 3);

    const qsizetype mesh = w.reserve(0x38);
    w.u32(mesh + 0x20, quint32(indexStart));
    w.u32(mesh + 0x24, 3);
    w.u32(mesh + 0x28, 1);
    w.u32(mesh + 0x2C, 3);
    const qsizetype shape = w.reserve(0x60);
    w.magic(shape, "FSHP");
    w.u64(shape + 0x08, quint64(w.str("Triangle")));
    w.u64(shape + 0x18, quint64(mesh));
    w.u16(shape + 0x54, 1);
    w.u8(shape + 0x5B, 1);

    const bool single = !onlyTexture.isEmpty();
    const qsizetype textureNames = w.reserve(16);
    w.u64(textureNames, quint64(w.str(single ? onlyTexture : QByteArray("tex_Nrm"))));
    w.u64(textureNames + 8, quint64(w.str("tex_Alb")));
    const qsizetype samplers = w.reserve(0x40);
    w.u8(samplers, 2);
    w.u8(samplers + 1, 1);
    const qsizetype samplerDict = single ? w.dict({"_x0"}) : w.dict({"_n0", "_a0"});
    const qsizetype material = w.reserve(0xA8);
    w.magic(material, "FMAT");
    w.u64(material + 0x08, quint64(w.str("Skin")));
    w.u64(material + 0x30, quint64(textureNames));
    w.u64(material + 0x40, quint64(samplers));
    w.u64(material + 0x48, quint64(samplerDict));
    w.u8(material + 0x9C, single ? 1 : 2);
    w.u8(material + 0x9D, single ? 1 : 2);

    const qsizetype modelName = w.str("Body");
    const qsizetype model = w.reserve(0x78);
    w.magic(model, "FMDL");
    w.u64(model + 0x08, quint64(modelName));
    w.u64(model + 0x18, quint64(skeleton));
    w.u64(model + 0x20, quint64(vertices));
    w.u64(model + 0x28, quint64(shape));
    w.u64(model + 0x38, quint64(material));
    w.u16(model + 0x68, 1);
    w.u16(model + 0x6A, 1);
    w.u16(model + 0x6C, 1);
    w.u64(0x28, quint64(model));
    w.u64(0x30, quint64(w.dict({"Body"})));

    if (withClip) {
        const qsizetype base = w.reserve(40);
        const float values[] = {1, 1, 1, 0, 0, 0, 1, 0, 1, 0};
        for (int i = 0; i < 10; ++i)
            w.f32(base + i * 4, values[i]);
        const qsizetype frames = w.reserve(4);
        w.f32(frames, 0);
        const qsizetype keys = w.reserve(8);
        w.f32(keys, 0);
        w.f32(keys + 4, float(std::numbers::pi / 2));
        const qsizetype curve = w.reserve(0x30);
        w.u64(curve, quint64(frames));
        w.u64(curve + 8, quint64(keys));
        w.u16(curve + 0x10, 0x10);
        w.u16(curve + 0x12, 1);
        w.u32(curve + 0x14, 0x28);
        w.f32(curve + 0x1C, 10);
        w.f32(curve + 0x20, 1);
        const qsizetype tracks = w.reserve(0x38 * 2);
        w.u64(tracks, quint64(armName));
        w.u64(tracks + 8, quint64(curve));
        w.u64(tracks + 0x10, quint64(base));
        w.u32(tracks + 0x28, 0x38 | (1u << 11));
        w.u8(tracks + 0x2E, 1);
        w.u64(tracks + 0x38, quint64(w.str("Tail")));
        w.u64(tracks + 0x38 + 0x10, quint64(base));
        w.u32(tracks + 0x38 + 0x28, 0x38);
        const qsizetype clip = w.reserve(0x50);
        w.magic(clip, "FSKA");
        w.u32(clip + 4, 0x1004);
        w.u64(clip + 0x08, quint64(w.str("Wave")));
        w.u64(clip + 0x28, quint64(tracks));
        w.u32(clip + 0x40, 10);
        w.u16(clip + 0x4C, 2);
        w.u64(0x58, quint64(clip));
        w.u64(0x60, quint64(w.dict({"Wave"})));
    }

    if (withTexture) {
        samplebntx::Texture t;
        t.name = "tex_Alb";
        t.linear = QByteArray(8 * 8 * 4, '\0');
        for (int i = 0; i < 64; ++i) {
            t.linear[i * 4] = char(200);
            t.linear[i * 4 + 1] = char(40);
            t.linear[i * 4 + 2] = char(10);
            t.linear[i * 4 + 3] = char(255);
        }
        w.reserve(0, 0x1000);
        w.b += samplebntx::make({t});
    }
    return w.b;
}

inline QByteArray makeV10()
{
    Builder w;
    w.reserve(0x100);
    std::memcpy(w.b.data(), "FRES    ", 8);
    w.u32(0x08, 0x000A0400);
    w.u8(0x0C, 0xFF);
    w.u8(0x0D, 0xFE);
    w.u8(0xEE, 2);
    w.u32(0x20, quint32(w.str("sample10")));

    const qsizetype rootName = w.str("Root"), armName = w.str("Arm");
    const qsizetype bones = w.reserve(0x58 * 2);
    auto bone = [&](int i, qsizetype name, int parent, float ty) {
        const qsizetype at = bones + i * 0x58;
        w.u64(at, quint64(name));
        w.s16(at + 0x22, qint16(parent));
        w.u32(at + 0x2C, 0x1001);
        w.f32(at + 0x30, 1);
        w.f32(at + 0x34, 1);
        w.f32(at + 0x38, 1);
        w.f32(at + 0x48, 1);
        w.f32(at + 0x50, ty);
    };
    bone(0, rootName, -1, 0);
    bone(1, armName, 0, 1);
    const qsizetype palette = w.reserve(4);
    w.u16(palette, 0);
    w.u16(palette + 2, 1);
    const qsizetype skeleton = w.reserve(0x40);
    w.magic(skeleton, "FSKL");
    w.u32(skeleton + 4, 0x1000);
    w.u64(skeleton + 0x10, quint64(bones));
    w.u64(skeleton + 0x18, quint64(palette));
    w.u16(skeleton + 0x38, 2);
    w.u16(skeleton + 0x3C, 2);

    const qsizetype poolData = w.reserve(0x300, 0x100);
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float uvs[] = {0.25f, 0.5f, 1, 0, 0, 1};
    for (int i = 0; i < 9; ++i)
        w.f32(poolData + i * 4, positions[i]);
    for (int i = 0; i < 6; ++i)
        w.f32(poolData + 0x100 + i * 4, uvs[i]);
    const qsizetype indexStart = 0x200;
    w.u16(poolData + indexStart, 0);
    w.u16(poolData + indexStart + 2, 1);
    w.u16(poolData + indexStart + 4, 2);
    const qsizetype pool = w.reserve(0x10);
    w.u32(pool + 4, 0x300);
    w.u64(pool + 8, quint64(poolData));
    w.u64(0xB0, quint64(pool));

    const qsizetype attributes = w.reserve(0x20);
    w.u64(attributes, quint64(w.str("_p0")));
    w.u32(attributes + 8, 0x1805);
    w.u64(attributes + 0x10, quint64(w.str("_u0")));
    w.u32(attributes + 0x18, 0x1705);
    w.u8(attributes + 0x1E, 1);
    const qsizetype bufferInfo = w.reserve(0x20);
    w.u32(bufferInfo, 36);
    w.u32(bufferInfo + 0x10, 24);
    const qsizetype bufferState = w.reserve(0x20);
    w.u32(bufferState, 12);
    w.u32(bufferState + 0x10, 8);
    const qsizetype vertices = w.reserve(0x58);
    w.magic(vertices, "FVTX");
    w.u64(vertices + 0x08, quint64(attributes));
    w.u64(vertices + 0x30, quint64(bufferInfo));
    w.u64(vertices + 0x38, quint64(bufferState));
    w.u8(vertices + 0x4C, 2);
    w.u8(vertices + 0x4D, 2);
    w.u32(vertices + 0x50, 3);
    w.u16(vertices + 0x56, 0x100);

    const qsizetype mesh = w.reserve(0x38);
    w.u32(mesh + 0x20, quint32(indexStart));
    w.u32(mesh + 0x24, 3);
    w.u32(mesh + 0x28, 1);
    w.u32(mesh + 0x2C, 3);
    const qsizetype shape = w.reserve(0x60);
    w.magic(shape, "FSHP");
    w.u64(shape + 0x08, quint64(w.str("Triangle")));
    w.u64(shape + 0x18, quint64(mesh));
    w.u16(shape + 0x54, 1);
    w.u8(shape + 0x5B, 1);

    const qsizetype textureNames = w.reserve(16);
    w.u64(textureNames, quint64(w.str("tex_Nrm")));
    w.u64(textureNames + 8, quint64(w.str("tex_Alb")));
    const qsizetype samplers = w.reserve(0x40);
    w.u8(samplers, 2);
    w.u8(samplers + 1, 1);
    const qsizetype samplerDict = w.dict({"_n0", "_a0"});
    const qsizetype renderList = w.reserve(0x10);
    w.u64(renderList, 0xF3BCBBAD5A0931BAull);
    w.u8(renderList + 8, 2);
    const qsizetype renderCounts = w.reserve(2);
    w.u16(renderCounts, 1);
    const qsizetype renderOffsets = w.reserve(2);
    w.u16(renderOffsets, 0);
    const qsizetype renderData = w.reserve(8);
    w.u64(renderData, quint64(w.str("opaque")));
    const qsizetype assign = w.reserve(0x58);
    w.u64(assign, quint64(w.str("archive")));
    w.u64(assign + 8, quint64(w.str("model")));
    w.u64(assign + 0x10, quint64(renderList));
    w.u16(assign + 0x48, 1);
    const qsizetype shaderInfo = w.reserve(0x50);
    w.u64(shaderInfo, quint64(assign));
    const qsizetype material = w.reserve(0xB0);
    w.magic(material, "FMAT");
    w.u64(material + 0x08, quint64(w.str("Skin")));
    w.u64(material + 0x10, quint64(shaderInfo));
    w.u64(material + 0x20, quint64(textureNames));
    w.u64(material + 0x30, quint64(samplers));
    w.u64(material + 0x38, quint64(samplerDict));
    w.u64(material + 0x40, quint64(renderData));
    w.u64(material + 0x48, quint64(renderCounts));
    w.u64(material + 0x50, quint64(renderOffsets));
    w.u8(material + 0xA2, 2);
    w.u8(material + 0xA3, 2);

    const qsizetype model = w.reserve(0x78);
    w.magic(model, "FMDL");
    w.u64(model + 0x08, quint64(w.str("Body")));
    w.u64(model + 0x18, quint64(skeleton));
    w.u64(model + 0x20, quint64(vertices));
    w.u64(model + 0x28, quint64(shape));
    w.u64(model + 0x38, quint64(material));
    w.u16(model + 0x68, 1);
    w.u16(model + 0x6A, 1);
    w.u16(model + 0x6C, 1);
    w.u64(0x28, quint64(model));
    w.u64(0x30, quint64(w.dict({"Body"})));

    const qsizetype base = w.reserve(40);
    const float values[] = {1, 1, 1, 0, 0, 0, 1, 0, 1, 0};
    for (int i = 0; i < 10; ++i)
        w.f32(base + i * 4, values[i]);
    const qsizetype frames = w.reserve(4);
    w.u16(frames, 0);
    w.u16(frames + 2, 1100 * 32);
    const qsizetype keys = w.reserve(8 + 2 * 4);
    w.u16(keys + 8 + 4, 65535);
    const qsizetype curve = w.reserve(0x30);
    w.u64(curve, quint64(frames));
    w.u64(curve + 8, quint64(keys));
    w.u16(curve + 0x10, 0x05);
    w.u16(curve + 0x12, 2);
    w.u32(curve + 0x14, 0x28);
    w.f32(curve + 0x1C, 1100);
    w.f32(curve + 0x20, 0.03125f);
    w.f32(curve + 0x24, float(std::numbers::pi / 2 / 65535));
    w.f32(curve + 0x28, 0);
    const qsizetype tracks = w.reserve(0x38);
    w.u64(tracks, quint64(armName));
    w.u64(tracks + 8, quint64(curve));
    w.u64(tracks + 0x10, quint64(base));
    w.u32(tracks + 0x28, 0x38 | (1u << 11));
    w.u8(tracks + 0x2E, 1);
    const qsizetype clip = w.reserve(0x50);
    w.magic(clip, "FSKA");
    w.u32(clip + 4, 0x1004);
    w.u64(clip + 0x08, quint64(w.str("Swing")));
    w.u64(clip + 0x28, quint64(tracks));
    w.u32(clip + 0x40, 1100);
    w.u16(clip + 0x4C, 1);
    w.u64(0x58, quint64(clip));
    w.u64(0x60, quint64(w.dict({"Swing"})));
    return w.b;
}

inline QByteArray makeV10Extras()
{
    Builder w;
    w.reserve(0x100);
    std::memcpy(w.b.data(), "FRES    ", 8);
    w.u32(0x08, 0x000A0400);
    w.u8(0x0C, 0xFF);
    w.u8(0x0D, 0xFE);
    w.u32(0x20, quint32(w.str("extras10")));

    const qsizetype anims = w.reserve(0x70 * 2);
    w.magic(anims, "FMAA");
    w.u64(anims + 0x08, quint64(w.str("Blink")));
    w.u32(anims + 0x58, 750);
    w.magic(anims + 0x70, "FMAA");
    w.u32(anims + 0x70 + 4, 4);
    w.u64(anims + 0x70 + 0x08, quint64(w.str("Glow")));
    w.u32(anims + 0x70 + 0x58, 30);
    w.u64(0x68, quint64(anims));
    w.u64(0x70, quint64(w.dict({"Blink", "Glow"})));
    w.u64(0x80, quint64(w.dict({"Hide"})));

    const QByteArray first("camera preset", 13), second("BNTX\0\0\0\0", 8);
    const qsizetype one = w.reserve(first.size(), 8), two = w.reserve(second.size(), 8);
    std::memcpy(w.b.data() + one, first.constData(), size_t(first.size()));
    std::memcpy(w.b.data() + two, second.constData(), size_t(second.size()));
    const qsizetype files = w.reserve(0x20);
    w.u64(files, quint64(one));
    w.u32(files + 8, quint32(first.size()));
    w.u64(files + 0x10, quint64(two));
    w.u32(files + 0x18, quint32(second.size()));
    w.u64(0xB8, quint64(files));
    w.u64(0xC0, quint64(w.dict({"Default.recb", "View01.bntx"})));
    return w.b;
}

}
