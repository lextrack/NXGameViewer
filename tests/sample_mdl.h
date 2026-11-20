#pragma once

#include <QByteArray>
#include <QtEndian>

#include <array>
#include <cstring>
#include <vector>

namespace sample {

class Writer
{
public:
    QByteArray bytes;

    void u32(quint32 v)
    {
        char raw[4];
        qToLittleEndian(v, raw);
        bytes.append(raw, 4);
    }
    void f32(float f)
    {
        quint32 bits;
        std::memcpy(&bits, &f, 4);
        u32(bits);
    }
    void str(const QByteArray &s)
    {
        bytes.append(char(s.size()));
        bytes.append(s);
    }
    // Section: type, size of what follows the size field, then the count and the records.
    void section(quint32 type, quint32 count, const QByteArray &records)
    {
        u32(type);
        u32(quint32(4 + records.size()));
        u32(count);
        bytes.append(records);
    }
};

// A hand-made version 5 MDL (no game data): one material, one mesh made of one triangle,
// and two nodes ("world" with the child "body").
inline QByteArray makeModel()
{
    Writer material;
    material.str("mat");
    material.str("chr");
    material.str("mat");
    material.u32(1);  // textures
    material.str("tex_a");
    for (int i = 0; i < 5; ++i)
        material.u32(0);
    material.u32(2);  // parameters
    material.str("colour_g");
    material.u32(6);  // 3 floats
    material.f32(1.f);
    material.f32(0.5f);
    material.f32(0.f);
    material.str("flag");
    material.u32(0);  // 1 integer
    material.u32(1);
    material.u32(0);  // switches
    material.u32(1);  // UV maps
    material.bytes.append('\0');
    material.u32(0);
    for (int i = 0; i < 3; ++i)
        material.u32(0);
    material.f32(0.f);
    material.u32(0);

    Writer meshBody;
    meshBody.u32(1);  // submeshes
    meshBody.u32(0);  // material
    meshBody.u32(3);  // index count
    meshBody.u32(0);
    meshBody.u32(0);  // bones
    Writer mesh;
    mesh.str("body");
    mesh.u32(quint32(meshBody.bytes.size()));
    mesh.bytes.append(meshBody.bytes);
    mesh.u32(44);
    mesh.bytes.append(QByteArray(44, '\0'));

    Writer nodes;
    auto node = [&](const QByteArray &name, quint32 kind, quint32 meshIndex, std::vector<quint32> children) {
        nodes.str(name);
        nodes.u32(kind);
        nodes.u32(meshIndex);
        for (int i = 0; i < 3 + 4; ++i)
            nodes.f32(0.f);  // position, 4 floats
        nodes.u32(0);
        for (int i = 0; i < 3; ++i)
            nodes.f32(0.f);  // rotation
        for (int i = 0; i < 3; ++i)
            nodes.f32(1.f);  // scale
        for (int i = 0; i < 3; ++i)
            nodes.f32(0.f);
        nodes.u32(0);  // version 5 only
        nodes.u32(quint32(children.size()));
        for (quint32 c : children)
            nodes.u32(c);
    };
    node("world", 0, 0xFFFFFFFF, {1});
    node("body", 2, 0, {});

    Writer vertices;
    for (quint32 v : {0u, 36u, 12u, 0u, 0u})  // positions: 3 × 12 bytes
        vertices.u32(v);
    for (quint32 v : {7u, 12u, 4u, 0u, 0u})   // indices: 3 × 4 bytes
        vertices.u32(v);
    for (float f : {-1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f})
        vertices.f32(f);
    for (quint32 i : {0u, 1u, 2u})
        vertices.u32(i);

    Writer file;
    file.bytes = "MDL ";
    file.u32(5);
    file.u32(0);
    file.section(0, 1, material.bytes);
    file.section(1, 1, mesh.bytes);
    file.section(2, 2, nodes.bytes);
    file.section(4, 2, vertices.bytes);
    file.u32(0);  // the 4 bytes after the last section
    return file.bytes;
}

inline QByteArray makeRichModel(bool withEnvironmentBox = false, std::array<float, 4> rootRotate = {0, 0, 0, 1})
{
    Writer materials;
    auto material = [&](const QByteArray &name, const QByteArray &texture, bool alphaTest) {
        materials.str(name);
        materials.str("chr_hair");
        materials.str(name);
        materials.u32(texture.isEmpty() ? 0 : 1);
        if (!texture.isEmpty()) {
            materials.str(texture);
            for (quint32 v : {0u, 0u, 2u, 1u, 0u})
                materials.u32(v);
        }
        materials.u32(alphaTest ? 3 : 0);
        if (alphaTest) {
            materials.str("Switch_AlphaTest");
            materials.u32(0);
            materials.u32(1);
            materials.str("alphaTestThreshold_g");
            materials.u32(4);
            materials.f32(0.3f);
            materials.str("Switch_DoubleSide");
            materials.u32(0);
            materials.u32(0);
        }
        materials.u32(0);
        materials.u32(1);
        materials.bytes.append('\0');
        materials.u32(0);
        for (int i = 0; i < 3; ++i)
            materials.u32(0);
        materials.f32(0.f);
        materials.u32(0);
    };
    material("skin", "tex_skin", false);
    material("hair", "tex_hair", true);
    if (withEnvironmentBox)
        material("black", "", false);

    Writer meshes;
    auto mesh = [&](const QByteArray &name, quint32 materialIndex, std::vector<QByteArray> bones) {
        Writer body;
        body.u32(1);
        body.u32(materialIndex);
        body.u32(3);
        body.u32(0);
        body.u32(quint32(bones.size()));
        for (const QByteArray &bone : bones) {
            body.str(bone);
            for (int i = 0; i < 16; ++i)
                body.f32(i % 5 == 0 ? 1.f : 0.f);
        }
        meshes.str(name);
        meshes.u32(quint32(body.bytes.size()));
        meshes.bytes.append(body.bytes);
        meshes.u32(44);
        meshes.bytes.append(QByteArray(44, '\0'));
    };
    mesh("body", 0, {"Root"});
    mesh("prop", 1, {});
    mesh("body_shadow", 0, {});
    if (withEnvironmentBox)
        mesh("night_box", 2, {});

    Writer nodes;
    auto node = [&](const QByteArray &name, quint32 kind, quint32 meshIndex, std::array<float, 3> position,
                    std::array<float, 3> rotation, std::vector<quint32> children) {
        nodes.str(name);
        nodes.u32(kind);
        nodes.u32(meshIndex);
        for (float f : position)
            nodes.f32(f);
        for (float f : name == "Root" ? rootRotate : std::array<float, 4>{0, 0, 0, 1})
            nodes.f32(f);
        nodes.u32(0);
        for (float f : rotation)
            nodes.f32(f);
        for (int i = 0; i < 3; ++i)
            nodes.f32(1.f);
        for (int i = 0; i < 3; ++i)
            nodes.f32(0.f);
        nodes.u32(0);
        nodes.u32(quint32(children.size()));
        for (quint32 c : children)
            nodes.u32(c);
    };
    node("world", 0, 0xFFFFFFFF, {0, 0, 0}, {0, 0, 0}, {1, 2});
    node("Root", 1, 0xFFFFFFFF, {0, 1, 0}, {1.5707964f, 0, 0}, {});
    node("prop", 2, 1, {5, 0, 0}, {0, 0, 0}, {});

    std::vector<std::array<quint32, 5>> headers;
    QByteArray data;
    auto stream = [&](quint32 kind, quint32 stride, quint32 meshIndex, const QByteArray &bytes) {
        headers.push_back({kind, quint32(bytes.size()), stride, meshIndex, 0});
        data += bytes;
    };
    auto floats = [](std::initializer_list<float> values) {
        Writer w;
        for (float f : values)
            w.f32(f);
        return w.bytes;
    };
    auto ints = [](std::initializer_list<quint32> values) {
        Writer w;
        for (quint32 v : values)
            w.u32(v);
        return w.bytes;
    };
    const quint32 meshCount = withEnvironmentBox ? 4 : 3;
    for (quint32 m = 0; m < 3; ++m) {
        stream(0, 12, m, floats({-1, 0, 0, 1, 0, 0, 0, 2, 0}));
        stream(1, 4, m, QByteArray("\x00\x7f\x00\x00\x00\x7f\x00\x00\x00\x7f\x00\x00", 12));
        stream(4, 8, m, floats({0, 0, 1, 0, 0.5f, 1}));
        stream(7, 4, m, ints({0, 1, 2}));
    }
    stream(3, 4, 1, QByteArray(12, '\0'));
    if (withEnvironmentBox) {
        stream(0, 12, 3, floats({-50, -50, -50, 50, -50, 50, 0, 50, 0}));
        stream(7, 4, 3, ints({0, 1, 2}));
    }
    stream(5, 16, 0, floats({2, 2, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}));
    stream(6, 16, 0, ints({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    Writer vertices;
    for (const auto &h : headers)
        for (quint32 v : h)
            vertices.u32(v);
    vertices.bytes += data;

    Writer file;
    file.bytes = "MDL ";
    file.u32(5);
    file.u32(0);
    file.section(0, withEnvironmentBox ? 3 : 2, materials.bytes);
    file.section(1, meshCount, meshes.bytes);
    file.section(2, 3, nodes.bytes);
    file.section(4, quint32(headers.size()), vertices.bytes);
    file.u32(0);
    return file.bytes;
}

struct SampleCurve
{
    QByteArray bone;
    quint32 type = 9;
    std::vector<std::vector<float>> keys;
};

inline QByteArray makeAnimation(const std::vector<SampleCurve> &curves, float start, float end)
{
    Writer records;
    for (const SampleCurve &curve : curves) {
        records.str(curve.bone + "_curve");
        records.str(curve.bone);
        records.u32(curve.type);
        records.u32(0);
        records.u32(0);
        records.u32(quint32(curve.keys.size()));
        const int keySize = curve.type == 10 ? 40 : curve.type == 16 ? 28 : 36;
        for (const std::vector<float> &key : curve.keys) {
            for (float f : key)
                records.f32(f);
            for (int pad = int(key.size()) * 4; pad < keySize; pad += 4)
                records.f32(0.f);
        }
    }
    records.f32(start);
    records.f32(end);

    Writer file;
    file.bytes = "MDL ";
    file.u32(5);
    file.u32(0);
    file.section(3, quint32(curves.size()), records.bytes);
    file.u32(0xFFFFFFFF);
    return file.bytes;
}

}
