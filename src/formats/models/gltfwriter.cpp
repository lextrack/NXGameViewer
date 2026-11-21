#include "models/gltfwriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <map>

namespace {

constexpr int kFloat = 5126, kUnsignedByte = 5121, kUnsignedShort = 5123, kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

quint32 crc32(const QByteArray &data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (char byte : data)
        c = table[(c ^ uchar(byte)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void appendBigEndian(QByteArray &out, quint32 v)
{
    char raw[4];
    qToBigEndian(v, raw);
    out.append(raw, 4);
}

void appendChunk(QByteArray &png, const char *type, const QByteArray &data)
{
    appendBigEndian(png, quint32(data.size()));
    QByteArray typed(type, 4);
    typed += data;
    png += typed;
    appendBigEndian(png, crc32(typed));
}

class BinaryBuilder
{
public:
    QJsonArray bufferViews, accessors;
    QByteArray bin;

    int addView(const void *data, qsizetype size, int target = 0)
    {
        while (bin.size() % 4)
            bin.append('\0');
        QJsonObject view{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", size}};
        if (target)
            view["target"] = target;
        bin.append(static_cast<const char *>(data), size);
        bufferViews.append(view);
        return int(bufferViews.size() - 1);
    }

    template <typename T>
    int addAccessor(const std::vector<T> &values, int components, int componentType, const char *type,
                    bool normalized = false, int target = kArrayBuffer, bool minMax = false)
    {
        const int view = addView(values.data(), qsizetype(values.size() * sizeof(T)), target);
        QJsonObject accessor{{"bufferView", view},
                             {"componentType", componentType},
                             {"count", qint64(values.size() / size_t(components))},
                             {"type", type}};
        if (normalized)
            accessor["normalized"] = true;
        if (minMax) {
            QJsonArray lo, hi;
            for (int c = 0; c < components; ++c) {
                double mn = std::numeric_limits<double>::max(), mx = std::numeric_limits<double>::lowest();
                for (size_t i = size_t(c); i < values.size(); i += size_t(components)) {
                    mn = std::min(mn, double(values[i]));
                    mx = std::max(mx, double(values[i]));
                }
                lo.append(mn);
                hi.append(mx);
            }
            accessor["min"] = lo;
            accessor["max"] = hi;
        }
        accessors.append(accessor);
        return int(accessors.size() - 1);
    }
};

Image completedNormalMap(const Image &image)
{
    Image out = image;
    auto *p = reinterpret_cast<uchar *>(out.rgba.data());
    const qsizetype pixels = qsizetype(out.width) * out.height;
    bool blueMissing = true;
    for (qsizetype i = 0; i < pixels && blueMissing; ++i)
        blueMissing = p[i * 4 + 2] == 0;
    if (!blueMissing)
        return out;
    for (qsizetype i = 0; i < pixels; ++i) {
        const float x = p[i * 4] / 127.5f - 1.f, y = p[i * 4 + 1] / 127.5f - 1.f;
        const float z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
        p[i * 4 + 2] = uchar(std::lround((z + 1.f) * 127.5f));
        p[i * 4 + 3] = 255;
    }
    return out;
}

Image doubledImage(const Image &source, bool doubleU, bool mirrorU, bool doubleV, bool mirrorV)
{
    const int w = source.width, h = source.height;
    Image out;
    out.width = doubleU ? w * 2 : w;
    out.height = doubleV ? h * 2 : h;
    out.rgba.resize(qsizetype(out.width) * out.height * 4);
    const auto *src = reinterpret_cast<const quint32 *>(source.rgba.constData());
    auto *dst = reinterpret_cast<quint32 *>(out.rgba.data());
    for (int y = 0; y < out.height; ++y) {
        int sy = y;
        if (y >= h)
            sy = mirrorV ? 2 * h - 1 - y : y - h;
        for (int x = 0; x < out.width; ++x) {
            int sx = x;
            if (x >= w)
                sx = mirrorU ? 2 * w - 1 - x : x - w;
            dst[qsizetype(y) * out.width + x] = src[qsizetype(sy) * w + sx];
        }
    }
    return out;
}

int wrapMode(int mdlWrap)
{
    switch (mdlWrap) {
    case 1: return 33648;
    case 2: return 33071;
    default: return 10497;
    }
}

}

QByteArray encodePng(const Image &image)
{
    QByteArray raw;
    const qsizetype rowBytes = qsizetype(image.width) * 4;
    raw.reserve((rowBytes + 1) * image.height);
    for (int y = 0; y < image.height; ++y) {
        raw.append('\0');
        raw.append(image.rgba.constData() + y * rowBytes, rowBytes);
    }
    QByteArray header;
    appendBigEndian(header, quint32(image.width));
    appendBigEndian(header, quint32(image.height));
    header.append(char(8)).append(char(6)).append(char(0)).append(char(0)).append(char(0));

    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", qCompress(raw, 6).mid(4));
    appendChunk(png, "IEND", {});
    return png;
}

QByteArray writeGlb(const Model3D &model, const std::vector<std::shared_ptr<const Image>> &textures,
                    const GltfOptions &options, const std::vector<ModelAnimation> &animations)
{
    BinaryBuilder b;
    QJsonArray nodes, meshes, materials, gltfTextures, images, samplers, skins, sceneNodes;
    std::map<QByteArray, int> skinCache;

    std::vector<QJsonArray> children(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode &n = model.nodes[i];
        nodes.append(QJsonObject{{"name", n.name},
                                 {"translation", QJsonArray{n.translation[0], n.translation[1], n.translation[2]}},
                                 {"rotation", QJsonArray{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]}},
                                 {"scale", QJsonArray{n.scale[0], n.scale[1], n.scale[2]}}});
        if (n.parent >= 0)
            children[size_t(n.parent)].append(int(i));
        else
            sceneNodes.append(int(i));
    }

    auto usable = [&](int texture) {
        return texture >= 0 && size_t(texture) < textures.size() && textures[size_t(texture)]
            && textures[size_t(texture)]->width > 0 && textures[size_t(texture)]->height > 0;
    };
    auto mirrored = [&](int texture, bool vertical) {
        if (!options.bakeMirroredRepeat || !usable(texture))
            return false;
        const ModelTexture &t = model.textures[size_t(texture)];
        return (vertical ? t.wrapV : t.wrapU) == 1;
    };
    std::vector<std::pair<bool, bool>> doubled;
    for (const ModelMaterial &m : model.materials)
        doubled.emplace_back(mirrored(m.baseColorTexture, false) || mirrored(m.normalTexture, false),
                             mirrored(m.baseColorTexture, true) || mirrored(m.normalTexture, true));

    std::map<std::tuple<int, bool, bool, bool>, int> textureCache;
    auto textureIndex = [&](int texture, bool normalMap, bool doubleU, bool doubleV) -> int {
        if (!usable(texture))
            return -1;
        const auto key = std::make_tuple(texture, normalMap, doubleU, doubleV);
        if (const auto it = textureCache.find(key); it != textureCache.end())
            return it->second;
        const ModelTexture &info = model.textures[size_t(texture)];
        Image image = normalMap ? completedNormalMap(*textures[size_t(texture)]) : *textures[size_t(texture)];
        if (doubleU || doubleV)
            image = doubledImage(image, doubleU, info.wrapU == 1, doubleV, info.wrapV == 1);
        const QByteArray png = encodePng(image);
        const int view = b.addView(png.constData(), png.size());
        images.append(QJsonObject{{"bufferView", view}, {"mimeType", "image/png"}, {"name", info.name}});
        samplers.append(QJsonObject{{"wrapS", doubleU ? wrapMode(0) : wrapMode(info.wrapU)},
                                    {"wrapT", doubleV ? wrapMode(0) : wrapMode(info.wrapV)}});
        gltfTextures.append(QJsonObject{{"source", int(images.size() - 1)}, {"sampler", int(samplers.size() - 1)}});
        return textureCache.emplace(key, int(gltfTextures.size() - 1)).first->second;
    };

    for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
        const ModelMaterial &m = model.materials[materialIndex];
        const auto [doubleU, doubleV] = doubled[materialIndex];
        QJsonObject pbr{{"metallicFactor", 0.0}, {"roughnessFactor", 1.0}};
        QJsonObject material{{"name", m.name}, {"doubleSided", m.doubleSided}};
        if (const int t = textureIndex(m.baseColorTexture, false, doubleU, doubleV); t >= 0) {
            pbr["baseColorTexture"] = QJsonObject{{"index", t}};
            if (m.alphaMode == AlphaMode::Mask) {
                material["alphaMode"] = "MASK";
                material["alphaCutoff"] = m.alphaCutoff;
            } else if (m.alphaMode == AlphaMode::Blend) {
                material["alphaMode"] = "BLEND";
            }
        }
        if (const int t = textureIndex(m.normalTexture, true, doubleU, doubleV); t >= 0)
            material["normalTexture"] = QJsonObject{{"index", t}};
        material["pbrMetallicRoughness"] = pbr;
        materials.append(material);
    }

    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const ModelMesh &mesh = model.meshes[meshIndex];
        std::vector<const ModelPrimitive *> exported;
        for (const ModelPrimitive &prim : model.primitives)
            if (prim.mesh == int(meshIndex) && !prim.indices.empty()
                && (options.includeAuxiliaryMeshes || !model.isAuxiliary(prim)))
                exported.push_back(&prim);
        if (exported.empty())
            continue;

        std::vector<int> skinJoints;
        std::vector<quint16> jointRemap;
        const bool hasWeights = std::any_of(exported.begin(), exported.end(),
                                            [](const ModelPrimitive *p) { return !p->joints.empty(); });
        if (hasWeights && !model.nodes.empty()) {
            for (int joint : mesh.joints) {
                const int node = joint >= 0 && size_t(joint) < model.nodes.size() ? joint : 0;
                const auto found = std::find(skinJoints.begin(), skinJoints.end(), node);
                jointRemap.push_back(quint16(found - skinJoints.begin()));
                if (found == skinJoints.end())
                    skinJoints.push_back(node);
            }
        }
        const bool skinned = !skinJoints.empty();

        QJsonArray primitives;
        for (const ModelPrimitive *primPtr : exported) {
            const ModelPrimitive &prim = *primPtr;
            QJsonObject attributes;
            attributes["POSITION"] = b.addAccessor(prim.positions, 3, kFloat, "VEC3", false, kArrayBuffer, true);
            if (!prim.normals.empty()) {
                std::vector<float> normals = prim.normals;
                for (size_t v = 0; v + 2 < normals.size(); v += 3) {
                    const float len = std::sqrt(normals[v] * normals[v] + normals[v + 1] * normals[v + 1]
                                                + normals[v + 2] * normals[v + 2]);
                    if (len > 1e-6f) {
                        normals[v] /= len;
                        normals[v + 1] /= len;
                        normals[v + 2] /= len;
                    } else {
                        normals[v] = 0;
                        normals[v + 1] = 1;
                        normals[v + 2] = 0;
                    }
                }
                attributes["NORMAL"] = b.addAccessor(normals, 3, kFloat, "VEC3");
            }
            auto uv = [&](const std::vector<float> &source, bool halveU, bool halveV) {
                std::vector<float> out = source;
                for (size_t v = 0; v + 1 < out.size(); v += 2) {
                    if (options.flipV)
                        out[v + 1] = 1.f - out[v + 1];
                    if (halveU)
                        out[v] *= 0.5f;
                    if (halveV)
                        out[v + 1] *= 0.5f;
                }
                return b.addAccessor(out, 2, kFloat, "VEC2");
            };
            const bool hasMaterial = prim.material >= 0 && size_t(prim.material) < doubled.size();
            const auto [halveU, halveV] = hasMaterial ? doubled[size_t(prim.material)] : std::pair{false, false};
            if (!prim.uv0.empty())
                attributes["TEXCOORD_0"] = uv(prim.uv0, halveU, halveV);
            if (!prim.uv1.empty())
                attributes["TEXCOORD_1"] = uv(prim.uv1, false, false);
            if (!prim.colors.empty())
                attributes[prim.colorsAreData ? "_COLOR_0" : "COLOR_0"] =
                    b.addAccessor(prim.colors, 4, kUnsignedByte, "VEC4", true);
            if (skinned) {
                const size_t slotCount = size_t(prim.vertexCount()) * 4;
                std::vector<quint16> joints(slotCount, 0);
                std::vector<float> weights(slotCount, 0.f);
                const bool own = prim.joints.size() == slotCount && prim.weights.size() == slotCount;
                for (size_t v = 0; v < slotCount; v += 4) {
                    if (!own) {
                        weights[v] = 1.f;
                        continue;
                    }
                    for (size_t k = 0; k < 4; ++k) {
                        const quint16 source = prim.joints[v + k];
                        const quint16 target = source < jointRemap.size() ? jointRemap[source] : 0;
                        const float w = prim.weights[v + k];
                        size_t slot = v + k;
                        for (size_t e = v; e < v + k; ++e)
                            if (weights[e] > 0 && joints[e] == target) {
                                slot = e;
                                break;
                            }
                        joints[slot] = target;
                        weights[slot] += w;
                    }
                    for (size_t k = 0; k < 4; ++k)
                        if (!(weights[v + k] > 0)) {
                            joints[v + k] = 0;
                            weights[v + k] = 0;
                        }
                }
                attributes["JOINTS_0"] = b.addAccessor(joints, 4, kUnsignedShort, "VEC4");
                attributes["WEIGHTS_0"] = b.addAccessor(weights, 4, kFloat, "VEC4");
            }
            const bool shortIndices = *std::max_element(prim.indices.begin(), prim.indices.end()) < 0xFFFF;
            const int indices = shortIndices
                                  ? b.addAccessor(std::vector<quint16>(prim.indices.begin(), prim.indices.end()), 1,
                                                  kUnsignedShort, "SCALAR", false, kElementArrayBuffer)
                                  : b.addAccessor(prim.indices, 1, kUnsignedInt, "SCALAR", false, kElementArrayBuffer);
            QJsonObject primitive{{"attributes", attributes}, {"indices", indices}, {"mode", 4}};
            if (prim.material >= 0)
                primitive["material"] = prim.material;
            primitives.append(primitive);
        }
        meshes.append(QJsonObject{{"name", mesh.name}, {"primitives", primitives}});
        QJsonObject node{{"name", mesh.name}, {"mesh", int(meshes.size() - 1)}};
        const int nodeIndex = int(nodes.size());
        if (skinned) {
            std::vector<float> inverseBind;
            QJsonArray joints;
            for (int joint : skinJoints) {
                Matrix4 ibm;
                const auto given = std::find(mesh.joints.begin(), mesh.joints.end(), joint);
                if (mesh.inverseBinds.size() == mesh.joints.size() && given != mesh.joints.end())
                    ibm = mesh.inverseBinds[size_t(given - mesh.joints.begin())];
                else
                    ibm = inverted(model.worldMatrix(joint));
                inverseBind.insert(inverseBind.end(), ibm.begin(), ibm.end());
                joints.append(joint);
            }
            QByteArray key(reinterpret_cast<const char *>(skinJoints.data()), qsizetype(skinJoints.size() * sizeof(int)));
            key.append(reinterpret_cast<const char *>(inverseBind.data()), qsizetype(inverseBind.size() * sizeof(float)));
            auto skin = skinCache.find(key);
            if (skin == skinCache.end()) {
                skins.append(QJsonObject{{"name", mesh.name},
                                         {"joints", joints},
                                         {"inverseBindMatrices", b.addAccessor(inverseBind, 16, kFloat, "MAT4", false, 0)}});
                skin = skinCache.emplace(key, int(skins.size() - 1)).first;
            }
            node["skin"] = skin->second;
            sceneNodes.append(nodeIndex);
        } else if (model.staticMeshesUseNodeTransform && mesh.node >= 0) {
            children[size_t(mesh.node)].append(nodeIndex);
        } else {
            sceneNodes.append(nodeIndex);
        }
        nodes.append(node);
    }
    for (size_t i = 0; i < children.size(); ++i)
        if (!children[i].isEmpty()) {
            QJsonObject n = nodes[qsizetype(i)].toObject();
            n["children"] = children[i];
            nodes[qsizetype(i)] = n;
        }

    QJsonArray gltfAnimations;
    std::vector<const ModelAnimation *> allAnimations;
    for (const ModelAnimation &animation : model.animations)
        allAnimations.push_back(&animation);
    for (const ModelAnimation &animation : animations)
        allAnimations.push_back(&animation);
    std::map<QByteArray, int> timeCache;
    auto timeAccessor = [&](const std::vector<float> &times) {
        const QByteArray key(reinterpret_cast<const char *>(times.data()), qsizetype(times.size() * sizeof(float)));
        auto it = timeCache.find(key);
        if (it == timeCache.end())
            it = timeCache.emplace(key, b.addAccessor(times, 1, kFloat, "SCALAR", false, 0, true)).first;
        return it->second;
    };
    auto componentsOf = [](const AnimationChannel &c) { return c.path == AnimationPath::Rotation ? size_t(4) : size_t(3); };
    auto constant = [&](const AnimationChannel &c) {
        const size_t components = componentsOf(c);
        for (size_t i = components; i < c.values.size(); ++i)
            if (std::abs(c.values[i] - c.values[i % components]) > 1e-6f)
                return false;
        return true;
    };
    for (const ModelAnimation *each : allAnimations) {
        const ModelAnimation &animation = *each;
        QJsonArray animationSamplers, channels;
        std::map<std::pair<int, int>, const AnimationChannel *> chosen;
        std::vector<std::pair<int, int>> order;
        for (const AnimationChannel &channel : animation.channels) {
            if (channel.node < 0 || size_t(channel.node) >= model.nodes.size() || channel.times.empty()
                || channel.values.size() != channel.times.size() * componentsOf(channel))
                continue;
            const std::pair<int, int> target{channel.node, int(channel.path)};
            const auto [it, added] = chosen.emplace(target, &channel);
            if (added)
                order.push_back(target);
            else if (constant(*it->second) && !constant(channel))
                it->second = &channel;
        }
        for (const auto &target : order) {
            const AnimationChannel &channel = *chosen[target];
            const size_t components = componentsOf(channel);
            int input, output;
            if (channel.times.size() > 2 && constant(channel)) {
                input = timeAccessor({channel.times.front(), channel.times.back()});
                std::vector<float> values(channel.values.begin(), channel.values.begin() + qsizetype(components));
                values.insert(values.end(), channel.values.begin(), channel.values.begin() + qsizetype(components));
                output = b.addAccessor(values, int(components), kFloat, components == 4 ? "VEC4" : "VEC3", false, 0);
            } else {
                input = timeAccessor(channel.times);
                output = b.addAccessor(channel.values, int(components), kFloat, components == 4 ? "VEC4" : "VEC3",
                                       false, 0);
            }
            animationSamplers.append(QJsonObject{{"input", input}, {"output", output}, {"interpolation", "LINEAR"}});
            const char *path = channel.path == AnimationPath::Rotation ? "rotation"
                             : channel.path == AnimationPath::Scale    ? "scale"
                                                                       : "translation";
            channels.append(QJsonObject{{"sampler", int(animationSamplers.size() - 1)},
                                        {"target", QJsonObject{{"node", channel.node}, {"path", path}}}});
        }
        if (!channels.isEmpty())
            gltfAnimations.append(
                QJsonObject{{"name", animation.name}, {"samplers", animationSamplers}, {"channels", channels}});
    }

    while (b.bin.size() % 4)
        b.bin.append('\0');
    QJsonObject root{{"asset", QJsonObject{{"version", "2.0"}, {"generator", "NX Game Viewer"}}},
                     {"scene", 0},
                     {"scenes", QJsonArray{QJsonObject{{"nodes", sceneNodes}}}},
                     {"nodes", nodes},
                     {"buffers", QJsonArray{QJsonObject{{"byteLength", b.bin.size()}}}},
                     {"bufferViews", b.bufferViews},
                     {"accessors", b.accessors}};
    auto put = [&](const char *key, const QJsonArray &a) {
        if (!a.isEmpty())
            root[key] = a;
    };
    put("meshes", meshes);
    put("materials", materials);
    put("textures", gltfTextures);
    put("images", images);
    put("samplers", samplers);
    put("skins", skins);
    put("animations", gltfAnimations);

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() % 4)
        json.append(' ');
    QByteArray glb;
    auto u32 = [&glb](quint32 v) {
        char raw[4];
        qToLittleEndian(v, raw);
        glb.append(raw, 4);
    };
    u32(0x46546C67);
    u32(2);
    u32(quint32(12 + 8 + json.size() + 8 + b.bin.size()));
    u32(quint32(json.size()));
    u32(0x4E4F534A);
    glb += json;
    u32(quint32(b.bin.size()));
    u32(0x004E4942);
    glb += b.bin;
    return glb;
}
