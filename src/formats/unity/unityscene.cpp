#include "unity/unityscene.h"

#include "core/format.h"
#include "unity/unitymesh.h"
#include "unity/unitytexture.h"

#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>

namespace {

constexpr float kSentinelTime = 1e30f;

enum Attribute { Position = 1, Rotation = 2, Scale = 3, Euler = 4 };

quint32 pathHash(const QString &path)
{
    const QByteArray bytes = path.toUtf8();
    return quint32(crc32(0, reinterpret_cast<const Bytef *>(bytes.constData()), uInt(bytes.size())));
}

std::array<float, 3> vector3(const UValue *v)
{
    if (!v)
        return {0, 0, 0};
    auto c = [v](const char16_t *name) {
        const UValue *x = v->child(name);
        return x ? float(x->toDouble()) : 0.f;
    };
    return {c(u"x"), c(u"y"), c(u"z")};
}

std::array<float, 4> quaternion(const UValue *v)
{
    if (!v)
        return {0, 0, 0, 1};
    auto c = [v](const char16_t *name, float fallback) {
        const UValue *x = v->child(name);
        return x ? float(x->toDouble()) : fallback;
    };
    return {c(u"x", 0), c(u"y", 0), c(u"z", 0), c(u"w", 1)};
}

std::array<float, 4> mirroredRotation(const std::array<float, 4> &q)
{
    return normalizedQuaternion({q[0], -q[1], -q[2], q[3]});
}

std::array<float, 4> unityEuler(float x, float y, float z)
{
    const float d = float(std::numbers::pi) / 360.f;
    const std::array<float, 4> qx{std::sin(x * d), 0, 0, std::cos(x * d)};
    const std::array<float, 4> qy{0, std::sin(y * d), 0, std::cos(y * d)};
    const std::array<float, 4> qz{0, 0, std::sin(z * d), std::cos(z * d)};
    return multiplyQuaternions(multiplyQuaternions(qy, qx), qz);
}

Matrix4 mirroredBindPose(const UValue &m)
{
    Matrix4 out = identityMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const UValue *e = m.child(QStringLiteral("e%1%2").arg(r).arg(c));
            const float sign = (r == 0) != (c == 0) ? -1.f : 1.f;
            out[size_t(c * 4 + r)] = e ? float(e->toDouble()) * sign : (r == c ? 1.f : 0.f);
        }
    return out;
}

const UValue *pointerField(const UValue &value, QStringView path)
{
    const UValue *p = value.at(path);
    return p && p->child(u"m_PathID") ? p : nullptr;
}

std::vector<float> floatArray(const UValue *array, bool bigEndian)
{
    std::vector<float> out;
    if (!array)
        return out;
    if (array->kind == UValue::Kind::Bytes && array->elementSize == 4) {
        const qint64 count = array->count();
        out.resize(size_t(count));
        for (qint64 i = 0; i < count; ++i) {
            const char *p = array->bytes.constData() + i * 4;
            const quint32 bits = bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
            std::memcpy(&out[size_t(i)], &bits, 4);
        }
    } else {
        for (const UValue &v : array->children)
            out.push_back(float(v.toDouble()));
    }
    return out;
}

struct Key
{
    float time = 0;
    std::array<float, 4> value{};
};

struct Track
{
    int node = -1;
    AnimationPath path = AnimationPath::Translation;
    std::vector<Key> keys;
};

struct Binding
{
    quint32 path = 0;
    int attribute = 0;
    int typeId = 0;
    int first = 0;
    int width = 1;
};

bool looksLikeNormalMap(const QString &name)
{
    static const QStringList words = {"n", "nml", "nrm", "nor", "norm", "normal", "normals", "bump", "nm"};
    const QStringList parts = unityNameWords(name);
    return parts.size() > 1 && std::any_of(parts.begin() + 1, parts.end(), [](const QString &p) { return words.contains(p); });
}

bool looksLikeDataMap(const QString &name)
{
    static const QStringList words = {"mask", "msk", "frmask", "mass", "emm", "emi", "emission", "emissive", "spec",
                                      "specular", "rough", "roughness", "metal", "metallic", "ao", "occ", "occlusion",
                                      "height", "disp", "matcap", "ramp", "lut", "noise", "detail", "light", "shadow"};
    const QStringList parts = unityNameWords(name);
    return std::any_of(parts.begin() + (parts.size() > 1 ? 1 : 0), parts.end(),
                       [](const QString &p) { return words.contains(p); });
}

class Builder
{
public:
    Builder(const UnityScene &scene, Model3D &model, UnitySceneSummary &summary)
        : m_scene(scene), m_model(model), m_summary(summary)
    {
    }

    bool build(int file, qint64 rootPathId, QString *error)
    {
        const UnitySceneObject *root = m_scene.find(file, rootPathId);
        const UnitySceneObject *rootTransform = root ? transformOf(*root) : nullptr;
        if (!rootTransform) {
            if (error)
                *error = QObject::tr("This game object has no transform.");
            return false;
        }
        addTransform(*rootTransform, -1, {});
        for (const UnitySceneObject *transform : m_order)
            addRenderers(*transform, m_nodeOf.at(transform));
        if (m_model.primitives.empty()) {
            if (error)
                *error = QObject::tr("This game object has no meshes the app can read.");
            return false;
        }
        m_summary.bones = int(m_model.nodes.size());
        m_summary.materials = int(m_model.materials.size());
        m_summary.textures = int(m_model.textures.size());
        addEmbeddedTextures();
        for (const UnitySceneObject &object : m_scene.objects)
            if (object.classId == unityclass::AnimationClip)
                addAnimation(object);
        m_summary.animations = int(m_model.animations.size());
        return true;
    }

private:
    const UnitySceneObject *resolve(const UnitySceneObject &from, const UValue *pointer) const
    {
        return pointer ? m_scene.resolve(from.file, pointer) : nullptr;
    }

    const UnitySceneObject *transformOf(const UnitySceneObject &gameObject) const
    {
        for (const UnitySceneObject *c : components(gameObject))
            if (c->classId == unityclass::Transform || c->classId == unityclass::RectTransform)
                return c;
        return nullptr;
    }

    std::vector<const UnitySceneObject *> components(const UnitySceneObject &gameObject) const
    {
        std::vector<const UnitySceneObject *> out;
        if (const UValue *list = gameObject.value.child(u"m_Component"))
            for (const UValue &pair : list->children)
                if (const UnitySceneObject *c = resolve(gameObject, pair.child(u"component") ? pair.child(u"component")
                                                                                            : pair.child(u"second")))
                    out.push_back(c);
        return out;
    }

    void addTransform(const UnitySceneObject &transform, int parent, const QString &parentPath)
    {
        if (m_nodeOf.count(&transform) || m_model.nodes.size() > 65535)
            return;
        const UnitySceneObject *gameObject = resolve(transform, pointerField(transform.value, u"m_GameObject"));
        const UValue *nameValue = gameObject ? gameObject->value.child(u"m_Name") : nullptr;
        QString name = nameValue ? nameValue->toString() : QString();
        if (name.isEmpty())
            name = QStringLiteral("node %1").arg(m_model.nodes.size());
        ModelNode node;
        node.name = name;
        node.parent = parent;
        const std::array<float, 3> t = vector3(transform.value.child(u"m_LocalPosition"));
        node.translation = {-t[0], t[1], t[2]};
        node.rotation = mirroredRotation(quaternion(transform.value.child(u"m_LocalRotation")));
        const std::array<float, 3> s = vector3(transform.value.child(u"m_LocalScale"));
        node.scale = s;
        const int index = int(m_model.nodes.size());
        m_model.nodes.push_back(node);
        m_nodeOf[&transform] = index;
        m_order.push_back(&transform);
        if (gameObject)
            m_gameObjectOf[&transform] = gameObject;
        const QString path = parentPath.isEmpty() ? name : parentPath + u'/' + name;
        for (QString part = path;;) {
            const quint32 hash = pathHash(part);
            if (!m_hashToNode.contains(hash))
                m_hashToNode.insert(hash, index);
            const qsizetype slash = part.indexOf(u'/');
            if (slash < 0)
                break;
            part = part.mid(slash + 1);
        }
        if (parent < 0 && !m_hashToNode.contains(pathHash(QString())))
            m_hashToNode.insert(pathHash(QString()), index);
        if (const UValue *children = transform.value.child(u"m_Children"))
            for (const UValue &child : children->children)
                if (const UnitySceneObject *c = resolve(transform, &child))
                    addTransform(*c, index, path);
    }

    void addRenderers(const UnitySceneObject &transform, int node)
    {
        const auto go = m_gameObjectOf.find(&transform);
        if (go == m_gameObjectOf.end())
            return;
        const std::vector<const UnitySceneObject *> parts = components(*go->second);
        for (const UnitySceneObject *c : parts) {
            if (c->classId == unityclass::SkinnedMeshRenderer) {
                if (const UnitySceneObject *mesh = resolve(*c, pointerField(c->value, u"m_Mesh")))
                    addMesh(*c, *mesh, node, true, go->second);
            } else if (c->classId == unityclass::MeshRenderer) {
                for (const UnitySceneObject *f : parts)
                    if (f->classId == unityclass::MeshFilter)
                        if (const UnitySceneObject *mesh = resolve(*f, pointerField(f->value, u"m_Mesh")))
                            addMesh(*c, *mesh, node, false, go->second);
            }
        }
    }

    int nodeForBone(const UnitySceneObject &renderer, const UValue *bone, quint32 hash) const
    {
        if (const UnitySceneObject *t = resolve(renderer, bone))
            if (const auto it = m_nodeOf.find(t); it != m_nodeOf.end())
                return it->second;
        return m_hashToNode.value(hash, -1);
    }

    void addMesh(const UnitySceneObject &renderer, const UnitySceneObject &meshObject, int node, bool skinned,
                 const UnitySceneObject *gameObject)
    {
        if (meshObject.classId != unityclass::Mesh)
            return;
        std::vector<int> submeshOf;
        QString error;
        const std::shared_ptr<Model3D> part = unityMeshModel(meshObject.value, meshObject.resource, m_scene.unityVersion,
                                                             m_scene.bigEndian, &error, &submeshOf);
        if (!part)
            return;
        ModelMesh mesh;
        const UValue *goName = gameObject ? gameObject->value.child(u"m_Name") : nullptr;
        mesh.name = goName && !goName->toString().isEmpty() ? goName->toString() : objectName(meshObject.value);
        mesh.node = node;
        bool hasSkin = false;
        if (skinned) {
            const UValue *bindPoses = meshObject.value.child(u"m_BindPose");
            const UValue *bones = renderer.value.child(u"m_Bones");
            const UValue *hashes = meshObject.value.child(u"m_BoneNameHashes");
            const qsizetype count = bindPoses ? qsizetype(bindPoses->children.size()) : 0;
            for (qsizetype i = 0; i < count; ++i) {
                const UValue *bone = bones && i < qsizetype(bones->children.size()) ? &bones->children[size_t(i)] : nullptr;
                quint32 hash = 0;
                if (hashes && hashes->kind == UValue::Kind::Bytes && i < hashes->count()) {
                    const char *p = hashes->bytes.constData() + i * 4;
                    hash = m_scene.bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
                } else if (hashes && i < qsizetype(hashes->children.size())) {
                    hash = quint32(hashes->children[size_t(i)].toInt());
                }
                const int boneNode = nodeForBone(renderer, bone, hash);
                mesh.joints.push_back(boneNode >= 0 ? boneNode : 0);
                mesh.inverseBinds.push_back(mirroredBindPose(bindPoses->children[size_t(i)]));
            }
            hasSkin = !mesh.joints.empty();
            if (hasSkin)
                ++m_summary.skinned;
        }
        const int meshIndex = int(m_model.meshes.size());
        m_model.meshes.push_back(std::move(mesh));
        ++m_summary.meshes;
        const UValue *materials = renderer.value.child(u"m_Materials");
        for (size_t p = 0; p < part->primitives.size(); ++p) {
            ModelPrimitive prim = std::move(part->primitives[p]);
            prim.mesh = meshIndex;
            const int submesh = p < submeshOf.size() ? submeshOf[p] : int(p);
            const UValue *material = materials && submesh < int(materials->children.size())
                                         ? &materials->children[size_t(submesh)]
                                         : nullptr;
            prim.material = materialIndex(renderer, material);
            if (!hasSkin) {
                prim.joints.clear();
                prim.weights.clear();
            }
            m_model.primitives.push_back(std::move(prim));
        }
        m_model.staticMeshesUseNodeTransform = true;
    }

    static bool hasKeyword(const UValue &material, const QString &keyword)
    {
        if (const UValue *k = material.child(u"m_ShaderKeywords"); k && k->toString().split(u' ').contains(keyword))
            return true;
        for (const char16_t *field : {u"m_ValidKeywords", u"m_InvalidKeywords"})
            if (const UValue *list = material.child(field))
                for (const UValue &v : list->children)
                    if (v.toString() == keyword)
                        return true;
        return false;
    }

    static std::optional<float> floatProperty(const UValue &material, QStringView name)
    {
        if (const UValue *floats = material.at(u"m_SavedProperties.m_Floats"))
            for (const UValue &pair : floats->children) {
                const UValue *first = pair.child(u"first"), *second = pair.child(u"second");
                if (first && second && first->toString() == name)
                    return float(second->toDouble());
            }
        return std::nullopt;
    }

    int textureFor(const UnitySceneObject &material, const QStringList &exact, const QStringList &contains)
    {
        const UValue *envs = material.value.at(u"m_SavedProperties.m_TexEnvs");
        if (!envs)
            return -1;
        auto pick = [&](auto &&matches) -> int {
            for (const UValue &pair : envs->children) {
                const UValue *first = pair.child(u"first");
                const UnitySceneObject *texture = resolve(material, pointerField(pair, u"second.m_Texture"));
                if (first && texture && texture->classId == unityclass::Texture2D && matches(first->toString()))
                    return textureIndex(*texture);
            }
            return -1;
        };
        for (const QString &name : exact)
            if (const int t = pick([&name](const QString &n) { return n == name; }); t >= 0)
                return t;
        return pick([&contains](const QString &n) {
            const QString lower = n.toLower();
            return std::any_of(contains.begin(), contains.end(), [&lower](const QString &c) { return lower.contains(c); });
        });
    }

    std::vector<const UnitySceneObject *> materialTextures(const UnitySceneObject &material) const
    {
        std::vector<const UnitySceneObject *> out;
        if (const UValue *envs = material.value.at(u"m_SavedProperties.m_TexEnvs"))
            for (const UValue &pair : envs->children)
                if (const UnitySceneObject *t = resolve(material, pointerField(pair, u"second.m_Texture"));
                    t && t->classId == unityclass::Texture2D && std::find(out.begin(), out.end(), t) == out.end())
                    out.push_back(t);
        return out;
    }

    int textureIndex(const UnitySceneObject &texture)
    {
        if (const auto it = m_textureIndex.find(&texture); it != m_textureIndex.end())
            return it->second;
        QString name = objectName(texture.value);
        if (name.isEmpty())
            name = QStringLiteral("texture");
        QString unique = name;
        for (int n = 2; m_textureNames.contains(unique); ++n)
            unique = QStringLiteral("%1 (%2)").arg(name).arg(n);
        m_textureNames.insert(unique);
        const int index = int(m_model.textures.size());
        m_model.textures.push_back({unique});
        m_textureObjects.push_back(&texture);
        m_textureIndex[&texture] = index;
        return index;
    }

    void texturesByName(const UnitySceneObject &material, ModelMaterial &m)
    {
        const QString materialName = objectName(material.value);
        const UnitySceneObject *color = nullptr, *only = nullptr;
        QString colorName;
        int colors = 0;
        for (const UnitySceneObject &o : m_scene.objects) {
            if (o.classId != unityclass::Texture2D || o.file != material.file)
                continue;
            const QString name = objectName(o.value);
            if (!unityColorTextureName(name))
                continue;
            ++colors;
            only = &o;
            if (unityNameContains(materialName, name) && (!color || name.size() > colorName.size())) {
                color = &o;
                colorName = name;
            }
        }
        if (!color && colors == 1) {
            color = only;
            colorName = objectName(only->value);
        }
        if (!color)
            return;
        m.baseColorTexture = textureIndex(*color);
        const QString prefix = colorName.toLower() + u'_';
        const UnitySceneObject *normal = nullptr;
        for (const UnitySceneObject &o : m_scene.objects) {
            if (o.classId != unityclass::Texture2D || o.file != material.file)
                continue;
            const QString name = objectName(o.value);
            if (name.toLower().startsWith(prefix) && looksLikeNormalMap(name)
                && (!normal || name.size() < objectName(normal->value).size()))
                normal = &o;
        }
        if (normal && m.normalTexture < 0)
            m.normalTexture = textureIndex(*normal);
    }

    int materialIndex(const UnitySceneObject &renderer, const UValue *pointer)
    {
        const UnitySceneObject *material = resolve(renderer, pointer);
        if (!material || material->classId != unityclass::Material) {
            if (m_defaultMaterial < 0) {
                m_defaultMaterial = int(m_model.materials.size());
                m_model.materials.push_back({QObject::tr("default")});
            }
            return m_defaultMaterial;
        }
        if (const auto it = m_materialIndex.find(material); it != m_materialIndex.end())
            return it->second;
        ModelMaterial m;
        m.name = objectName(material->value);
        m.baseColorTexture = textureFor(*material,
                                        {"_BaseMap", "_MainTex", "_BaseColorMap", "_MainTexture", "_Albedo", "_AlbedoMap",
                                         "_DiffuseMap", "_Diffuse", "_BaseTex", "_ColorMap"},
                                        {"basemap", "maintex", "albedo", "diffuse", "basecolor", "base", "main", "color"});
        m.normalTexture = textureFor(*material, {"_BumpMap", "_NormalMap", "_NormalTex"}, {"normal", "bump"});
        if (m.baseColorTexture < 0 || m.normalTexture < 0) {
            const std::vector<const UnitySceneObject *> all = materialTextures(*material);
            if (m.normalTexture < 0)
                for (const UnitySceneObject *t : all)
                    if (looksLikeNormalMap(objectName(t->value))) {
                        m.normalTexture = textureIndex(*t);
                        break;
                    }
            if (m.baseColorTexture < 0) {
                const UnitySceneObject *best = nullptr;
                for (const UnitySceneObject *t : all) {
                    const QString name = objectName(t->value);
                    if (looksLikeNormalMap(name) || looksLikeDataMap(name))
                        continue;
                    if (!best || name.size() < objectName(best->value).size())
                        best = t;
                }
                if (best)
                    m.baseColorTexture = textureIndex(*best);
            }
        }
        if (m.baseColorTexture < 0 && !unityMaterialHasTextures(material->value))
            texturesByName(*material, m);
        const UValue *queue = material->value.child(u"m_CustomRenderQueue");
        if (hasKeyword(material->value, QStringLiteral("_ALPHATEST_ON"))) {
            m.alphaMode = AlphaMode::Mask;
            m.alphaCutoff = floatProperty(material->value, u"_Cutoff").value_or(0.5f);
        } else if ((queue && queue->toInt() >= 3000) || hasKeyword(material->value, QStringLiteral("_SURFACE_TYPE_TRANSPARENT"))
                   || hasKeyword(material->value, QStringLiteral("_ALPHABLEND_ON"))) {
            m.alphaMode = AlphaMode::Blend;
        }
        const int index = int(m_model.materials.size());
        m_model.materials.push_back(std::move(m));
        m_materialIndex[material] = index;
        return index;
    }

    void addEmbeddedTextures()
    {
        if (m_textureObjects.empty())
            return;
        struct Source
        {
            UValue value;
            QByteArray data;
        };
        auto sources = std::make_shared<QHash<QString, Source>>();
        for (size_t i = 0; i < m_textureObjects.size(); ++i) {
            const UnitySceneObject &t = *m_textureObjects[i];
            QByteArray data = t.resource;
            if (data.isEmpty())
                if (const UValue *inline_ = t.value.child(u"image data"))
                    data = inline_->bytes;
            sources->insert(m_model.textures[i].name, Source{t.value, std::move(data)});
        }
        const qint32 platform = m_scene.platform;
        m_model.embeddedTexture = [sources, platform](const QString &name) -> std::shared_ptr<const Image> {
            const auto it = sources->constFind(name);
            if (it == sources->cend())
                return nullptr;
            std::optional<Image> image = decodeUnityTexture(it->value, it->data, platform, nullptr);
            if (!image || image->width <= 0 || image->height <= 0)
                return nullptr;
            return std::make_shared<const Image>(std::move(*image));
        };
    }

    void addKey(std::map<std::pair<int, int>, Track> &tracks, int node, AnimationPath path, float time,
                const std::array<float, 4> &value)
    {
        Track &track = tracks[{node, int(path)}];
        track.node = node;
        track.path = path;
        track.keys.push_back({time, value});
    }

    void addCurveValues(std::map<std::pair<int, int>, Track> &tracks, const Binding &binding, float time,
                        const float *v)
    {
        const int node = m_hashToNode.value(binding.path, -1);
        if (node < 0)
            return;
        switch (binding.attribute) {
        case Position: addKey(tracks, node, AnimationPath::Translation, time, {-v[0], v[1], v[2], 0}); break;
        case Rotation: addKey(tracks, node, AnimationPath::Rotation, time, mirroredRotation({v[0], v[1], v[2], v[3]})); break;
        case Scale: addKey(tracks, node, AnimationPath::Scale, time, {v[0], v[1], v[2], 0}); break;
        case Euler: addKey(tracks, node, AnimationPath::Rotation, time, mirroredRotation(unityEuler(v[0], v[1], v[2]))); break;
        default: break;
        }
    }

    void addLegacyCurves(std::map<std::pair<int, int>, Track> &tracks, const UValue &clip)
    {
        struct Kind
        {
            const char16_t *field;
            int attribute;
        };
        for (const Kind kind : {Kind{u"m_PositionCurves", Position}, Kind{u"m_RotationCurves", Rotation},
                                Kind{u"m_ScaleCurves", Scale}, Kind{u"m_EulerCurves", Euler}}) {
            const UValue *curves = clip.child(kind.field);
            if (!curves)
                continue;
            for (const UValue &curve : curves->children) {
                const UValue *path = curve.child(u"path");
                const UValue *keys = curve.at(u"curve.m_Curve");
                if (!path || !keys)
                    continue;
                const Binding binding{pathHash(path->toString()), kind.attribute, unityclass::Transform, 0, 0};
                for (const UValue &key : keys->children) {
                    const UValue *time = key.child(u"time");
                    if (!time)
                        continue;
                    const std::array<float, 4> q = quaternion(key.child(u"value"));
                    addCurveValues(tracks, binding, float(time->toDouble()), q.data());
                }
            }
        }
    }

    void addMuscleCurves(std::map<std::pair<int, int>, Track> &tracks, const UValue &clip)
    {
        const UValue *generic = clip.at(u"m_ClipBindingConstant.genericBindings");
        const UValue *data = clip.at(u"m_MuscleClip.m_Clip.data");
        if (!generic || !data)
            return;
        std::vector<Binding> bindings;
        int curves = 0;
        for (const UValue &g : generic->children) {
            Binding b;
            b.path = quint32(g.child(u"path") ? g.child(u"path")->toInt() : 0);
            b.attribute = int(g.child(u"attribute") ? g.child(u"attribute")->toInt() : 0);
            b.typeId = int(g.child(u"typeID") ? g.child(u"typeID")->toInt() : 0);
            b.first = curves;
            b.width = b.typeId == unityclass::Transform
                          ? (b.attribute == Rotation ? 4 : (b.attribute == Position || b.attribute == Scale || b.attribute == Euler) ? 3 : 1)
                          : 1;
            curves += b.width;
            bindings.push_back(b);
        }
        auto bindingAt = [&bindings](int curve) -> const Binding * {
            auto it = std::upper_bound(bindings.begin(), bindings.end(), curve,
                                       [](int c, const Binding &b) { return c < b.first; });
            if (it == bindings.begin())
                return nullptr;
            --it;
            return curve < it->first + it->width ? &*it : nullptr;
        };
        auto take = [&](int curve, float time, const std::vector<float> &values, size_t at, size_t available) -> int {
            const Binding *b = bindingAt(curve);
            if (!b || b->typeId != unityclass::Transform || b->first != curve || at + size_t(b->width) > available)
                return 1;
            if (b->width >= 3)
                addCurveValues(tracks, *b, time, values.data() + at);
            return b->width;
        };

        const UValue *streamed = data->at(u"m_StreamedClip.data");
        const UValue *streamCountValue = data->at(u"m_StreamedClip.curveCount");
        const int streamCount = streamCountValue ? int(streamCountValue->toInt()) : 0;
        const std::vector<float> words = floatArray(streamed, m_scene.bigEndian);
        std::vector<quint32> raw(words.size());
        std::memcpy(raw.data(), words.data(), words.size() * 4);
        for (size_t at = 0; at + 2 <= words.size();) {
            const float time = words[at];
            const qint64 keyCount = qint32(raw[at + 1]);
            at += 2;
            if (keyCount < 0 || at + size_t(keyCount) * 5 > words.size())
                break;
            std::vector<int> indices(static_cast<size_t>(keyCount));
            std::vector<float> values(static_cast<size_t>(keyCount));
            for (qint64 k = 0; k < keyCount; ++k) {
                indices[size_t(k)] = qint32(raw[at + size_t(k) * 5]);
                values[size_t(k)] = words[at + size_t(k) * 5 + 4];
            }
            at += size_t(keyCount) * 5;
            if (!std::isfinite(time) || std::abs(time) > kSentinelTime)
                continue;
            for (size_t k = 0; k < indices.size();)
                k += size_t(take(indices[k], time, values, k, values.size()));
        }

        const UValue *dense = data->child(u"m_DenseClip");
        if (dense) {
            const int frames = int(dense->child(u"m_FrameCount") ? dense->child(u"m_FrameCount")->toInt() : 0);
            const int count = int(dense->child(u"m_CurveCount") ? dense->child(u"m_CurveCount")->toInt() : 0);
            const float rate = dense->child(u"m_SampleRate") ? float(dense->child(u"m_SampleRate")->toDouble()) : 0;
            const float begin = dense->child(u"m_BeginTime") ? float(dense->child(u"m_BeginTime")->toDouble()) : 0;
            const std::vector<float> samples = floatArray(dense->child(u"m_SampleArray"), m_scene.bigEndian);
            if (count > 0 && rate > 0 && samples.size() >= size_t(frames) * size_t(count))
                for (int f = 0; f < frames; ++f) {
                    const float time = begin + float(f) / rate;
                    const size_t offset = size_t(f) * size_t(count);
                    std::vector<float> frame(samples.begin() + qsizetype(offset), samples.begin() + qsizetype(offset + size_t(count)));
                    for (int c = 0; c < count;)
                        c += take(streamCount + c, time, frame, size_t(c), frame.size());
                }
        }

        const std::vector<float> constants = floatArray(data->at(u"m_ConstantClip.data"), m_scene.bigEndian);
        const int denseCount = dense && dense->child(u"m_CurveCount") ? int(dense->child(u"m_CurveCount")->toInt()) : 0;
        const UValue *stop = clip.at(u"m_MuscleClip.m_StopTime");
        const float stopTime = stop ? float(stop->toDouble()) : 0.f;
        for (float time : {0.f, stopTime}) {
            for (int c = 0; c < int(constants.size());)
                c += take(streamCount + denseCount + c, time, constants, size_t(c), constants.size());
            if (stopTime <= 0)
                break;
        }
    }

    void addAnimation(const UnitySceneObject &object)
    {
        std::map<std::pair<int, int>, Track> tracks;
        const UValue *legacy = object.value.child(u"m_Legacy");
        if (legacy && legacy->toInt())
            addLegacyCurves(tracks, object.value);
        else
            addMuscleCurves(tracks, object.value);
        if (tracks.empty()) {
            ++m_summary.skippedAnimations;
            return;
        }
        ModelAnimation animation;
        QString name = objectName(object.value);
        if (name.isEmpty())
            name = QStringLiteral("animation");
        QString unique = name;
        for (int n = 2; m_animationNames.contains(unique); ++n)
            unique = QStringLiteral("%1_%2").arg(name).arg(n);
        m_animationNames.insert(unique);
        animation.name = unique;
        for (auto &[key, track] : tracks) {
            std::stable_sort(track.keys.begin(), track.keys.end(), [](const Key &a, const Key &b) { return a.time < b.time; });
            AnimationChannel channel;
            channel.node = track.node;
            channel.path = track.path;
            const int components = track.path == AnimationPath::Rotation ? 4 : 3;
            for (size_t i = 0; i < track.keys.size(); ++i) {
                if (!channel.times.empty() && track.keys[i].time <= channel.times.back()) {
                    channel.values.resize(channel.values.size() - size_t(components));
                    channel.times.pop_back();
                }
                channel.times.push_back(std::max(0.f, track.keys[i].time));
                channel.values.insert(channel.values.end(), track.keys[i].value.begin(),
                                      track.keys[i].value.begin() + components);
            }
            if (!channel.times.empty())
                animation.channels.push_back(std::move(channel));
        }
        if (animation.channels.empty()) {
            ++m_summary.skippedAnimations;
            return;
        }
        m_model.animations.push_back(std::move(animation));
    }

    const UnityScene &m_scene;
    Model3D &m_model;
    UnitySceneSummary &m_summary;
    std::map<const UnitySceneObject *, int> m_nodeOf;
    std::vector<const UnitySceneObject *> m_order;
    std::map<const UnitySceneObject *, const UnitySceneObject *> m_gameObjectOf;
    QHash<quint32, int> m_hashToNode;
    std::map<const UnitySceneObject *, int> m_materialIndex;
    std::map<const UnitySceneObject *, int> m_textureIndex;
    std::vector<const UnitySceneObject *> m_textureObjects;
    QSet<QString> m_textureNames;
    QSet<QString> m_animationNames;
    int m_defaultMaterial = -1;
};

}

void UnityScene::buildIndex()
{
    index.clear();
    for (size_t i = 0; i < objects.size(); ++i)
        index.emplace(std::make_pair(objects[i].file, objects[i].pathId), i);
}

const UnitySceneObject *UnityScene::find(int file, qint64 pathId) const
{
    if (!index.empty()) {
        const auto it = index.find({file, pathId});
        return it == index.end() ? nullptr : &objects[it->second];
    }
    for (const UnitySceneObject &o : objects)
        if (o.file == file && o.pathId == pathId)
            return &o;
    return nullptr;
}

const UnitySceneObject *UnityScene::resolve(int file, const UValue *pointer) const
{
    const UValue *fileId = pointer ? pointer->child(u"m_FileID") : nullptr;
    const UValue *pathId = pointer ? pointer->child(u"m_PathID") : nullptr;
    if (!fileId || !pathId || pathId->toInt() == 0)
        return nullptr;
    int target = file;
    if (const qint64 id = fileId->toInt(); id != 0) {
        if (file < 0 || size_t(file) >= externals.size() || id < 1 || id > externals[size_t(file)].size())
            return nullptr;
        target = externals[size_t(file)][qsizetype(id - 1)];
        if (target < 0)
            return nullptr;
    }
    return find(target, pathId->toInt());
}

QStringList unityNameWords(const QString &name)
{
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    return name.toLower().split(separators, Qt::SkipEmptyParts);
}

bool unityColorTextureName(const QString &name)
{
    return !looksLikeNormalMap(name) && !looksLikeDataMap(name);
}

bool unityNameContains(const QString &name, const QString &part)
{
    const QStringList words = unityNameWords(name), wanted = unityNameWords(part);
    if (wanted.isEmpty() || wanted.size() > words.size())
        return false;
    for (qsizetype at = 0; at + wanted.size() <= words.size(); ++at)
        if (std::equal(wanted.begin(), wanted.end(), words.begin() + at))
            return true;
    return false;
}

bool unityMaterialHasTextures(const UValue &material)
{
    if (const UValue *envs = material.at(u"m_SavedProperties.m_TexEnvs"))
        for (const UValue &pair : envs->children)
            if (const UValue *id = pair.at(u"second.m_Texture.m_PathID"); id && id->toInt() != 0)
                return true;
    return false;
}

std::vector<const UValue *> unityScenePointers(int classId, const UValue &value)
{
    std::vector<const UValue *> out;
    auto one = [&](QStringView path) {
        if (const UValue *p = pointerField(value, path))
            out.push_back(p);
    };
    auto each = [&](QStringView array, QStringView field) {
        if (const UValue *list = value.at(array))
            for (const UValue &item : list->children) {
                const UValue *p = field.isEmpty() ? &item : item.at(field);
                if (p && p->child(u"m_PathID"))
                    out.push_back(p);
            }
    };
    switch (classId) {
    case unityclass::GameObject:
        each(u"m_Component", u"component");
        break;
    case unityclass::Transform:
    case unityclass::RectTransform:
        each(u"m_Children", {});
        one(u"m_GameObject");
        break;
    case unityclass::SkinnedMeshRenderer:
        one(u"m_Mesh");
        each(u"m_Materials", {});
        each(u"m_Bones", {});
        break;
    case unityclass::MeshRenderer:
        each(u"m_Materials", {});
        break;
    case unityclass::MeshFilter:
        one(u"m_Mesh");
        break;
    case unityclass::Material:
        each(u"m_SavedProperties.m_TexEnvs", u"second.m_Texture");
        break;
    case unityclass::Animator:
        one(u"m_Controller");
        break;
    case unityclass::AnimatorController:
        each(u"m_AnimationClips", {});
        break;
    case unityclass::AnimatorOverrideController:
        one(u"m_Controller");
        each(u"m_Clips", u"m_OverrideClip");
        break;
    default:
        break;
    }
    return out;
}

std::shared_ptr<Model3D> unitySceneModel(const UnityScene &scene, int file, qint64 rootPathId,
                                         UnitySceneSummary *summary, QString *error)
{
    auto model = std::make_shared<Model3D>();
    UnitySceneSummary local;
    Builder builder(scene, *model, summary ? *summary : local);
    if (!builder.build(file, rootPathId, error))
        return nullptr;
    return model;
}
