#include "models/falcommdlreader.h"
#include "core/storedreader.h"
#include "models/meshpreview.h"
#include "models/model3d.h"
#include "core/simpletable.h"

#include <QObject>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <array>
#include <limits>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <optional>

namespace {

constexpr quint32 kSectionMaterials = 0, kSectionMeshes = 1, kSectionNodes = 2, kSectionAnimation = 3,
                  kSectionVertexData = 4;
constexpr quint32 kStreamPositions = 0, kStreamNormals = 1, kStreamColours = 3, kStreamUvs = 4, kStreamWeights = 5,
                  kStreamJoints = 6, kStreamIndices = 7;
constexpr quint32 kMaxRecords = 1'000'000;

class Cursor
{
public:
    Cursor(const QByteArray &data, qsizetype pos, qsizetype end) : m_data(data), m_pos(pos), m_end(end) {}

    bool ok() const { return m_ok; }
    qsizetype pos() const { return m_pos; }
    void seek(qsizetype pos) { (pos < 0 || pos > m_end) ? void(m_ok = false) : void(m_pos = pos); }
    void skip(qint64 bytes) { (bytes < 0 || bytes > m_end - m_pos) ? void(m_ok = false) : void(m_pos += bytes); }

    quint32 u32()
    {
        if (!take(4))
            return 0;
        return qFromLittleEndian<quint32>(m_data.constData() + m_pos - 4);
    }
    float f32()
    {
        const quint32 bits = u32();
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    QString string()
    {
        if (!take(1))
            return {};
        const int length = uchar(m_data[m_pos - 1]);
        if (!take(length))
            return {};
        return QString::fromUtf8(m_data.constData() + m_pos - length, length);
    }
    quint32 count(int minSize)
    {
        const quint32 n = u32();
        if (n > kMaxRecords || qint64(n) * minSize > m_end - m_pos)
            m_ok = false;
        return m_ok ? n : 0;
    }

private:
    bool take(qint64 bytes)
    {
        if (!m_ok || bytes > m_end - m_pos) {
            m_ok = false;
            return false;
        }
        m_pos += bytes;
        return true;
    }

    const QByteArray &m_data;
    qsizetype m_pos, m_end;
    bool m_ok = true;
};

struct Section
{
    quint32 type = 0, count = 0;
    qsizetype start = 0, end = 0;
};

struct TextureRef
{
    QString name;
    quint32 slot = 0, wrapU = 0, wrapV = 0;
};

struct Material
{
    QString name, shader, materialName;
    std::vector<TextureRef> textures;
    std::vector<std::pair<QString, QString>> parameters;
    std::map<QString, double> numbers;
    int switches = 0;
};

struct Submesh
{
    quint32 material = 0, indexCount = 0;
};

struct Mesh
{
    QString name;
    std::vector<Submesh> submeshes;
    QStringList bones;
};

struct Node
{
    QString name;
    quint32 kind = 0, mesh = 0;
    float position[3] = {}, quaternion[4] = {0, 0, 0, 1}, rotation[3] = {}, scale[3] = {};
    std::vector<quint32> children;
};

struct Stream
{
    quint32 kind = 0, bytes = 0, stride = 0, mesh = 0, submesh = 0;
    qsizetype offset = 0;

    std::pair<quint32, quint32> key() const { return {mesh, submesh}; }
};

QString number(float f) { return QString::number(double(f), 'g', 6); }
QString vec3(const float v[3]) { return QStringLiteral("%1, %2, %3").arg(number(v[0]), number(v[1]), number(v[2])); }

QString parameterValue(quint32 type, const std::vector<quint32> &values)
{
    QStringList parts;
    for (quint32 v : values) {
        if (type == 0 || type == 1) {
            parts << QString::number(v);
        } else {
            float f;
            std::memcpy(&f, &v, 4);
            parts << number(f);
        }
    }
    return parts.join(QStringLiteral(", "));
}

int parameterValueCount(quint32 type)
{
    switch (type) {
    case 0: case 1: case 4: return 1;
    case 2: case 5: return 2;
    case 3: case 6: return 3;
    case 7: return 4;
    case 8: return 16;
    default: return -1;
    }
}

bool readMaterials(Cursor &c, quint32 count, std::vector<Material> &out)
{
    for (quint32 i = 0; i < count && c.ok(); ++i) {
        Material m;
        m.name = c.string();
        m.shader = c.string();
        m.materialName = c.string();
        for (quint32 t = 0, n = c.count(21); t < n && c.ok(); ++t) {
            TextureRef ref;
            ref.name = c.string();
            ref.slot = c.u32();
            c.u32();
            ref.wrapU = c.u32();
            ref.wrapV = c.u32();
            c.u32();
            m.textures.push_back(ref);
        }
        for (quint32 p = 0, n = c.count(5); p < n && c.ok(); ++p) {
            const QString name = c.string();
            const quint32 type = c.u32();
            const int valueCount = parameterValueCount(type);
            if (valueCount < 0)
                return false;
            std::vector<quint32> values;
            for (int v = 0; v < valueCount; ++v)
                values.push_back(c.u32());
            m.parameters.emplace_back(name, parameterValue(type, values));
            if (valueCount == 1) {
                float f;
                std::memcpy(&f, &values[0], 4);
                m.numbers.emplace(name, type == 0 || type == 1 ? double(values[0]) : double(f));
            }
        }
        m.switches = int(c.count(5));
        for (int s = 0; s < m.switches && c.ok(); ++s) {
            c.string();
            c.u32();
        }
        c.skip(c.count(1));
        c.skip(c.count(1));
        c.skip(3 * 4 + 4 + 4);
        out.push_back(std::move(m));
    }
    return c.ok();
}

bool readMeshes(Cursor &c, quint32 count, std::vector<Mesh> &out)
{
    for (quint32 i = 0; i < count; ++i) {
        Mesh mesh;
        mesh.name = c.string();
        const quint32 size = c.u32();
        if (!c.ok())
            return false;
        const qsizetype next = c.pos() + size;
        for (quint32 s = 0, n = c.count(12); s < n && c.ok(); ++s) {
            Submesh sub;
            sub.material = c.u32();
            sub.indexCount = c.u32();
            c.u32();
            mesh.submeshes.push_back(sub);
        }
        for (quint32 b = 0, n = c.count(65); b < n && c.ok(); ++b) {
            mesh.bones << c.string();
            c.skip(64);
        }
        if (!c.ok())
            mesh.bones.clear();
        out.push_back(std::move(mesh));
        c.seek(next);
        c.skip(c.u32());
        if (!c.ok())
            return false;
    }
    return true;
}

bool readNodes(Cursor &c, quint32 count, quint32 version, std::vector<Node> &out)
{
    for (quint32 i = 0; i < count && c.ok(); ++i) {
        Node n;
        n.name = c.string();
        n.kind = c.u32();
        n.mesh = c.u32();
        for (float &v : n.position)
            v = c.f32();
        for (float &v : n.quaternion)
            v = c.f32();
        c.skip(4);
        for (float &v : n.rotation)
            v = c.f32();
        for (float &v : n.scale)
            v = c.f32();
        c.skip(3 * 4 + (version >= 5 ? 4 : 0));
        for (quint32 k = 0, children = c.count(4); k < children && c.ok(); ++k)
            n.children.push_back(c.u32());
        out.push_back(std::move(n));
    }
    return c.ok();
}

bool readStreams(Cursor &c, quint32 count, qsizetype sectionEnd, std::vector<Stream> &out)
{
    for (quint32 i = 0; i < count && c.ok(); ++i) {
        Stream s;
        s.kind = c.u32();
        s.bytes = c.u32();
        s.stride = c.u32();
        s.mesh = c.u32();
        s.submesh = c.u32();
        out.push_back(s);
    }
    qsizetype offset = c.pos();
    for (Stream &s : out) {
        if (s.bytes > sectionEnd - offset)
            return false;
        s.offset = offset;
        offset += s.bytes;
    }
    return c.ok();
}

QString kindName(quint32 kind)
{
    switch (kind) {
    case 0: return QObject::tr("locator");
    case 1: return QObject::tr("bone");
    case 2: return QObject::tr("mesh");
    default: return QString::number(kind);
    }
}

QString streamName(quint32 kind)
{
    static const char *const names[] = {"positions", "normals", "tangents", "colours", "UVs", "weights", "bones", "indices"};
    return kind < 8 ? QString::fromLatin1(names[kind]) : QObject::tr("kind %1").arg(kind);
}

bool isAuxiliaryMesh(const QString &name)
{
    return name.contains(QLatin1String("shadow"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("collision"), Qt::CaseInsensitive);
}

int normalSlot(const QString &shader)
{
    if (shader == QLatin1String("fur"))
        return 2;
    static const QStringList withNormals = {"chr_cloth", "chr_hair", "chr_skin", "map", "monster"};
    return withNormals.contains(shader) ? 3 : -1;
}

template <typename T>
std::vector<T> readArray(const QByteArray &data, const Stream &s)
{
    std::vector<T> out(s.bytes / sizeof(T));
    if (!out.empty())
        std::memcpy(out.data(), data.constData() + s.offset, out.size() * sizeof(T));
    return out;
}

struct Curve
{
    QString bone;
    quint32 type = 0;
    std::vector<float> times;
    std::vector<float> values;
};

constexpr quint32 kCurveTranslation = 9, kCurveRotation = 10, kCurveScale = 11;

int curveKeySize(quint32 type)
{
    switch (type) {
    case kCurveTranslation: return 36;
    case kCurveRotation: return 40;
    case kCurveScale: return 36;
    case 12: return 28;
    case 13: return 32;
    case 14: return 36;
    case 16: return 28;
    default: return 0;
    }
}

bool readCurves(Cursor &c, quint32 count, std::vector<Curve> &out, std::optional<float> *start)
{
    for (quint32 i = 0; i < count && c.ok(); ++i) {
        Curve curve;
        c.string();
        curve.bone = c.string();
        curve.type = c.u32();
        c.skip(8);
        const int keySize = curveKeySize(curve.type);
        if (keySize == 0)
            return false;
        const quint32 keys = c.count(keySize);
        const int components = curve.type == kCurveRotation ? 4 : 3;
        const bool kept = curve.type == kCurveTranslation || curve.type == kCurveRotation || curve.type == kCurveScale;
        for (quint32 k = 0; k < keys && c.ok(); ++k) {
            if (!kept) {
                c.skip(keySize);
                continue;
            }
            curve.times.push_back(c.f32());
            for (int v = 0; v < components; ++v)
                curve.values.push_back(c.f32());
            c.skip(keySize - 4 - 4 * components);
        }
        if (kept && c.ok() && !curve.times.empty())
            out.push_back(std::move(curve));
    }
    if (c.ok() && start) {
        const qsizetype here = c.pos();
        const float first = c.f32();
        if (c.ok() && std::isfinite(first))
            *start = first;
        else
            c.seek(here);
    }
    return c.ok();
}

std::vector<Section> readSections(const QByteArray &data, QString *error)
{
    std::vector<Section> sections;
    Cursor top(data, 12, data.size());
    while (top.pos() + 12 <= data.size()) {
        Section s;
        s.type = top.u32();
        if (s.type == 0xFFFFFFFF)
            break;
        const quint32 size = top.u32();
        s.end = top.pos() + size;
        if (size < 4 || s.end > data.size()) {
            if (error)
                *error = QObject::tr("Section at 0x%1 goes past the end of the file.").arg(top.pos() - 8, 0, 16);
            return {};
        }
        s.count = top.u32();
        s.start = top.pos();
        sections.push_back(s);
        top.seek(s.end);
    }
    return sections;
}

struct Parsed
{
    QByteArray data;
    QString path;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Stream> streams;
};

struct Bounds
{
    std::array<float, 3> lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()};
    std::array<float, 3> hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest()};

    bool valid() const { return lo[0] <= hi[0]; }
    void add(const std::array<float, 3> &p)
    {
        for (int k = 0; k < 3; ++k) {
            lo[size_t(k)] = std::min(lo[size_t(k)], p[size_t(k)]);
            hi[size_t(k)] = std::max(hi[size_t(k)], p[size_t(k)]);
        }
    }
    void add(const Bounds &b)
    {
        if (b.valid()) {
            add(b.lo);
            add(b.hi);
        }
    }
    bool encloses(const Bounds &inner) const
    {
        for (int k = 0; k < 3; ++k) {
            const float margin = (inner.hi[size_t(k)] - inner.lo[size_t(k)]) * 0.02f;
            if (lo[size_t(k)] > inner.lo[size_t(k)] + margin || hi[size_t(k)] < inner.hi[size_t(k)] - margin)
                return false;
        }
        return true;
    }
};

void markEnvironmentBoxes(Model3D &model)
{
    std::vector<Bounds> bounds(model.primitives.size());
    Bounds textured;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < model.primitives.size(); ++i) {
        const ModelPrimitive &prim = model.primitives[i];
        const Matrix4 matrix = model.meshMatrix(prim.mesh);
        for (qsizetype v = 0; v < prim.vertexCount(); ++v)
            bounds[i].add(transformPoint(matrix, &prim.positions[size_t(v) * 3]));
        if (model.isAuxiliary(prim))
            continue;
        const bool hasTexture = prim.material >= 0 && model.materials[size_t(prim.material)].baseColorTexture >= 0;
        if (hasTexture)
            textured.add(bounds[i]);
        else
            candidates.push_back(i);
    }
    if (!textured.valid())
        return;
    for (size_t i : candidates)
        if (bounds[i].valid() && bounds[i].encloses(textured))
            model.primitives[i].environment = true;
}

std::shared_ptr<Model3D> buildModel(const Parsed &p)
{
    auto model = std::make_shared<Model3D>();
    model->sourcePath = p.path;
    model->staticMeshesUseNodeTransform = true;

    std::map<QString, int> nodeByName;
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const Node &n = p.nodes[i];
        ModelNode node;
        node.name = n.name;
        node.translation = {n.position[0], n.position[1], n.position[2]};
        node.orientation = quaternionFromEulerZYX(n.rotation[0], n.rotation[1], n.rotation[2]);
        node.rotation = normalizedQuaternion(multiplyQuaternions(
            node.orientation, normalizedQuaternion({n.quaternion[0], n.quaternion[1], n.quaternion[2], n.quaternion[3]})));
        node.scale = {n.scale[0], n.scale[1], n.scale[2]};
        model->nodes.push_back(node);
        nodeByName.emplace(n.name, int(i));
    }
    for (size_t i = 0; i < p.nodes.size(); ++i)
        for (quint32 child : p.nodes[i].children)
            if (child < model->nodes.size() && child != i && model->nodes[child].parent < 0)
                model->nodes[child].parent = int(i);
    for (ModelNode &n : model->nodes) {
        int steps = 0;
        for (int a = n.parent; a >= 0 && steps <= int(model->nodes.size()); a = model->nodes[size_t(a)].parent)
            ++steps;
        if (steps > int(model->nodes.size()))
            n.parent = -1;
    }

    std::map<QString, int> textureByName;
    for (const Material &m : p.materials) {
        ModelMaterial material;
        material.name = m.name;
        material.auxiliary = isAuxiliaryMesh(m.name);
        const auto number = [&m](const char *key) -> std::optional<double> {
            const auto it = m.numbers.find(QString::fromLatin1(key));
            return it != m.numbers.end() ? std::optional<double>(it->second) : std::nullopt;
        };
        if (const auto alphaTest = number("Switch_AlphaTest"); alphaTest && *alphaTest != 0) {
            material.alphaMode = AlphaMode::Mask;
            material.alphaCutoff = float(number("alphaTestThreshold_g").value_or(0.5));
        } else if (!alphaTest && m.shader.contains(QLatin1String("eye"), Qt::CaseInsensitive)) {
            material.alphaMode = AlphaMode::Blend;
        }
        if (const auto doubleSide = number("Switch_DoubleSide"))
            material.doubleSided = *doubleSide != 0;
        const int normal = normalSlot(m.shader);
        for (const TextureRef &ref : m.textures) {
            auto [it, added] = textureByName.emplace(ref.name, int(model->textures.size()));
            if (added)
                model->textures.push_back({ref.name, int(ref.wrapU), int(ref.wrapV)});
            if (ref.slot == 0 && material.baseColorTexture < 0)
                material.baseColorTexture = it->second;
            if (normal >= 0 && int(ref.slot) == normal && material.normalTexture < 0)
                material.normalTexture = it->second;
        }
        model->materials.push_back(material);
    }

    quint32 meshCount = quint32(p.meshes.size());
    for (const Stream &s : p.streams)
        meshCount = std::max(meshCount, s.mesh + 1);
    meshCount = std::min<quint32>(meshCount, 100000);
    for (quint32 i = 0; i < meshCount; ++i) {
        ModelMesh mesh;
        if (i < p.meshes.size()) {
            mesh.name = p.meshes[i].name;
            for (const QString &bone : p.meshes[i].bones) {
                const auto it = nodeByName.find(bone);
                mesh.joints.push_back(it != nodeByName.end() ? it->second : 0);
            }
        }
        for (size_t n = 0; n < p.nodes.size(); ++n)
            if (p.nodes[n].kind == 2 && p.nodes[n].mesh == i) {
                mesh.node = int(n);
                break;
            }
        if (mesh.name.isEmpty())
            mesh.name = mesh.node >= 0 ? p.nodes[size_t(mesh.node)].name : QStringLiteral("mesh%1").arg(i);
        mesh.auxiliary = isAuxiliaryMesh(mesh.name);
        model->meshes.push_back(mesh);
    }

    std::map<std::pair<quint32, quint32>, size_t> primitiveByKey;
    for (const Stream &s : p.streams) {
        if (s.mesh >= meshCount)
            continue;
        auto [it, added] = primitiveByKey.emplace(s.key(), model->primitives.size());
        if (added) {
            ModelPrimitive prim;
            prim.mesh = int(s.mesh);
            if (s.mesh < p.meshes.size() && s.submesh < p.meshes[s.mesh].submeshes.size()) {
                const quint32 material = p.meshes[s.mesh].submeshes[s.submesh].material;
                prim.material = material < model->materials.size() ? int(material) : -1;
            }
            model->primitives.push_back(std::move(prim));
        }
        ModelPrimitive &prim = model->primitives[it->second];
        const char *bytes = p.data.constData() + s.offset;
        switch (s.kind) {
        case kStreamPositions:
            if (s.stride == 12 && prim.positions.empty())
                prim.positions = readArray<float>(p.data, s);
            break;
        case kStreamNormals:
            if (!prim.normals.empty())
                break;
            if (s.stride == 12) {
                prim.normals = readArray<float>(p.data, s);
            } else if (s.stride == 4) {
                prim.normals.resize(size_t(s.bytes / 4) * 3);
                for (size_t v = 0; v < s.bytes / 4; ++v)
                    for (int k = 0; k < 3; ++k)
                        prim.normals[v * 3 + size_t(k)] = std::max(-1.f, float(qint8(bytes[v * 4 + size_t(k)])) / 127.f);
            }
            break;
        case kStreamColours:
            if (!prim.colors.empty())
                break;
            prim.colorsAreData = true;
            if (s.stride == 4) {
                prim.colors.assign(reinterpret_cast<const quint8 *>(bytes), reinterpret_cast<const quint8 *>(bytes) + s.bytes / 4 * 4);
                for (size_t v = 0; v + 3 < prim.colors.size(); v += 4)
                    std::swap(prim.colors[v], prim.colors[v + 2]);
            } else if (s.stride == 16) {
                const std::vector<float> f = readArray<float>(p.data, s);
                prim.colors.resize(f.size());
                for (size_t k = 0; k < f.size(); ++k)
                    prim.colors[k] = quint8(std::clamp(f[k], 0.f, 1.f) * 255.f + 0.5f);
            }
            break;
        case kStreamUvs:
            if (s.stride == 8) {
                if (prim.uv0.empty())
                    prim.uv0 = readArray<float>(p.data, s);
                else if (prim.uv1.empty())
                    prim.uv1 = readArray<float>(p.data, s);
            }
            break;
        case kStreamWeights:
            if (s.stride == 16 && prim.weights.empty())
                prim.weights = readArray<float>(p.data, s);
            break;
        case kStreamJoints:
            if (s.stride == 16 && prim.joints.empty()) {
                const std::vector<quint32> j = readArray<quint32>(p.data, s);
                prim.joints.resize(j.size());
                for (size_t k = 0; k < j.size(); ++k)
                    prim.joints[k] = quint16(std::min<quint32>(j[k], 0xFFFF));
            }
            break;
        case kStreamIndices:
            if (s.stride == 4 && prim.indices.empty())
                prim.indices = readArray<quint32>(p.data, s);
            break;
        default:
            break;
        }
    }

    for (ModelPrimitive &prim : model->primitives) {
        const size_t vertices = prim.positions.size() / 3;
        prim.positions.resize(vertices * 3);
        auto fit = [vertices](auto &v, size_t per) {
            if (v.size() != vertices * per)
                v.clear();
        };
        fit(prim.normals, 3);
        fit(prim.uv0, 2);
        fit(prim.uv1, 2);
        fit(prim.colors, 4);
        fit(prim.weights, 4);
        fit(prim.joints, 4);
        const size_t jointCount = model->meshes[size_t(prim.mesh)].joints.size();
        if (prim.weights.empty() || prim.joints.empty() || jointCount == 0) {
            prim.weights.clear();
            prim.joints.clear();
        } else {
            for (size_t v = 0; v < vertices; ++v) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    float &w = prim.weights[v * 4 + size_t(k)];
                    quint16 &j = prim.joints[v * 4 + size_t(k)];
                    if (!(w > 0) || j >= jointCount) {
                        w = 0;
                        j = 0;
                    }
                    sum += w;
                }
                for (int k = 0; k < 4; ++k)
                    prim.weights[v * 4 + size_t(k)] = sum > 0 ? prim.weights[v * 4 + size_t(k)] / sum : (k == 0 ? 1.f : 0.f);
            }
        }
        std::vector<quint32> kept;
        kept.reserve(prim.indices.size());
        for (size_t t = 0; t + 2 < prim.indices.size(); t += 3)
            if (prim.indices[t] < vertices && prim.indices[t + 1] < vertices && prim.indices[t + 2] < vertices)
                kept.insert(kept.end(), prim.indices.begin() + qsizetype(t), prim.indices.begin() + qsizetype(t) + 3);
        prim.indices = std::move(kept);
    }
    std::erase_if(model->primitives, [](const ModelPrimitive &prim) { return prim.indices.empty(); });
    markEnvironmentBoxes(*model);
    return model;
}

class ModelTable : public SimpleTable
{
public:
    ModelTable(std::vector<Row> rows, std::shared_ptr<const Parsed> parsed)
        : SimpleTable(QObject::tr("Model"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_parsed(std::move(parsed))
    {
        setImageMaker([this] {
            const std::shared_ptr<const Model3D> m = model();
            return m && !m->primitives.empty() ? renderPreview(previewMeshes(*m)) : Image{};
        });
    }

    bool hasModel() const override { return true; }
    std::shared_ptr<const Model3D> model() const override
    {
        std::call_once(m_made, [this] {
            try {
                m_model = buildModel(*m_parsed);
            } catch (const std::bad_alloc &) {
                m_model.reset();
            }
        });
        return m_model;
    }

private:
    std::shared_ptr<const Parsed> m_parsed;
    mutable std::once_flag m_made;
    mutable std::shared_ptr<const Model3D> m_model;
};

} // namespace

int FalcomMdlReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("MDL ") ? 100 : 0;
}

std::unique_ptr<Document> FalcomMdlReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return fromBytes(*bytes, error, path);
}

std::unique_ptr<Document> FalcomMdlReader::fromBytes(const QByteArray &data, QString *error, const QString &path)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < 12 || !data.startsWith("MDL "))
        return fail(QObject::tr("Not a Falcom model (no \"MDL \" at the start)."));
    const quint32 version = qFromLittleEndian<quint32>(data.constData() + 4);

    QString sectionError;
    const std::vector<Section> sections = readSections(data, &sectionError);
    if (!sectionError.isEmpty())
        return fail(sectionError);

    auto parsed = std::make_shared<Parsed>();
    parsed->data = data;
    parsed->path = path;
    QStringList problems;
    for (const Section &s : sections) {
        Cursor c(data, s.start, s.end);
        switch (s.type) {
        case kSectionMaterials:
            if (!readMaterials(c, s.count, parsed->materials))
                problems << QObject::tr("materials only partly read");
            break;
        case kSectionMeshes:
            if (!readMeshes(c, s.count, parsed->meshes))
                problems << QObject::tr("meshes only partly read");
            break;
        case kSectionNodes:
            if (!readNodes(c, s.count, version, parsed->nodes))
                problems << QObject::tr("nodes only partly read");
            break;
        case kSectionVertexData:
            if (!readStreams(c, s.count, s.end, parsed->streams)) {
                problems << QObject::tr("vertex data could not be read");
                parsed->streams.clear();
            }
            break;
        default:
            break;
        }
    }
    const std::vector<Material> &materials = parsed->materials;
    const std::vector<Mesh> &meshes = parsed->meshes;
    const std::vector<Node> &nodes = parsed->nodes;
    const std::vector<Stream> &streams = parsed->streams;

    auto doc = std::make_unique<Document>();

    qint64 vertexCount = 0, triangleCount = 0, submeshCount = 0;
    for (const Stream &s : streams) {
        if (s.kind == kStreamPositions && s.stride == 12)
            vertexCount += s.bytes / 12;
        else if (s.kind == kStreamIndices && s.stride == 4)
            triangleCount += s.bytes / 12;
    }
    for (const Mesh &m : meshes)
        submeshCount += qint64(m.submeshes.size());
    std::vector<SimpleTable::Row> summaryRows = {
        {QObject::tr("version"), version},
        {QObject::tr("nodes"), qint64(nodes.size())},
        {QObject::tr("materials"), qint64(materials.size())},
        {QObject::tr("meshes"), qint64(meshes.size())},
        {QObject::tr("submeshes"), submeshCount},
        {QObject::tr("vertices"), vertexCount},
        {QObject::tr("triangles"), triangleCount},
    };
    if (!problems.isEmpty())
        summaryRows.push_back({QObject::tr("problems"), problems.join(QStringLiteral("; "))});
    const bool hasAnimation =
        std::any_of(sections.begin(), sections.end(), [](const Section &s) { return s.type == kSectionAnimation; });
    std::shared_ptr<SimpleTable> summary;
    if (triangleCount > 0) {
        summary = std::make_shared<ModelTable>(std::move(summaryRows), parsed);
    } else {
        summary = std::make_shared<SimpleTable>(
            QObject::tr("Model"), std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
            std::move(summaryRows));
    }
    if (triangleCount == 0 && hasAnimation)
        summary->setDescription(QObject::tr("animation only, no geometry"));
    else
        summary->setDescription(QObject::tr("%1 vertices, %2 triangles").arg(vertexCount).arg(triangleCount));
    doc->tables.push_back(summary);

    if (!nodes.empty()) {
        std::vector<qint64> parent(nodes.size(), -1);
        for (size_t i = 0; i < nodes.size(); ++i)
            for (quint32 child : nodes[i].children)
                if (child < nodes.size())
                    parent[child] = qint64(i);
        std::vector<SimpleTable::Row> rows;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const Node &n = nodes[i];
            rows.push_back({n.name, kindName(n.kind), n.mesh == 0xFFFFFFFF ? QVariant() : QVariant(n.mesh),
                            parent[i] < 0 ? QString() : nodes[size_t(parent[i])].name, vec3(n.position),
                            vec3(n.rotation), vec3(n.scale), qint64(n.children.size())});
        }
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Nodes"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("kind")}, {QObject::tr("mesh")},
                                      {QObject::tr("parent")}, {QObject::tr("position")}, {QObject::tr("rotation")},
                                      {QObject::tr("scale")}, {QObject::tr("children")}},
            std::move(rows));
        table->setDescription(QObject::tr("%1 nodes").arg(nodes.size()));
        doc->tables.push_back(table);
    }

    if (!materials.empty()) {
        std::vector<SimpleTable::Row> rows, parameterRows;
        for (const Material &m : materials) {
            QStringList textures;
            for (const TextureRef &t : m.textures)
                textures << t.name;
            rows.push_back({m.name, m.shader, textures.join(QStringLiteral(", ")), qint64(m.parameters.size()),
                            m.switches});
            for (const auto &[name, value] : m.parameters)
                parameterRows.push_back({m.name, name, value});
        }
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Materials"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("shader")},
                                      {QObject::tr("textures"), false, QObject::tr("Names of .xtx files")},
                                      {QObject::tr("parameters")}, {QObject::tr("switches")}},
            std::move(rows));
        table->setDescription(QObject::tr("%1 materials").arg(materials.size()));
        doc->tables.push_back(table);
        auto parameters = std::make_shared<SimpleTable>(
            QObject::tr("Material parameters"),
            std::vector<SimpleColumn>{{QObject::tr("material")}, {QObject::tr("parameter")}, {QObject::tr("value")}},
            std::move(parameterRows));
        doc->tables.push_back(parameters);
    }

    if (!streams.empty() || !meshes.empty()) {
        std::map<std::pair<quint32, quint32>, std::vector<const Stream *>> byKey;
        for (const Stream &s : streams)
            byKey[s.key()].push_back(&s);
        std::vector<SimpleTable::Row> rows;
        auto addRow = [&](quint32 meshIndex, quint32 submeshIndex, const std::vector<const Stream *> &list) {
            QString mesh, material;
            qint64 bones = 0;
            if (meshIndex < meshes.size()) {
                mesh = meshes[meshIndex].name;
                bones = qint64(meshes[meshIndex].bones.size());
                if (submeshIndex < meshes[meshIndex].submeshes.size()) {
                    const quint32 m = meshes[meshIndex].submeshes[submeshIndex].material;
                    if (m < materials.size())
                        material = materials[m].name;
                }
            }
            qint64 vertices = 0, triangles = 0;
            QStringList kinds;
            for (const Stream *s : list) {
                kinds << streamName(s->kind);
                if (s->kind == kStreamPositions && s->stride == 12)
                    vertices = s->bytes / 12;
                if (s->kind == kStreamIndices && s->stride == 4)
                    triangles = s->bytes / 12;
            }
            rows.push_back({mesh, qint64(submeshIndex), material, vertices, triangles, bones,
                            kinds.join(QStringLiteral(", "))});
        };
        for (quint32 m = 0; m < meshes.size(); ++m)
            for (quint32 s = 0; s < std::max<size_t>(meshes[m].submeshes.size(), 1); ++s) {
                const auto it = byKey.find({m, s});
                addRow(m, s, it != byKey.end() ? it->second : std::vector<const Stream *>{});
                if (it != byKey.end())
                    byKey.erase(it);
            }
        for (const auto &[key, list] : byKey)
            addRow(key.first, key.second, list);
        const size_t rowCount = rows.size();
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Meshes"),
            std::vector<SimpleColumn>{{QObject::tr("mesh")}, {QObject::tr("part")}, {QObject::tr("material")},
                                      {QObject::tr("vertices")}, {QObject::tr("triangles")}, {QObject::tr("bones")},
                                      {QObject::tr("data")}},
            std::move(rows));
        table->setDescription(QObject::tr("%1 meshes, %2 parts").arg(meshes.size()).arg(rowCount));
        doc->tables.push_back(table);
    }
    return doc;
}

QStringList FalcomMdlReader::animatedNodeNames(const QByteArray &data)
{
    if (data.size() < 12 || !data.startsWith("MDL "))
        return {};
    const std::vector<Section> sections = readSections(data, nullptr);
    if (std::any_of(sections.begin(), sections.end(), [](const Section &s) { return s.type == kSectionVertexData; }))
        return {};
    QStringList names;
    for (const Section &s : sections) {
        if (s.type != kSectionAnimation)
            continue;
        Cursor c(data, s.start, s.end);
        std::vector<Curve> curves;
        readCurves(c, s.count, curves, nullptr);
        for (const Curve &curve : curves)
            if (!names.contains(curve.bone))
                names << curve.bone;
    }
    return names;
}

std::optional<ModelAnimation> FalcomMdlReader::readAnimation(const QByteArray &data, const Model3D &model,
                                                             const QString &name, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<ModelAnimation> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < 12 || !data.startsWith("MDL "))
        return fail(QObject::tr("Not a Falcom model (no \"MDL \" at the start)."));
    QString sectionError;
    const std::vector<Section> sections = readSections(data, &sectionError);
    if (!sectionError.isEmpty())
        return fail(sectionError);

    std::vector<Curve> curves;
    std::optional<float> start;
    for (const Section &s : sections) {
        if (s.type != kSectionAnimation)
            continue;
        Cursor c(data, s.start, s.end);
        readCurves(c, s.count, curves, &start);
    }
    if (curves.empty())
        return fail(QObject::tr("The file has no bone animation."));

    float origin = std::numeric_limits<float>::max();
    for (const Curve &curve : curves)
        for (float t : curve.times)
            if (std::isfinite(t))
                origin = std::min(origin, t);
    if (start && *start < origin)
        origin = *start;

    std::map<QString, int> nodeByName;
    for (size_t i = 0; i < model.nodes.size(); ++i)
        nodeByName.emplace(model.nodes[i].name, int(i));

    ModelAnimation animation;
    animation.name = name;
    for (const Curve &curve : curves) {
        const auto found = nodeByName.find(curve.bone);
        if (found == nodeByName.end())
            continue;
        AnimationChannel channel;
        channel.node = found->second;
        channel.path = curve.type == kCurveRotation ? AnimationPath::Rotation
                     : curve.type == kCurveScale    ? AnimationPath::Scale
                                                    : AnimationPath::Translation;
        const size_t components = curve.type == kCurveRotation ? 4 : 3;
        const std::array<float, 4> orientation = model.nodes[size_t(channel.node)].orientation;
        std::array<float, 4> previous{0, 0, 0, 1};
        for (size_t k = 0; k < curve.times.size(); ++k) {
            const float t = curve.times[k] - origin;
            const float *v = curve.values.data() + k * components;
            if (!std::isfinite(t) || (!channel.times.empty() && !(t > channel.times.back()))
                || !std::all_of(v, v + components, [](float x) { return std::isfinite(x); }))
                continue;
            channel.times.push_back(t);
            if (channel.path == AnimationPath::Rotation) {
                std::array<float, 4> q = normalizedQuaternion(
                    multiplyQuaternions(orientation, normalizedQuaternion({v[0], v[1], v[2], v[3]})));
                if (channel.values.size() >= 4
                    && q[0] * previous[0] + q[1] * previous[1] + q[2] * previous[2] + q[3] * previous[3] < 0)
                    q = {-q[0], -q[1], -q[2], -q[3]};
                previous = q;
                channel.values.insert(channel.values.end(), q.begin(), q.end());
            } else {
                channel.values.insert(channel.values.end(), v, v + components);
            }
        }
        if (!channel.times.empty())
            animation.channels.push_back(std::move(channel));
    }
    if (animation.channels.empty())
        return fail(QObject::tr("None of the animated bones are in this model."));
    return animation;
}
