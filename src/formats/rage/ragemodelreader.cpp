#include "rage/ragemodelreader.h"

#include "archives/rpf6reader.h"
#include "core/simpletable.h"
#include "core/storedreader.h"
#include "models/meshpreview.h"
#include "models/model3d.h"
#include "rage/rageresource.h"
#include "rage/ragetexturereader.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QtEndian>
#include <qfloat16.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

constexpr int kBoneSize = 224;
constexpr int kMaxBones = 4096;
constexpr int kMaxModels = 4096;
constexpr quint32 kMaxIndices = 1u << 24;
constexpr qint64 kMaxVertices = qint64{1} << 25;

using Damaged = RageDamaged;
using View = RageView;

int elementSize(int type)
{
    switch (type) {
    case 1: case 2: case 4: case 8: case 9: case 10: case 14: return 4;
    case 3: case 5: return 8;
    case 6: return 12;
    case 7: return 16;
    default: return -1;
    }
}

float half(const char *p)
{
    qfloat16 h;
    std::memcpy(&h, p, 2);
    return float(h);
}

float single(const char *p)
{
    const quint32 bits = qFromLittleEndian<quint32>(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

int decode(int type, const char *p, std::array<float, 4> &out)
{
    switch (type) {
    case 1: out = {half(p), half(p + 2), 0, 0}; return 2;
    case 2: case 4: out = {single(p), 0, 0, 0}; return 1;
    case 3: out = {half(p), half(p + 2), half(p + 4), half(p + 6)}; return 4;
    case 5: out = {single(p), single(p + 4), 0, 0}; return 2;
    case 6: out = {single(p), single(p + 4), single(p + 8), 0}; return 3;
    case 7: out = {single(p), single(p + 4), single(p + 8), single(p + 12)}; return 4;
    case 8: case 9:
        out = {float(uchar(p[0])), float(uchar(p[1])), float(uchar(p[2])), float(uchar(p[3]))};
        return 4;
    case 10: {
        const quint32 u = qFromLittleEndian<quint32>(p);
        auto component = [u](int shift) { return float(qint32(((u >> shift) & 0x3FF) << 22) >> 22) / 511.0f; };
        out = {component(0), component(10), component(20), 0};
        return 3;
    }
    case 14:
        out = {float(qFromLittleEndian<qint16>(p)) / 32767.0f, float(qFromLittleEndian<qint16>(p + 2)) / 32767.0f, 0, 0};
        return 2;
    default: return 0;
    }
}

QString cleanTextureName(QString name)
{
    if (name.startsWith(QLatin1String("memory:")))
        name = name.mid(name.lastIndexOf(u':') + 1);
    const qsizetype slash = std::max(name.lastIndexOf(u'/'), name.lastIndexOf(u'\\'));
    return slash >= 0 ? name.mid(slash + 1) : name;
}

struct Counts
{
    int drawables = 0;
    int skipped = 0;
    qint64 triangles = 0;
    int bones = 0;
};

class Builder
{
public:
    Builder(const View &v, Model3D &model, Counts &counts) : m_v(v), m_model(model), m_counts(counts) {}

    void drawable(qint64 at, const QString &name)
    {
        const int nodeBase = int(m_model.nodes.size());
        const int boneCount = skeleton(m_v.u32(at + 0x0C));
        const std::vector<int> materials = shaderGroup(m_v.u32(at + 0x08), name);
        std::optional<qint64> lod;
        for (int i = 0; i < 4 && !lod; ++i)
            lod = m_v.maybe(m_v.u32(at + 0x40 + 4 * i));
        if (!lod)
            return;
        const quint16 modelCount = m_v.u16(*lod + 4);
        if (modelCount > kMaxModels)
            throw Damaged();
        const qint64 models = modelCount ? m_v.at(m_v.u32(*lod)) : 0;
        ++m_counts.drawables;
        for (int m = 0; m < modelCount; ++m) {
            try {
                model(m_v.at(m_v.u32(models + 4 * m)), name, m, nodeBase, boneCount, materials);
            } catch (const Damaged &) {
                ++m_counts.skipped;
            }
        }
    }

private:
    int skeleton(quint32 pointer)
    {
        const std::optional<qint64> at = m_v.maybe(pointer);
        if (!at)
            return 0;
        const quint16 count = m_v.u16(*at + 0x18);
        if (count == 0 || count > kMaxBones)
            return 0;
        const qint64 bones = m_v.at(m_v.u32(*at));
        m_v.need(bones, qint64(count) * kBoneSize);
        const int base = int(m_model.nodes.size());
        for (int i = 0; i < count; ++i) {
            const qint64 b = bones + qint64(i) * kBoneSize;
            ModelNode node;
            node.name = m_v.cstring(m_v.u32(b));
            if (node.name.isEmpty())
                node.name = QStringLiteral("bone %1").arg(i);
            if (const std::optional<qint64> parent = m_v.maybe(m_v.u32(b + 0x10))) {
                const qint64 delta = *parent - bones;
                if (delta >= 0 && delta % kBoneSize == 0 && delta / kBoneSize < count && delta / kBoneSize != i)
                    node.parent = base + int(delta / kBoneSize);
            }
            node.boneId = m_v.u16(b + 0x16);
            node.translation = {m_v.f32(b + 0x20), m_v.f32(b + 0x24), m_v.f32(b + 0x28)};
            node.rotation = normalizedQuaternion({m_v.f32(b + 0x40), m_v.f32(b + 0x44), m_v.f32(b + 0x48), m_v.f32(b + 0x4C)});
            m_model.nodes.push_back(node);
        }
        for (int i = 0; i < count; ++i) {
            int p = m_model.nodes[size_t(base + i)].parent;
            for (int steps = 0; p >= 0; ++steps) {
                if (steps > count || p == base + i) {
                    m_model.nodes[size_t(base + i)].parent = -1;
                    break;
                }
                p = m_model.nodes[size_t(p)].parent;
            }
        }
        m_counts.bones += count;
        return count;
    }

    int texture(const QString &name)
    {
        const QString key = name.toLower();
        if (const auto it = m_textures.constFind(key); it != m_textures.cend())
            return *it;
        m_model.textures.push_back({name, 0, 0});
        const int index = int(m_model.textures.size() - 1);
        m_textures.insert(key, index);
        return index;
    }

    std::vector<int> shaderGroup(quint32 pointer, const QString &drawableName)
    {
        std::vector<int> materials;
        const std::optional<qint64> at = m_v.maybe(pointer);
        if (!at)
            return materials;
        static const QList<quint32> baseSlots{joaat("diffusesampler"), joaat("texturesampler"),
                                              joaat("terraindiffusesampler1")};
        static const QList<quint32> normalSlots{joaat("bumpsampler"), joaat("normalsampler"),
                                                joaat("terrainnormalsampler1")};
        const quint16 count = m_v.u16(*at + 0x0C);
        if (count > kMaxModels)
            throw Damaged();
        const qint64 shaders = count ? m_v.at(m_v.u32(*at + 0x08)) : 0;
        for (int i = 0; i < count; ++i) {
            ModelMaterial material;
            material.name = QStringLiteral("%1 shader %2").arg(drawableName).arg(i);
            try {
                const qint64 shader = m_v.at(m_v.u32(shaders + 4 * i));
                const qint64 params = m_v.at(m_v.u32(shader));
                const int paramCount = m_v.u8(shader + 8);
                qint64 size = qint64(paramCount) * 8;
                for (int k = 0; k < paramCount; ++k)
                    size += 16 * m_v.u8(params + 8 * k);
                size = (size + 15) / 16 * 16;
                int baseRank = baseSlots.size(), normalRank = normalSlots.size();
                for (int k = 0; k < paramCount; ++k) {
                    if (m_v.u8(params + 8 * k) != 0)
                        continue;
                    const quint32 hash = m_v.u32(params + size + 4 * k);
                    const std::optional<qint64> tex = m_v.maybe(m_v.u32(params + 8 * k + 4));
                    if (!tex)
                        continue;
                    const QString name = cleanTextureName(m_v.cstring(m_v.u32(*tex + 24)));
                    if (name.isEmpty())
                        continue;
                    if (const int rank = int(baseSlots.indexOf(hash)); rank >= 0 && rank < baseRank) {
                        baseRank = rank;
                        material.baseColorTexture = texture(name);
                    } else if (const int nrank = int(normalSlots.indexOf(hash)); nrank >= 0 && nrank < normalRank) {
                        normalRank = nrank;
                        material.normalTexture = texture(name);
                    }
                }
            } catch (const Damaged &) {
            }
            m_model.materials.push_back(material);
            materials.push_back(int(m_model.materials.size() - 1));
        }
        return materials;
    }

    void model(qint64 at, const QString &name, int index, int nodeBase, int boneCount, const std::vector<int> &materials)
    {
        const quint16 geometryCount = m_v.u16(at + 8);
        if (geometryCount > kMaxModels)
            throw Damaged();
        const qint64 geometries = geometryCount ? m_v.at(m_v.u32(at + 4)) : 0;
        const std::optional<qint64> mapping = m_v.maybe(m_v.u32(at + 0x10));
        const int matrixIndex = m_v.u8(at + 0x17);
        const bool skinned = m_v.u8(at + 0x19) != 0 && boneCount > 0;
        for (int g = 0; g < geometryCount; ++g) {
            try {
                ModelMesh mesh;
                mesh.name = geometryCount > 1 ? QStringLiteral("%1 model %2.%3").arg(name).arg(index).arg(g)
                                              : QStringLiteral("%1 model %2").arg(name).arg(index);
                ModelPrimitive primitive;
                const qint64 geometry = m_v.at(m_v.u32(geometries + 4 * g));
                if (!this->geometry(geometry, skinned, primitive, mesh.joints, nodeBase, boneCount))
                    continue;
                if (!skinned && boneCount > 0 && matrixIndex < boneCount)
                    mesh.node = nodeBase + matrixIndex;
                const int shader = mapping ? m_v.u16(*mapping + 2 * g) : 0;
                primitive.material = shader < int(materials.size()) ? materials[size_t(shader)] : -1;
                primitive.mesh = int(m_model.meshes.size());
                m_counts.triangles += qint64(primitive.indices.size() / 3);
                m_model.meshes.push_back(std::move(mesh));
                m_model.primitives.push_back(std::move(primitive));
            } catch (const Damaged &) {
                ++m_counts.skipped;
            }
        }
    }

    bool geometry(qint64 at, bool skinned, ModelPrimitive &p, std::vector<int> &joints, int nodeBase, int boneCount)
    {
        const qint64 buffer = m_v.at(m_v.u32(at + 0x0C));
        const qint64 indexBuffer = m_v.at(m_v.u32(at + 0x1C));
        const int primitiveType = m_v.u8(at + 0x36);
        if (primitiveType != 3 && primitiveType != 4)
            throw Damaged();
        const qint64 declaration = m_v.at(m_v.u32(buffer + 0x18));
        const quint32 mask = m_v.u32(declaration);
        const int stride = m_v.u8(declaration + 4);
        const quint64 types = qFromLittleEndian<quint64>(m_v.bytes(declaration + 8, 8));
        int count = m_v.u16(at + 0x34);
        if (count == 0)
            count = m_v.u16(buffer + 4);
        if (count == 0 || stride == 0)
            return false;
        std::array<int, 16> offsets{}, kinds{};
        auto lay = [&](bool alignWide) {
            int offset = 0;
            for (int i = 0; i < 16; ++i) {
                kinds[size_t(i)] = -1;
                if (!((mask >> i) & 1))
                    continue;
                const int type = int((types >> (4 * i)) & 0xF);
                const int size = elementSize(type);
                if (size < 0)
                    throw Damaged();
                if (alignWide && size == 16)
                    offset = (offset + 15) / 16 * 16;
                offsets[size_t(i)] = offset;
                kinds[size_t(i)] = type;
                offset += size;
            }
            return alignWide ? (offset + 15) / 16 * 16 : offset;
        };
        if ((lay(false) != stride && lay(true) != stride) || kinds[0] < 0)
            throw Damaged();
        if (m_totalVertices + count > kMaxVertices)
            throw Damaged();
        std::optional<qint64> data = m_v.maybe(m_v.u32(buffer + 0x10));
        if (!data)
            data = m_v.at(m_v.u32(buffer + 0x08));
        m_v.need(*data, qint64(count) * stride);

        const bool weighted = skinned && kinds[1] >= 0 && kinds[2] >= 0;
        if (weighted) {
            const int ids = m_v.u16(at + 0x3E);
            if (ids > 0) {
                const qint64 list = m_v.at(m_v.u32(at + 0x38));
                for (int i = 0; i < ids; ++i) {
                    const int bone = m_v.u16(list + 2 * i);
                    joints.push_back(nodeBase + (bone < boneCount ? bone : 0));
                }
            } else {
                for (int i = 0; i < boneCount; ++i)
                    joints.push_back(nodeBase + i);
            }
        }
        const int palette = int(joints.size());
        std::array<float, 4> e{};
        for (int v = 0; v < count; ++v) {
            const char *vertex = m_v.bytes(*data + qint64(v) * stride, stride);
            const int n = decode(kinds[0], vertex + offsets[0], e);
            p.positions.insert(p.positions.end(), {e[0], n > 1 ? e[1] : 0.0f, n > 2 ? e[2] : 0.0f});
            if (kinds[3] >= 0 && decode(kinds[3], vertex + offsets[3], e) >= 3)
                p.normals.insert(p.normals.end(), {e[0], e[1], e[2]});
            if (kinds[6] >= 0 && decode(kinds[6], vertex + offsets[6], e) >= 2)
                p.uv0.insert(p.uv0.end(), {e[0], 1.0f - e[1]});
            if (kinds[7] >= 0 && decode(kinds[7], vertex + offsets[7], e) >= 2)
                p.uv1.insert(p.uv1.end(), {e[0], 1.0f - e[1]});
            if (kinds[4] == 9) {
                const char *c = vertex + offsets[4];
                p.colors.insert(p.colors.end(), {quint8(c[2]), quint8(c[1]), quint8(c[0]), quint8(c[3])});
            }
            if (weighted) {
                std::array<float, 4> w{}, j{};
                decode(kinds[1], vertex + offsets[1], w);
                decode(kinds[2], vertex + offsets[2], j);
                const bool bytes = kinds[1] == 8 || kinds[1] == 9;
                for (int k = 0; k < 4; ++k) {
                    const int joint = int(j[size_t(k)]);
                    const bool valid = joint >= 0 && joint < palette;
                    p.joints.push_back(quint16(valid ? joint : 0));
                    p.weights.push_back(valid ? (bytes ? w[size_t(k)] / 255.0f : w[size_t(k)]) : 0.0f);
                }
            }
        }

        const quint32 indexCount = std::min(m_v.u32(at + 0x2C), m_v.u32(indexBuffer + 4));
        if (indexCount == 0 || indexCount > kMaxIndices)
            throw Damaged();
        const qint64 indexData = m_v.at(m_v.u32(indexBuffer + 8));
        m_v.need(indexData, qint64(indexCount) * 2);
        auto index = [&](quint32 i) { return quint32(m_v.u16(indexData + 2 * qint64(i))); };
        if (primitiveType == 3) {
            for (quint32 i = 0; i + 2 < indexCount; i += 3) {
                const quint32 a = index(i), b = index(i + 1), c = index(i + 2);
                if (a < quint32(count) && b < quint32(count) && c < quint32(count))
                    p.indices.insert(p.indices.end(), {a, b, c});
            }
        } else {
            quint32 start = 0;
            for (quint32 i = 0; i + 2 < indexCount; ++i) {
                const quint32 a = index(i), b = index(i + 1), c = index(i + 2);
                if (a == 0xFFFF || b == 0xFFFF || c == 0xFFFF) {
                    start = i + 1;
                    continue;
                }
                if (a == b || b == c || a == c || a >= quint32(count) || b >= quint32(count) || c >= quint32(count))
                    continue;
                if ((i - start) % 2 == 0)
                    p.indices.insert(p.indices.end(), {a, b, c});
                else
                    p.indices.insert(p.indices.end(), {b, a, c});
            }
        }
        if (p.indices.empty())
            return false;
        if (p.normals.size() != p.positions.size())
            p.normals.clear();
        if (p.uv0.size() * 3 != p.positions.size() * 2)
            p.uv0.clear();
        if (p.uv1.size() * 3 != p.positions.size() * 2)
            p.uv1.clear();
        m_totalVertices += count;
        return true;
    }

    const View &m_v;
    Model3D &m_model;
    Counts &m_counts;
    QHash<QString, int> m_textures;
    qint64 m_totalVertices = 0;
};

struct Drawable
{
    qint64 at = 0;
    QString name;
};

std::vector<Drawable> drawables(const View &v, quint32 type, const QString &fileName)
{
    const qint64 root = v.objectStart();
    const QString base = QFileInfo(fileName).completeBaseName();
    std::vector<Drawable> out;
    if (type == RageModelReader::kFragmentType) {
        QString name = QFileInfo(v.cstring(v.u32(root + 0xB0))).fileName();
        out.push_back({v.at(v.u32(root + 0xB4)), name.isEmpty() ? base : name});
    } else if (type == RageModelReader::kFragmentDrawableType) {
        out.push_back({v.at(v.u32(root + 0x08)), base});
    } else if (type == RageModelReader::kVisualDictionaryType) {
        const quint16 count = v.u16(root + 0x24);
        const quint16 hashCount = v.u16(root + 0x1C);
        if (count == 0)
            throw Damaged();
        const qint64 list = v.at(v.u32(root + 0x20));
        const std::optional<qint64> hashes = v.maybe(v.u32(root + 0x18));
        for (int i = 0; i < count; ++i) {
            const QString name = hashes && i < hashCount ? QStringLiteral("%1").arg(v.u32(*hashes + 4 * i), 8, 16, QLatin1Char('0'))
                                                         : QStringLiteral("%1 %2").arg(base).arg(i);
            if (const std::optional<qint64> d = v.maybe(v.u32(list + 4 * i)))
                out.push_back({*d, name});
        }
    }
    return out;
}

class ModelTable : public SimpleTable
{
public:
    ModelTable(QString name, std::vector<Row> rows, std::shared_ptr<const Model3D> model)
        : SimpleTable(std::move(name), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_model(std::move(model))
    {
        setImageMaker([m = m_model] { return renderPreview(previewMeshes(*m)); });
    }
    bool hasModel() const override { return true; }
    std::shared_ptr<const Model3D> model() const override { return m_model; }

private:
    std::shared_ptr<const Model3D> m_model;
};

std::unique_ptr<Document> parse(const QByteArray &data, const QString &fileName, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    std::optional<RageResource> r = readRageResource(data, error);
    if (!r)
        return nullptr;
    if (r->type != RageModelReader::kFragmentType && r->type != RageModelReader::kFragmentDrawableType
        && r->type != RageModelReader::kVisualDictionaryType)
        return fail(QObject::tr("This RAGE resource (type %1) is not a model.").arg(r->type));
    const View v(*r);
    auto model = std::make_shared<Model3D>();
    model->staticMeshesUseNodeTransform = true;
    Counts counts;
    try {
        Builder builder(v, *model, counts);
        for (const Drawable &d : drawables(v, r->type, fileName)) {
            try {
                builder.drawable(d.at, d.name);
            } catch (const Damaged &) {
                ++counts.skipped;
            }
        }
    } catch (const Damaged &) {
        return fail(QObject::tr("This RAGE resource is not a model the app can read."));
    }
    if (model->primitives.empty())
        return fail(QObject::tr("This RAGE resource has no meshes the app can read."));
    if (QFileInfo(fileName).isAbsolute())
        model->sourcePath = fileName;

    QString textureError;
    std::unique_ptr<Document> textures = RageTextureDictionaryReader().readBytes(data, fileName, &textureError);
    QHash<QString, std::shared_ptr<const Table>> byName;
    if (textures)
        for (size_t i = 1; i < textures->tables.size(); ++i)
            byName.insert(textures->tables[i]->name().toLower(), textures->tables[i]);
    int embedded = 0;
    for (const ModelTexture &t : model->textures)
        embedded += byName.contains(t.name.toLower()) ? 1 : 0;
    if (!byName.isEmpty())
        model->embeddedTexture = [byName](const QString &name) -> std::shared_ptr<const Image> {
            const auto it = byName.constFind(name.toLower());
            return it != byName.cend() && (*it)->hasImage() ? (*it)->image() : nullptr;
        };

    qint64 vertices = 0;
    for (const ModelPrimitive &p : model->primitives)
        vertices += p.vertexCount();
    std::vector<SimpleTable::Row> rows{
        {QObject::tr("drawables"), counts.drawables},
        {QObject::tr("meshes"), qint64(model->meshes.size())},
        {QObject::tr("vertices"), vertices},
        {QObject::tr("triangles"), counts.triangles},
        {QObject::tr("bones"), counts.bones},
        {QObject::tr("materials"), qint64(model->materials.size())},
        {QObject::tr("textures"), qint64(model->textures.size())},
        {QObject::tr("textures inside this file"), embedded},
    };
    if (counts.skipped)
        rows.push_back({QObject::tr("parts that could not be read"), counts.skipped});
    QString description = QObject::tr("RAGE model, %n mesh(es)", nullptr, int(model->meshes.size()))
                          + QObject::tr(", %n triangle(s)", nullptr, int(std::min<qint64>(counts.triangles, INT_MAX)));
    if (counts.bones)
        description += QObject::tr(", %n bone(s)", nullptr, counts.bones);
    if (model->textures.size() > size_t(embedded))
        description += QObject::tr("; %n texture(s) are in other files", nullptr, int(model->textures.size()) - embedded);
    const QString base = QFileInfo(fileName).completeBaseName();
    auto table = std::make_shared<ModelTable>(base.isEmpty() ? QObject::tr("Model") : base, std::move(rows), model);
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    if (textures)
        for (auto &t : textures->tables)
            doc->tables.push_back(std::move(t));
    return doc;
}

}

int RageModelReader::score(const QString &, QByteArrayView head) const
{
    if (!isRageResource(head))
        return 0;
    const quint32 type = qFromLittleEndian<quint32>(head.data() + 4);
    if (type == kFragmentType || type == kVisualDictionaryType)
        return 60;
    if (type != kFragmentDrawableType)
        return 0;
    const qsizetype bodyAt = uchar(head[3]) == 0x85 ? 16 : 12;
    if (head.size() < bodyAt + 16)
        return 0;
    const quint32 drawable = qFromLittleEndian<quint32>(head.data() + bodyAt + 8);
    const quint32 textures = qFromLittleEndian<quint32>(head.data() + bodyAt + 12);
    return (drawable >> 28) == 5 && (textures == 0 || (textures >> 28) == 5) ? 40 : 0;
}

std::unique_ptr<Document> RageModelReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return parse(*bytes, path, error);
}

std::unique_ptr<Document> RageModelReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return parse(data, fileName, error);
}
