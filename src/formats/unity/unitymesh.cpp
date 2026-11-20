#include "unity/unitymesh.h"

#include <QObject>
#include <QtEndian>
#include <qfloat16.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>

namespace {

enum class Format { Float32, Float16, UNorm8, SNorm8, UNorm16, SNorm16, UInt8, SInt8, UInt16, SInt16, UInt32, SInt32 };

enum Channel { Position = 0, Normal = 1, Color = 3, TexCoord0 = 4, TexCoord1 = 5, BlendWeight = 12, BlendIndices = 13,
               ChannelCount = 14 };
constexpr int kInfluences = 4;

int formatSize(Format f)
{
    switch (f) {
    case Format::Float32:
    case Format::UInt32:
    case Format::SInt32:
        return 4;
    case Format::Float16:
    case Format::UNorm16:
    case Format::SNorm16:
    case Format::UInt16:
    case Format::SInt16:
        return 2;
    default:
        return 1;
    }
}

std::optional<Format> formatOf(int raw, int major)
{
    if (major < 2019) {
        if (raw == 0)
            return Format::Float32;
        if (raw == 1)
            return Format::Float16;
        if (raw == 2)
            return Format::UNorm8;
        raw -= 1;
    }
    if (raw < 0 || raw > int(Format::SInt32))
        return std::nullopt;
    return Format(raw);
}

float halfToFloat(quint16 h)
{
    return float(std::bit_cast<qfloat16>(h));
}

struct Reader
{
    const uchar *data;
    bool big;

    template <typename T> T get(qsizetype at) const
    {
        return big ? qFromBigEndian<T>(data + at) : qFromLittleEndian<T>(data + at);
    }

    float component(Format f, qsizetype at) const
    {
        switch (f) {
        case Format::Float32: {
            const quint32 bits = get<quint32>(at);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        }
        case Format::Float16:
            return halfToFloat(get<quint16>(at));
        case Format::UNorm8:
            return float(data[at]) / 255.f;
        case Format::SNorm8:
            return std::max(float(qint8(data[at])) / 127.f, -1.f);
        case Format::UNorm16:
            return float(get<quint16>(at)) / 65535.f;
        case Format::SNorm16:
            return std::max(float(get<qint16>(at)) / 32767.f, -1.f);
        case Format::UInt8:
            return float(data[at]);
        case Format::SInt8:
            return float(qint8(data[at]));
        case Format::UInt16:
            return float(get<quint16>(at));
        case Format::SInt16:
            return float(get<qint16>(at));
        case Format::UInt32:
            return float(get<quint32>(at));
        case Format::SInt32:
            return float(get<qint32>(at));
        }
        return 0;
    }
};

struct ChannelInfo
{
    int stream = 0;
    int offset = 0;
    Format format = Format::Float32;
    int dimension = 0;
};

int majorVersion(const QString &version)
{
    bool ok = false;
    const int major = version.section(u'.', 0, 0).toInt(&ok);
    return ok ? major : 0;
}

}

std::shared_ptr<Model3D> unityMeshModel(const UValue &mesh, const QByteArray &streamed, const QString &unityVersion,
                                        bool bigEndian, QString *error, std::vector<int> *submeshOfPrimitive)
{
    auto fail = [error](const QString &message) -> std::shared_ptr<Model3D> {
        if (error)
            *error = message;
        return nullptr;
    };
    const UValue *vertexData = mesh.child(u"m_VertexData");
    const UValue *channelList = vertexData ? vertexData->child(u"m_Channels") : nullptr;
    const UValue *countValue = vertexData ? vertexData->child(u"m_VertexCount") : nullptr;
    if (!channelList || !countValue)
        return fail(QObject::tr("This mesh has no vertex data the app can read."));
    const UValue *compressed = mesh.at(u"m_CompressedMesh.m_Vertices.m_NumItems");
    const qint64 vertexCount = countValue->toInt();
    if (vertexCount <= 0)
        return fail(compressed && compressed->toInt() > 0
                        ? QObject::tr("This mesh uses Unity's mesh compression, which is not supported yet.")
                        : QObject::tr("This mesh has no vertices."));
    if (channelList->children.size() != ChannelCount)
        return fail(QObject::tr("This Unity version's mesh layout (%n channel(s)) is not supported yet.", nullptr,
                                int(channelList->children.size())));

    const int major = majorVersion(unityVersion);
    std::array<ChannelInfo, ChannelCount> channels{};
    std::array<qint64, 8> strides{};
    int streams = 0;
    for (int c = 0; c < ChannelCount; ++c) {
        const UValue &v = channelList->children[size_t(c)];
        const UValue *stream = v.child(u"stream"), *offset = v.child(u"offset"), *format = v.child(u"format"),
                     *dimension = v.child(u"dimension");
        if (!stream || !offset || !format || !dimension)
            return fail(QObject::tr("A vertex channel of this mesh is damaged."));
        ChannelInfo &info = channels[size_t(c)];
        info.dimension = int(dimension->toInt() & 0xF);
        if (info.dimension == 0)
            continue;
        const std::optional<Format> f = formatOf(int(format->toInt()), major);
        info.stream = int(stream->toInt());
        info.offset = int(offset->toInt());
        if (!f || info.stream < 0 || info.stream >= int(strides.size()) || info.offset < 0)
            return fail(QObject::tr("A vertex channel of this mesh is damaged."));
        info.format = *f;
        strides[size_t(info.stream)] = std::max<qint64>(strides[size_t(info.stream)],
                                                       info.offset + qint64(formatSize(info.format)) * info.dimension);
        streams = std::max(streams, info.stream + 1);
    }
    const UValue *inlineData = vertexData->child(u"m_DataSize");
    const QByteArray &bytes = inlineData && !inlineData->bytes.isEmpty() ? inlineData->bytes : streamed;
    if (vertexCount > bytes.size())
        return fail(bytes.isEmpty() ? QObject::tr("The mesh's vertex data is missing (it may be in a file that was not found).")
                                    : QObject::tr("The mesh's vertex data is shorter than its layout."));
    std::array<qint64, 8> streamStart{};
    qint64 total = 0;
    for (int s = 0; s < streams; ++s) {
        if (strides[size_t(s)] > 4096)
            return fail(QObject::tr("A vertex channel of this mesh is damaged."));
        streamStart[size_t(s)] = total;
        total += strides[size_t(s)] * vertexCount;
        total = (total + 15) / 16 * 16;
    }
    if (strides[0] <= 0 || channels[Position].dimension < 3)
        return fail(QObject::tr("This mesh has no positions."));
    for (int s = 0; s < streams; ++s)
        if (streamStart[size_t(s)] + strides[size_t(s)] * vertexCount > bytes.size())
            return fail(bytes.isEmpty() ? QObject::tr("The mesh's vertex data is missing (it may be in a file that was not found).")
                                        : QObject::tr("The mesh's vertex data is shorter than its layout."));

    const Reader reader{reinterpret_cast<const uchar *>(bytes.constData()), bigEndian};
    auto read = [&](int channel, std::vector<float> &out, int components) {
        const ChannelInfo &info = channels[size_t(channel)];
        if (info.dimension == 0)
            return;
        out.resize(size_t(vertexCount) * size_t(components));
        const int size = formatSize(info.format);
        for (qint64 v = 0; v < vertexCount; ++v) {
            const qsizetype at = streamStart[size_t(info.stream)] + strides[size_t(info.stream)] * v + info.offset;
            for (int k = 0; k < components; ++k)
                out[size_t(v * components + k)] = k < info.dimension ? reader.component(info.format, at + k * size) : 0.f;
        }
    };
    std::vector<float> positions, normals, colors, uv0, uv1;
    read(Position, positions, 3);
    read(Normal, normals, 3);
    read(Color, colors, 4);
    read(TexCoord0, uv0, 2);
    read(TexCoord1, uv1, 2);
    for (size_t i = 0; i < positions.size(); i += 3)
        positions[i] = -positions[i];
    for (size_t i = 0; i < normals.size(); i += 3)
        normals[i] = -normals[i];

    std::vector<float> weights, boneIndices;
    if (channels[BlendIndices].dimension > 0) {
        std::vector<float> rawWeights;
        read(BlendWeight, rawWeights, kInfluences);
        read(BlendIndices, boneIndices, kInfluences);
        const int weightCount = channels[BlendWeight].dimension;
        weights.assign(size_t(vertexCount) * kInfluences, 0.f);
        for (qint64 v = 0; v < vertexCount; ++v) {
            float sum = 0;
            for (int k = 0; k < kInfluences; ++k) {
                const float w = rawWeights.empty() ? (k == 0 ? 1.f : 0.f)
                              : k < weightCount   ? rawWeights[size_t(v * kInfluences + k)]
                              : k == weightCount  ? std::max(0.f, 1.f - sum)
                                                  : 0.f;
                weights[size_t(v * kInfluences + k)] = w;
                sum += w;
            }
        }
    } else if (const UValue *skin = mesh.child(u"m_Skin"); skin && qint64(skin->children.size()) == vertexCount) {
        weights.resize(size_t(vertexCount) * kInfluences);
        boneIndices.resize(size_t(vertexCount) * kInfluences);
        for (qint64 v = 0; v < vertexCount; ++v) {
            const UValue &influence = skin->children[size_t(v)];
            for (int k = 0; k < kInfluences; ++k) {
                const UValue *w = influence.child(QStringLiteral("weight[%1]").arg(k));
                const UValue *b = influence.child(QStringLiteral("boneIndex[%1]").arg(k));
                weights[size_t(v * kInfluences + k)] = w ? float(w->toDouble()) : 0.f;
                boneIndices[size_t(v * kInfluences + k)] = b ? float(b->toInt()) : 0.f;
            }
        }
    }

    const UValue *indexFormat = mesh.child(u"m_IndexFormat");
    const UValue *indexBuffer = mesh.child(u"m_IndexBuffer");
    const UValue *subMeshes = mesh.child(u"m_SubMeshes");
    if (!indexBuffer || !subMeshes)
        return fail(QObject::tr("This mesh has no index data."));
    const int indexSize = indexFormat && indexFormat->toInt() == 1 ? 4 : 2;
    const QByteArray &indexBytes = indexBuffer->bytes;
    const Reader indexReader{reinterpret_cast<const uchar *>(indexBytes.constData()), bigEndian};

    auto model = std::make_shared<Model3D>();
    const QString name = mesh.child(u"m_Name") ? mesh.child(u"m_Name")->toString() : QString();
    model->nodes.push_back({name.isEmpty() ? QStringLiteral("mesh") : name});
    model->meshes.push_back({name.isEmpty() ? QStringLiteral("mesh") : name, 0});
    int skipped = 0;
    for (size_t s = 0; s < subMeshes->children.size(); ++s) {
        const UValue &sub = subMeshes->children[s];
        const UValue *firstByte = sub.child(u"firstByte"), *indexCount = sub.child(u"indexCount"),
                     *topology = sub.child(u"topology"), *baseVertex = sub.child(u"baseVertex"),
                     *firstVertex = sub.child(u"firstVertex"), *subVertexCount = sub.child(u"vertexCount");
        if (!firstByte || !indexCount)
            continue;
        const qint64 kind = topology ? topology->toInt() : 0;
        if (kind != 0 && kind != 2) {
            ++skipped;
            continue;
        }
        const qint64 start = firstByte->toInt(), count = indexCount->toInt();
        if (start < 0 || count <= 0 || start > indexBytes.size() || count > (indexBytes.size() - start) / indexSize)
            continue;
        const qint64 base = baseVertex ? baseVertex->toInt() : 0;
        qint64 first = firstVertex ? firstVertex->toInt() : 0;
        qint64 used = subVertexCount ? subVertexCount->toInt() : vertexCount - first;
        if (first < 0 || used <= 0 || first > vertexCount - used) {
            first = 0;
            used = vertexCount;
        }
        std::vector<quint32> raw;
        raw.reserve(size_t(count));
        bool valid = true;
        for (qint64 i = 0; i < count && valid; ++i) {
            const qint64 index = (indexSize == 4 ? qint64(indexReader.get<quint32>(start + i * 4))
                                                 : qint64(indexReader.get<quint16>(start + i * 2)))
                                 + base - first;
            valid = index >= 0 && index < used;
            raw.push_back(quint32(index));
        }
        if (!valid) {
            ++skipped;
            continue;
        }
        ModelPrimitive prim;
        prim.mesh = 0;
        prim.material = int(model->materials.size());
        model->materials.push_back({QObject::tr("Submesh %1").arg(s)});
        auto slice = [first, used](const std::vector<float> &all, int components) {
            if (all.empty())
                return std::vector<float>();
            return std::vector<float>(all.begin() + first * components, all.begin() + (first + used) * components);
        };
        prim.positions = slice(positions, 3);
        prim.normals = slice(normals, 3);
        prim.uv0 = slice(uv0, 2);
        prim.uv1 = slice(uv1, 2);
        if (!boneIndices.empty()) {
            prim.weights = slice(weights, kInfluences);
            const std::vector<float> b = slice(boneIndices, kInfluences);
            prim.joints.reserve(b.size());
            for (float f : b)
                prim.joints.push_back(quint16(std::clamp(std::lround(f), 0L, 65535L)));
        }
        if (!colors.empty()) {
            const std::vector<float> c = slice(colors, 4);
            prim.colors.reserve(c.size());
            for (float f : c)
                prim.colors.push_back(quint8(std::clamp(std::lround(f * 255.f), 0L, 255L)));
        }
        if (kind == 2) {
            for (size_t q = 0; q + 3 < raw.size(); q += 4) {
                const quint32 a = raw[q], b = raw[q + 1], c = raw[q + 2], d = raw[q + 3];
                prim.indices.insert(prim.indices.end(), {a, c, b, a, d, c});
            }
        } else {
            for (size_t t = 0; t + 2 < raw.size(); t += 3)
                prim.indices.insert(prim.indices.end(), {raw[t], raw[t + 2], raw[t + 1]});
        }
        if (!prim.indices.empty()) {
            model->primitives.push_back(std::move(prim));
            if (submeshOfPrimitive)
                submeshOfPrimitive->push_back(int(s));
        }
    }
    if (model->primitives.empty())
        return fail(skipped > 0 ? QObject::tr("This mesh only has lines or points, which cannot be shown.")
                                : QObject::tr("This mesh has no triangles."));
    return model;
}
