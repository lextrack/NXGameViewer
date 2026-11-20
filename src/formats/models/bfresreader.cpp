#include "models/bfresreader.h"

#include "textures/bntxreader.h"
#include "archives/decompress.h"
#include "archives/zstddictionaries.h"
#include "models/meshpreview.h"
#include "core/simpletable.h"
#include "core/contentkind.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFloat16>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>

namespace {

constexpr int kMaxCount = 1 << 20;
constexpr int kMaxElements = 1 << 25;
constexpr float kFramesPerSecond = 30.f;

struct BadOffset : std::runtime_error
{
    BadOffset() : std::runtime_error("offset out of range") {}
};

QString externalString(quint64 id);

class View
{
public:
    explicit View(const QByteArray &data) : m_data(data) {}

    void need(qint64 at, qint64 size) const
    {
        if (at < 0 || size < 0 || at > m_data.size() - size)
            throw BadOffset();
    }
    const char *raw(qint64 at, qint64 size) const
    {
        need(at, size);
        return m_data.constData() + at;
    }
    quint8 u8(qint64 at) const { return quint8(*raw(at, 1)); }
    qint8 s8(qint64 at) const { return qint8(*raw(at, 1)); }
    quint16 u16(qint64 at) const { return qFromLittleEndian<quint16>(raw(at, 2)); }
    qint16 s16(qint64 at) const { return qFromLittleEndian<qint16>(raw(at, 2)); }
    quint32 u32(qint64 at) const { return qFromLittleEndian<quint32>(raw(at, 4)); }
    qint32 s32(qint64 at) const { return qFromLittleEndian<qint32>(raw(at, 4)); }
    float f32(qint64 at) const
    {
        const quint32 bits = u32(at);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    qint64 ptr(qint64 at) const
    {
        const quint64 v = qFromLittleEndian<quint64>(raw(at, 8));
        if (v > quint64(m_data.size()))
            throw BadOffset();
        return qint64(v);
    }
    int elements(quint32 n) const
    {
        if (n > quint32(kMaxElements))
            throw BadOffset();
        return int(n);
    }
    int count(quint32 n) const
    {
        if (n > quint32(kMaxCount))
            throw BadOffset();
        return int(n);
    }
    QString str(qint64 at) const
    {
        if (at == 0)
            return {};
        const quint16 n = u16(at);
        return QString::fromUtf8(raw(at + 2, n), n);
    }
    QString strAt(qint64 at) const { return str(ptr(at)); }
    QString nameAt(qint64 at) const
    {
        const quint64 value = qFromLittleEndian<quint64>(raw(at, 8));
        return value <= quint64(m_data.size()) ? str(qint64(value)) : externalString(value);
    }
    QStringList dict(qint64 at) const
    {
        QStringList names;
        if (at == 0)
            return names;
        const int n = count(u32(at + 4));
        for (int i = 0; i < n; ++i)
            names << strAt(at + 0x18 + qint64(i) * 0x10 + 8);
        return names;
    }

private:
    const QByteArray &m_data;
};

QMutex externalMutex;
QHash<quint64, QString> externalStrings;
QSet<QString> externalSources;

void addExternalStrings(const QByteArray &file)
{
    if (file.size() < 0xC8 || !file.startsWith("FRES") || !(uchar(file[0xEE]) & 4))
        return;
    auto u64 = [&file](qint64 at) { return at + 8 <= file.size() ? qFromLittleEndian<quint64>(file.constData() + at) : 0; };
    auto u32 = [&file](qint64 at) { return at + 4 <= file.size() ? qFromLittleEndian<quint32>(file.constData() + at) : 0; };
    auto str = [&file](quint64 at) -> QString {
        if (at == 0 || at + 2 > quint64(file.size()))
            return {};
        const quint16 n = qFromLittleEndian<quint16>(file.constData() + at);
        return at + 2 + n <= quint64(file.size()) ? QString::fromUtf8(file.constData() + at + 2, n) : QString();
    };
    const quint64 ids = u64(0xB8), dict = u64(0xC0);
    if (!ids || !dict || dict + 8 > quint64(file.size()))
        return;
    const quint32 n = u32(qint64(dict) + 4);
    const QMutexLocker lock(&externalMutex);
    for (quint32 i = 0; i < n; ++i) {
        const QString name = str(u64(qint64(dict) + 0x18 + qint64(i) * 0x10 + 8));
        const qint64 idAt = qint64(ids) + qint64(i) * 8;
        if (!name.isEmpty() && idAt + 8 <= file.size())
            externalStrings.insert(u64(idAt), name);
    }
}

void loadExternalStrings()
{
    for (const QString &root : zstdDictionaryRoots()) {
        {
            const QMutexLocker lock(&externalMutex);
            if (externalSources.contains(root))
                continue;
            externalSources.insert(root);
        }
        for (const QString &name : {QStringLiteral("Shader/ExternalBinaryString.bfres.zs"),
                                    QStringLiteral("Shader/ExternalBinaryString.bfres")}) {
            QFile f(QDir(root).filePath(name));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            if (const std::optional<QByteArray> bytes = decompress(f.readAll(), nullptr))
                addExternalStrings(*bytes);
            break;
        }
    }
}

QString externalString(quint64 id)
{
    const QMutexLocker lock(&externalMutex);
    const auto it = externalStrings.constFind(id);
    return it == externalStrings.cend() ? QStringLiteral("#%1").arg(id, 16, 16, QLatin1Char('0')) : *it;
}

struct Layout
{
    bool v9;
    bool v10 = false;
    template <typename T>
    T pick(T newer, T older) const { return v9 ? newer : older; }
};

struct Header
{
    Layout layout{true};
    quint32 version = 0;
    QString name;
    qint64 modelArray = 0;
    QStringList models;
    qint64 clipArray = 0;
    QStringList clips;
    qint64 poolData = 0;
    qint64 materialAnimArray = 0;
    QStringList materialAnims;
    QStringList boneVisibilityAnims;
    QStringList shapeAnims;
    QStringList sceneAnims;
    qint64 externalArray = 0;
    QStringList externalFiles;
};

QString versionText(quint32 version)
{
    return QStringLiteral("%1.%2.%3").arg(version >> 16).arg((version >> 8) & 0xFF).arg(version & 0xFF);
}

Header readHeader(const View &v, const QByteArray &data)
{
    if (!BfresReader::isBfres(data))
        throw std::runtime_error(QObject::tr("Not a Switch BFRES file (no \"FRES    \" at the start).").toStdString());
    if (uchar(data[0x0C]) != 0xFF || uchar(data[0x0D]) != 0xFE)
        throw std::runtime_error(QObject::tr("Big-endian (Wii U) BFRES files are not supported yet.").toStdString());
    Header h;
    h.version = v.u32(0x08);
    const int major = int(h.version >> 16);
    if (major < 5 || major > 10)
        throw std::runtime_error(
            QObject::tr("BFRES version %1 is not supported yet.").arg(versionText(h.version)).toStdString());
    h.layout.v9 = major >= 9;
    h.layout.v10 = major >= 10;
    if (h.layout.v10 && data.size() > 0xEE && (uchar(data[0xEE]) & 2))
        loadExternalStrings();
    const Layout &l = h.layout;
    h.name = v.str(v.u32(0x20));
    h.modelArray = v.ptr(0x28);
    h.models = v.dict(v.ptr(0x30));
    h.clipArray = v.ptr(l.pick(0x58, 0x38));
    h.clips = v.dict(v.ptr(l.pick(0x60, 0x40)));
    const qint64 poolInfo = v.ptr(l.pick(0xB0, 0x90));
    if (poolInfo)
        h.poolData = v.ptr(poolInfo + 8);
    h.materialAnimArray = v.ptr(l.pick(0x68, 0x48));
    h.materialAnims = v.dict(v.ptr(l.pick(0x70, 0x50)));
    h.boneVisibilityAnims = v.dict(v.ptr(l.pick(0x80, 0x60)));
    h.shapeAnims = v.dict(v.ptr(l.pick(0x90, 0x70)));
    h.sceneAnims = v.dict(v.ptr(l.pick(0xA0, 0x80)));
    const bool stringTable = h.layout.v10 && data.size() > 0xEE && (uchar(data[0xEE]) & 4);
    if (!stringTable) {
        h.externalArray = v.ptr(l.pick(0xB8, 0x98));
        h.externalFiles = v.dict(v.ptr(l.pick(0xC0, 0xA0)));
    }
    return h;
}

struct Bone
{
    QString name;
    int parent = -1;
    quint32 flags = 0;
    std::array<float, 3> scale{};
    std::array<float, 4> rotation{};
    std::array<float, 3> translation{};
};

struct Skeleton
{
    std::vector<Bone> bones;
    std::vector<int> palette;
};

std::array<float, 4> boneRotation(const Bone &b)
{
    if (((b.flags >> 12) & 7) == 1)
        return quaternionFromEulerZYX(b.rotation[0], b.rotation[1], b.rotation[2]);
    return normalizedQuaternion(b.rotation);
}

Skeleton readSkeleton(const View &v, const Layout &l, qint64 at)
{
    Skeleton s;
    const qint64 boneArray = v.ptr(at + l.pick(0x10, 0x18));
    const qint64 paletteArray = v.ptr(at + l.pick(0x18, 0x20));
    const int boneCount = v.count(v.u16(at + l.pick(0x38, 0x4C)));
    const int smooth = v.count(v.u16(at + l.pick(0x3A, 0x4E)));
    const int rigid = v.count(v.u16(at + l.pick(0x3C, 0x50)));
    for (int i = 0; i < boneCount; ++i) {
        const qint64 shift = l.v10 ? 8 : 0;
        const qint64 b = boneArray + qint64(i) * (0x60 - shift);
        Bone bone;
        bone.name = v.strAt(b);
        bone.parent = v.s16(b + 0x2A - shift);
        bone.flags = v.u32(b + 0x34 - shift);
        for (int k = 0; k < 3; ++k)
            bone.scale[size_t(k)] = v.f32(b + 0x38 - shift + k * 4);
        for (int k = 0; k < 4; ++k)
            bone.rotation[size_t(k)] = v.f32(b + 0x44 - shift + k * 4);
        for (int k = 0; k < 3; ++k)
            bone.translation[size_t(k)] = v.f32(b + 0x54 - shift + k * 4);
        if (bone.parent >= boneCount || bone.parent == i)
            bone.parent = -1;
        s.bones.push_back(std::move(bone));
    }
    for (int i = 0; i < smooth + rigid; ++i) {
        const int bone = v.u16(paletteArray + qint64(i) * 2);
        s.palette.push_back(bone < boneCount ? bone : 0);
    }
    return s;
}

struct VertexBuffer
{
    qint64 start = 0;
    qint64 size = 0;
    int stride = 0;
};

struct Attribute
{
    QString name;
    quint32 format = 0;
    int offset = 0;
    int buffer = 0;
};

struct Vertices
{
    std::vector<Attribute> attributes;
    std::vector<VertexBuffer> buffers;
    int count = 0;
};

Vertices readVertices(const View &v, const Layout &l, const Header &h, qint64 at)
{
    Vertices out;
    const qint64 attributeArray = v.ptr(at + l.pick(0x08, 0x10));
    const qint64 bufferInfo = v.ptr(at + l.pick(0x30, 0x38));
    const qint64 bufferState = v.ptr(at + l.pick(0x38, 0x40));
    qint64 pool = v.u32(at + l.pick(0x48, 0x50));
    const int attributes = v.u8(at + l.pick(0x4C, 0x54));
    const int buffers = v.u8(at + l.pick(0x4D, 0x55));
    out.count = v.elements(v.u32(at + l.pick(0x50, 0x58)));
    const qint64 alignment = l.v10 ? std::max<qint64>(1, v.u16(at + 0x56)) : 1;
    for (int i = 0; i < attributes; ++i) {
        const qint64 a = attributeArray + qint64(i) * 0x10;
        out.attributes.push_back({v.strAt(a), v.u32(a + 8), v.u16(a + 0x0C), v.u8(a + 0x0E)});
    }
    for (int i = 0; i < buffers; ++i) {
        VertexBuffer b;
        b.size = v.u32(bufferInfo + qint64(i) * 0x10);
        b.stride = int(v.u32(bufferState + qint64(i) * 0x10));
        b.start = h.poolData + pool;
        if (l.v10)
            b.start = (b.start + alignment - 1) / alignment * alignment;
        out.buffers.push_back(b);
        pool = l.v10 ? b.start - h.poolData + b.size : (pool + b.size + 7) & ~qint64(7);
    }
    return out;
}

bool readAttribute(const View &v, const Vertices &vertices, const Attribute &a, std::vector<float> &out)
{
    if (a.buffer < 0 || size_t(a.buffer) >= vertices.buffers.size())
        return false;
    const VertexBuffer &buffer = vertices.buffers[size_t(a.buffer)];
    const int channel = int(a.format >> 8);
    const int type = int(a.format & 0xFF);
    int components = 0, bytes = 0;
    switch (channel) {
    case 0x02: components = 1; bytes = 1; break;
    case 0x09: components = 2; bytes = 1; break;
    case 0x0B: components = 4; bytes = 1; break;
    case 0x0A: components = 1; bytes = 2; break;
    case 0x12: components = 2; bytes = 2; break;
    case 0x15: components = 4; bytes = 2; break;
    case 0x14: components = 1; bytes = 4; break;
    case 0x17: components = 2; bytes = 4; break;
    case 0x18: components = 3; bytes = 4; break;
    case 0x19: components = 4; bytes = 4; break;
    case 0x0E: components = 4; bytes = 0; break;
    default: return false;
    }
    if (type < 1 || type > 9 || type == 6 || type == 7)
        return false;
    const bool unorm = type == 1, snorm = type == 2, floating = type == 5;
    const bool isSigned = snorm || type == 4 || type == 9;
    const qint64 elementSize = bytes ? qint64(components) * bytes : 4;
    out.assign(size_t(vertices.count) * 4, 0.f);
    for (int i = 0; i < vertices.count; ++i) {
        float *dst = &out[size_t(i) * 4];
        dst[3] = 1.f;
        const qint64 at = buffer.start + qint64(i) * buffer.stride + a.offset;
        const char *p = v.raw(at, elementSize);
        if (channel == 0x0E) {
            const quint32 packed = qFromLittleEndian<quint32>(p);
            for (int c = 0; c < 4; ++c) {
                const int bits = c < 3 ? 10 : 2;
                const quint32 raw = (packed >> (c * 10)) & ((1u << bits) - 1);
                if (isSigned) {
                    const int value = int(raw << (32 - bits)) >> (32 - bits);
                    const float max = float((1 << (bits - 1)) - 1);
                    dst[c] = snorm ? std::max(-1.f, value / max) : float(value);
                } else {
                    dst[c] = unorm ? raw / float((1u << bits) - 1) : float(raw);
                }
            }
            continue;
        }
        for (int c = 0; c < components; ++c) {
            const char *q = p + c * bytes;
            float value = 0;
            if (bytes == 1) {
                value = isSigned ? float(qint8(*q)) : float(quint8(*q));
                if (unorm)
                    value /= 255.f;
                else if (snorm)
                    value = std::max(-1.f, value / 127.f);
            } else if (bytes == 2) {
                if (floating) {
                    qfloat16 h;
                    std::memcpy(&h, q, 2);
                    value = float(h);
                } else {
                    value = isSigned ? float(qFromLittleEndian<qint16>(q)) : float(qFromLittleEndian<quint16>(q));
                    if (unorm)
                        value /= 65535.f;
                    else if (snorm)
                        value = std::max(-1.f, value / 32767.f);
                }
            } else {
                if (floating) {
                    const quint32 bits = qFromLittleEndian<quint32>(q);
                    std::memcpy(&value, &bits, 4);
                } else {
                    value = isSigned ? float(qFromLittleEndian<qint32>(q)) : float(qFromLittleEndian<quint32>(q));
                    if (unorm)
                        value /= 4294967295.f;
                    else if (snorm)
                        value = std::max(-1.f, value / 2147483647.f);
                }
            }
            dst[c] = std::isfinite(value) ? value : 0.f;
        }
    }
    return true;
}

struct Shape
{
    QString name;
    int material = 0;
    int bone = 0;
    int vertices = 0;
    int skinCount = 0;
    std::vector<quint32> indices;
};

Shape readShape(const View &v, const Layout &l, const Header &h, qint64 at)
{
    Shape s;
    s.name = v.strAt(at + l.pick(0x08, 0x10));
    const qint64 meshArray = v.ptr(at + l.pick(0x18, 0x20));
    s.material = v.u16(at + l.pick(0x52, 0x5E));
    s.bone = v.u16(at + l.pick(0x54, 0x60));
    s.vertices = v.u16(at + l.pick(0x56, 0x62));
    s.skinCount = v.u8(at + l.pick(0x5A, 0x66));
    const int meshes = v.u8(at + l.pick(0x5B, 0x67));
    if (meshes == 0)
        return s;
    const qint64 mesh = meshArray;
    const quint32 primitive = v.u32(mesh + 0x24);
    if (primitive != 3)
        return s;
    const quint32 format = v.u32(mesh + 0x28);
    const int count = v.elements(v.u32(mesh + 0x2C));
    const qint64 start = h.poolData + v.u32(mesh + 0x20);
    const int size = format == 0 ? 1 : format == 1 ? 2 : format == 2 ? 4 : 0;
    if (size == 0)
        return s;
    const char *p = v.raw(start, qint64(count) * size);
    s.indices.resize(size_t(count));
    for (int i = 0; i < count; ++i)
        s.indices[size_t(i)] = size == 1   ? quint8(p[i])
                             : size == 2 ? qFromLittleEndian<quint16>(p + i * 2)
                                         : qFromLittleEndian<quint32>(p + i * 4);
    return s;
}

struct RenderInfo
{
    QString name;
    QStringList strings;
    std::vector<float> numbers;
};

struct Material
{
    QString name;
    QString shaderArchive;
    QString shadingModel;
    QStringList textures;
    QStringList samplers;
    std::vector<std::pair<int, int>> wraps;
    std::vector<RenderInfo> renderInfo;

    const RenderInfo *info(const QString &key) const
    {
        for (const RenderInfo &r : renderInfo)
            if (r.name == key)
                return &r;
        return nullptr;
    }
    QString infoText(const QString &key) const
    {
        const RenderInfo *r = info(key);
        if (!r)
            return {};
        if (!r->strings.isEmpty())
            return r->strings.front();
        return r->numbers.empty() ? QString() : QString::number(r->numbers.front());
    }
};

void readRenderInfoValues(const View &v, RenderInfo &info, int type, int n, qint64 values)
{
    for (int k = 0; k < n && values; ++k) {
        if (type == 0)
            info.numbers.push_back(float(v.s32(values + qint64(k) * 4)));
        else if (type == 1)
            info.numbers.push_back(v.f32(values + qint64(k) * 4));
        else if (type == 2)
            info.strings << v.strAt(values + qint64(k) * 8);
    }
}

Material readMaterialV10(const View &v, qint64 at)
{
    Material m;
    m.name = v.strAt(at + 0x08);
    const qint64 shaderInfo = v.ptr(at + 0x10);
    const qint64 textureNames = v.ptr(at + 0x20);
    const qint64 samplerArray = v.ptr(at + 0x30);
    const qint64 samplerDict = v.ptr(at + 0x38);
    const qint64 renderData = v.ptr(at + 0x40);
    const qint64 renderCounts = v.ptr(at + 0x48);
    const qint64 renderOffsets = v.ptr(at + 0x50);
    const int samplerCount = v.u8(at + 0xA2);
    const int textureCount = v.u8(at + 0xA3);
    for (int i = 0; i < textureCount; ++i)
        m.textures << v.strAt(textureNames + qint64(i) * 8);
    m.samplers = v.dict(samplerDict);
    for (int i = 0; i < samplerCount; ++i)
        m.wraps.emplace_back(v.u8(samplerArray + qint64(i) * 0x20), v.u8(samplerArray + qint64(i) * 0x20 + 1));
    const qint64 assign = shaderInfo ? v.ptr(shaderInfo) : 0;
    if (!assign)
        return m;
    m.shaderArchive = v.strAt(assign);
    m.shadingModel = v.strAt(assign + 8);
    const qint64 renderList = v.ptr(assign + 0x10);
    const int renderInfoCount = v.u16(assign + 0x48);
    for (int i = 0; i < renderInfoCount && renderList && renderCounts && renderOffsets && renderData; ++i) {
        RenderInfo info;
        info.name = v.nameAt(renderList + qint64(i) * 16);
        const int type = v.u8(renderList + qint64(i) * 16 + 8);
        const int n = v.count(v.u16(renderCounts + qint64(i) * 2));
        readRenderInfoValues(v, info, type, n, renderData + v.u16(renderOffsets + qint64(i) * 2));
        m.renderInfo.push_back(std::move(info));
    }
    return m;
}

Material readMaterial(const View &v, const Layout &l, qint64 at)
{
    if (l.v10)
        return readMaterialV10(v, at);
    Material m;
    m.name = v.strAt(at + l.pick(0x08, 0x10));
    const qint64 renderInfoArray = v.ptr(at + l.pick(0x10, 0x18));
    const qint64 shaderAssign = v.ptr(at + l.pick(0x20, 0x28));
    const qint64 textureNames = v.ptr(at + l.pick(0x30, 0x38));
    const qint64 samplerArray = v.ptr(at + l.pick(0x40, 0x48));
    const qint64 samplerDict = v.ptr(at + l.pick(0x48, 0x50));
    const int renderInfoCount = v.u16(at + l.pick(0x9A, 0xA6));
    const int samplerCount = v.u8(at + l.pick(0x9C, 0xA8));
    const int textureCount = v.u8(at + l.pick(0x9D, 0xA9));
    if (shaderAssign) {
        m.shaderArchive = v.strAt(shaderAssign);
        m.shadingModel = v.strAt(shaderAssign + 8);
    }
    for (int i = 0; i < textureCount; ++i)
        m.textures << v.strAt(textureNames + qint64(i) * 8);
    m.samplers = v.dict(samplerDict);
    for (int i = 0; i < samplerCount; ++i)
        m.wraps.emplace_back(v.u8(samplerArray + qint64(i) * 0x20), v.u8(samplerArray + qint64(i) * 0x20 + 1));
    for (int i = 0; i < renderInfoCount; ++i) {
        const qint64 r = renderInfoArray + qint64(i) * 0x18;
        RenderInfo info;
        info.name = v.strAt(r);
        const qint64 values = v.ptr(r + 8);
        const int n = v.count(v.u16(r + 0x10));
        const int type = v.u8(r + 0x12);
        readRenderInfoValues(v, info, type, n, values);
        m.renderInfo.push_back(std::move(info));
    }
    return m;
}

struct ModelSource
{
    QString name;
    Skeleton skeleton;
    std::vector<Vertices> vertices;
    std::vector<Shape> shapes;
    std::vector<Material> materials;
};

ModelSource readModel(const View &v, const Header &h, int index)
{
    const Layout &l = h.layout;
    const qint64 at = h.modelArray + qint64(index) * 0x78;
    if (QByteArray(v.raw(at, 4), 4) != "FMDL")
        throw BadOffset();
    ModelSource m;
    m.name = v.strAt(at + l.pick(0x08, 0x10));
    const qint64 skeleton = v.ptr(at + l.pick(0x18, 0x20));
    const qint64 vertexArray = v.ptr(at + l.pick(0x20, 0x28));
    const qint64 shapeArray = v.ptr(at + l.pick(0x28, 0x30));
    const qint64 materialArray = v.ptr(at + l.pick(0x38, 0x40));
    const int vertexCount = v.u16(at + 0x68);
    const int shapeCount = v.u16(at + 0x6A);
    const int materialCount = v.u16(at + 0x6C);
    m.skeleton = readSkeleton(v, l, skeleton);
    for (int i = 0; i < vertexCount; ++i)
        m.vertices.push_back(readVertices(v, l, h, vertexArray + qint64(i) * l.pick(0x58, 0x60)));
    for (int i = 0; i < shapeCount; ++i)
        m.shapes.push_back(readShape(v, l, h, shapeArray + qint64(i) * l.pick(0x60, 0x70)));
    for (int i = 0; i < materialCount; ++i)
        m.materials.push_back(readMaterial(v, l, materialArray + qint64(i) * (l.v10 ? 0xB0 : l.pick(0xA8, 0xB8))));
    return m;
}

int pickTexture(const Material &m, const QStringList &samplers, const QStringList &nameWords,
                const QStringList &avoidWords = {})
{
    for (const QString &wanted : samplers) {
        const int i = int(m.samplers.indexOf(wanted));
        if (i >= 0 && i < m.textures.size())
            return i;
    }
    for (int i = 0; i < m.textures.size(); ++i) {
        const QString name = m.textures[i].toLower();
        const bool avoided = std::any_of(avoidWords.begin(), avoidWords.end(),
                                         [&](const QString &w) { return name.contains(w); });
        if (!avoided && std::any_of(nameWords.begin(), nameWords.end(), [&](const QString &w) { return name.contains(w); }))
            return i;
    }
    return -1;
}

ModelMaterial convertMaterial(const Material &m, Model3D &model)
{
    ModelMaterial out;
    out.name = m.name;
    auto textureIndex = [&](int i) -> int {
        if (i < 0)
            return -1;
        const QString &name = m.textures[i];
        for (size_t t = 0; t < model.textures.size(); ++t)
            if (model.textures[t].name == name)
                return int(t);
        ModelTexture texture;
        texture.name = name;
        if (size_t(i) < m.wraps.size()) {
            texture.wrapU = std::min(m.wraps[size_t(i)].first, 2);
            texture.wrapV = std::min(m.wraps[size_t(i)].second, 2);
        }
        model.textures.push_back(texture);
        return int(model.textures.size() - 1);
    };
    int albedo = pickTexture(m, {"_a0", "_a"}, {"_alb", "albedo", "basecolor", "diffuse", "_col"}, {"albgry"});
    if (albedo < 0)
        albedo = pickTexture(m, {"_albgry0"}, {"alb"});
    if (albedo < 0 && m.textures.size() == 1) {
        const QString name = m.textures.front().toLower();
        const QString sampler = m.samplers.isEmpty() ? QString() : m.samplers.front().toLower();
        static const QStringList notColour = {"_nrm", "normal", "_mix", "_spm", "_spec", "_rgh", "_mtl", "_ao", "_emm", "_msk", "_mask"};
        static const QStringList notColourSamplers = {"_n0", "_s0", "_e0", "_r0", "_m0", "_o0"};
        const bool data = std::any_of(notColour.begin(), notColour.end(), [&](const QString &w) { return name.contains(w); })
                       || notColourSamplers.contains(sampler);
        if (!data)
            albedo = 0;
    }
    out.baseColorTexture = textureIndex(albedo);
    out.normalTexture = textureIndex(pickTexture(m, {"_n0", "_n"}, {"_nrm", "normal"}));

    const QString mode = m.infoText("gsys_render_state_mode").toLower();
    const QString alphaTest = m.infoText("gsys_alpha_test_enable").toLower();
    if (mode == "translucent" || mode == "xlu") {
        out.alphaMode = AlphaMode::Blend;
    } else if (mode == "mask" || alphaTest == "true" || alphaTest == "1") {
        out.alphaMode = AlphaMode::Mask;
        bool ok = false;
        const float cutoff = m.infoText("gsys_alpha_test_value").toFloat(&ok);
        if (ok && cutoff > 0 && cutoff < 1)
            out.alphaCutoff = cutoff;
    }
    const QString face = m.infoText("gsys_render_state_display_face").toLower();
    out.doubleSided = !(face == "front" || face == "back");
    out.auxiliary = m.textures.isEmpty() && m.name.contains(QStringLiteral("shadow"), Qt::CaseInsensitive);
    return out;
}

struct EmbeddedTextures
{
    QByteArray data;
    std::map<QString, std::pair<qsizetype, BntxTexture>> byName;

    std::shared_ptr<const Image> decode(const QString &name) const
    {
        auto it = byName.find(name);
        if (it == byName.end()) {
            const qsizetype dot = name.lastIndexOf(u'.');
            if (dot > 0)
                it = byName.find(name.left(dot));
        }
        if (it == byName.end())
            return nullptr;
        std::optional<Image> image = decodeBntxTexture(data, it->second.first, it->second.second);
        return image ? std::make_shared<const Image>(std::move(*image)) : nullptr;
    }
};

std::shared_ptr<const EmbeddedTextures> findEmbeddedTextures(const QByteArray &data)
{
    auto out = std::make_shared<EmbeddedTextures>();
    out->data = data;
    for (qsizetype offset : findEmbeddedBntx(data)) {
        if (std::optional<BntxFile> file = parseBntx(data, offset, nullptr))
            for (const BntxTexture &t : file->textures)
                out->byName.emplace(t.name, std::make_pair(offset, t));
    }
    return out;
}

std::array<float, 3> rotateVector(const Matrix4 &m, const float *n)
{
    return {m[0] * n[0] + m[4] * n[1] + m[8] * n[2], m[1] * n[0] + m[5] * n[1] + m[9] * n[2],
            m[2] * n[0] + m[6] * n[1] + m[10] * n[2]};
}

std::shared_ptr<Model3D> buildModel(const View &v, const ModelSource &source,
                                    const std::shared_ptr<const EmbeddedTextures> &textures)
{
    auto model = std::make_shared<Model3D>();
    for (const Bone &b : source.skeleton.bones) {
        ModelNode node;
        node.name = b.name;
        node.parent = b.parent;
        node.translation = b.translation;
        node.rotation = boneRotation(b);
        node.scale = b.scale;
        model->nodes.push_back(node);
    }
    std::vector<Matrix4> world(model->nodes.size());
    for (size_t i = 0; i < world.size(); ++i)
        world[i] = model->worldMatrix(int(i));
    for (const Material &m : source.materials)
        model->materials.push_back(convertMaterial(m, *model));
    if (textures && !textures->byName.empty())
        model->embeddedTexture = [textures](const QString &name) { return textures->decode(name); };

    const std::vector<int> &palette = source.skeleton.palette;
    for (const Shape &shape : source.shapes) {
        if (shape.indices.empty() || size_t(shape.vertices) >= source.vertices.size())
            continue;
        const Vertices &vertices = source.vertices[size_t(shape.vertices)];
        std::map<QString, std::vector<float>> attributes;
        for (const Attribute &a : vertices.attributes) {
            std::vector<float> values;
            if (readAttribute(v, vertices, a, values))
                attributes[a.name] = std::move(values);
        }
        const auto positions = attributes.find(QStringLiteral("_p0"));
        if (positions == attributes.end())
            continue;
        const int count = vertices.count;
        const int shapeBone = shape.bone < int(model->nodes.size()) ? shape.bone : 0;

        ModelMesh mesh;
        mesh.name = shape.name;
        mesh.node = shapeBone;
        if (!model->nodes.empty()) {
            if (shape.skinCount == 0) {
                mesh.joints = {shapeBone};
            } else {
                mesh.joints = palette;
                if (mesh.joints.empty())
                    mesh.joints = {shapeBone};
            }
        }
        model->meshes.push_back(mesh);

        ModelPrimitive prim;
        prim.mesh = int(model->meshes.size() - 1);
        prim.material = shape.material < int(model->materials.size()) ? shape.material : -1;
        prim.positions.resize(size_t(count) * 3);
        const std::vector<float> &p = positions->second;
        const auto normals = attributes.find(QStringLiteral("_n0"));
        if (normals != attributes.end())
            prim.normals.resize(size_t(count) * 3);
        for (int i = 0; i < count; ++i)
            for (int c = 0; c < 3; ++c) {
                prim.positions[size_t(i) * 3 + size_t(c)] = p[size_t(i) * 4 + size_t(c)];
                if (normals != attributes.end())
                    prim.normals[size_t(i) * 3 + size_t(c)] = normals->second[size_t(i) * 4 + size_t(c)];
            }
        auto uv = [&](const char *name, std::vector<float> &out) {
            const auto it = attributes.find(QString::fromLatin1(name));
            if (it == attributes.end())
                return;
            out.resize(size_t(count) * 2);
            for (int i = 0; i < count; ++i) {
                out[size_t(i) * 2] = it->second[size_t(i) * 4];
                out[size_t(i) * 2 + 1] = 1.f - it->second[size_t(i) * 4 + 1];
            }
        };
        uv("_u0", prim.uv0);
        uv("_u1", prim.uv1);
        if (const auto colors = attributes.find(QStringLiteral("_c0")); colors != attributes.end()) {
            prim.colorsAreData = true;
            prim.colors.resize(size_t(count) * 4);
            for (size_t i = 0; i < prim.colors.size(); ++i)
                prim.colors[i] = quint8(std::lround(std::clamp(colors->second[i], 0.f, 1.f) * 255.f));
        }

        const auto i0 = attributes.find(QStringLiteral("_i0"));
        const auto i1 = attributes.find(QStringLiteral("_i1"));
        const auto w0 = attributes.find(QStringLiteral("_w0"));
        const auto w1 = attributes.find(QStringLiteral("_w1"));
        const int influences = std::min(shape.skinCount, 8);
        if (!model->nodes.empty()) {
            prim.joints.assign(size_t(count) * 4, 0);
            prim.weights.assign(size_t(count) * 4, 0.f);
        }
        for (int i = 0; i < count && !model->nodes.empty(); ++i) {
            const size_t base = size_t(i) * 4;
            if (shape.skinCount == 0 || i0 == attributes.end()) {
                prim.weights[base] = 1.f;
                if (shape.skinCount == 0) {
                    const Matrix4 &m = world[size_t(shapeBone)];
                    const auto moved = transformPoint(m, &prim.positions[size_t(i) * 3]);
                    std::copy(moved.begin(), moved.end(), prim.positions.begin() + qsizetype(i) * 3);
                    if (!prim.normals.empty()) {
                        const auto n = rotateVector(m, &prim.normals[size_t(i) * 3]);
                        std::copy(n.begin(), n.end(), prim.normals.begin() + qsizetype(i) * 3);
                    }
                }
                continue;
            }
            std::array<std::pair<float, int>, 8> influence{};
            for (int k = 0; k < influences; ++k) {
                const auto &indexSource = k < 4 ? i0 : i1;
                const auto &weightSource = k < 4 ? w0 : w1;
                if (indexSource == attributes.end())
                    break;
                const int slot = int(indexSource->second[base + size_t(k % 4)]);
                float weight = weightSource == attributes.end() ? (k == 0 ? 1.f : 0.f)
                                                                 : weightSource->second[base + size_t(k % 4)];
                if (influences == 1)
                    weight = 1.f;
                influence[size_t(k)] = {weight, slot};
            }
            std::sort(influence.begin(), influence.begin() + influences,
                      [](const auto &a, const auto &b) { return a.first > b.first; });
            float total = 0;
            for (int k = 0; k < std::min(influences, 4); ++k)
                total += std::max(0.f, influence[size_t(k)].first);
            for (int k = 0; k < std::min(influences, 4); ++k) {
                const int slot = influence[size_t(k)].second;
                prim.joints[base + size_t(k)] = quint16(slot >= 0 && size_t(slot) < mesh.joints.size() ? slot : 0);
                prim.weights[base + size_t(k)] = total > 0 ? std::max(0.f, influence[size_t(k)].first) / total : 0.f;
            }
            if (!(total > 0))
                prim.weights[base] = 1.f;
            if (shape.skinCount == 1) {
                const int node = mesh.joints[prim.joints[base]];
                const Matrix4 &m = world[size_t(node)];
                const auto moved = transformPoint(m, &prim.positions[size_t(i) * 3]);
                std::copy(moved.begin(), moved.end(), prim.positions.begin() + qsizetype(i) * 3);
                if (!prim.normals.empty()) {
                    const auto n = rotateVector(m, &prim.normals[size_t(i) * 3]);
                    std::copy(n.begin(), n.end(), prim.normals.begin() + qsizetype(i) * 3);
                }
            }
        }
        prim.indices.reserve(shape.indices.size());
        for (size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
            const quint32 a = shape.indices[t], b = shape.indices[t + 1], c = shape.indices[t + 2];
            if (a < quint32(count) && b < quint32(count) && c < quint32(count))
                prim.indices.insert(prim.indices.end(), {a, b, c});
        }
        model->primitives.push_back(std::move(prim));
    }
    return model;
}

struct Curve
{
    int type = 0;
    bool hermite = false;
    bool keyed = false;
    quint32 target = 0;
    float start = 0;
    float end = 0;
    std::vector<float> frames;
    std::vector<std::array<float, 4>> keys;

    float at(float frame) const
    {
        if (frames.empty() || keys.empty())
            return 0;
        const size_t n = std::min(frames.size(), keys.size());
        size_t i = 0;
        while (i + 1 < n && frames[i + 1] <= frame)
            ++i;
        const auto &k = keys[i];
        if (hermite || keyed) {
            if (frame <= frames[0] || i + 1 >= n)
                return k[0];
            const float span = frames[i + 1] - frames[i];
            const float s = span > 0 ? std::clamp((frame - frames[i]) / span, 0.f, 1.f) : 0.f;
            const auto &k1 = keys[i + 1];
            if (keyed)
                return k[0] + (k1[0] - k[0]) * s;
            const float s2 = s * s, s3 = s2 * s;
            return (2 * s3 - 3 * s2 + 1) * k[0] + (s3 - 2 * s2 + s) * span * k[1] + (-2 * s3 + 3 * s2) * k1[0]
                 + (s3 - s2) * span * k1[1];
        }
        if (type != 0 && type != 1)
            return k[0];
        if (frame <= frames[0])
            return k[0];
        const float next = i + 1 < n ? frames[i + 1] : end;
        const float span = next - frames[i];
        const float t = span > 0 ? std::clamp((frame - frames[i]) / span, 0.f, 1.f) : 0.f;
        if (type == 1)
            return k[0] + k[1] * t;
        return k[0] + t * (k[1] + t * (k[2] + t * k[3]));
    }
};

void readCurveKeysV10(const View &v, Curve &curve, qint64 keyArray, int keyType, int n, float scale, float offset,
                      float delta)
{
    const bool cubic = curve.type == 0;
    curve.hermite = cubic;
    curve.keyed = !cubic;
    auto finite = [](float f) { return std::isfinite(f) ? f : 0.f; };
    if (keyType == 0) {
        const int perKey = cubic ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            std::array<float, 4> key{};
            key[0] = finite(v.f32(keyArray + qint64(i) * perKey * 4));
            if (cubic)
                key[1] = finite(v.f32(keyArray + qint64(i) * perKey * 4 + 4));
            curve.keys.push_back(key);
        }
        return;
    }
    qint64 at = keyArray;
    float slopeScale = 0, slopeOffset = 0;
    if (cubic) {
        slopeScale = v.f32(at);
        slopeOffset = v.f32(at + 4);
        at += 8;
    }
    const int size = keyType == 1 ? 2 : 1;
    auto raw = [&](qint64 where) { return size == 2 ? float(v.u16(where)) : float(v.u8(where)); };
    for (int i = 0; i < n; ++i) {
        std::array<float, 4> key{};
        key[0] = finite(delta + raw(at) * offset);
        at += size;
        if (cubic) {
            key[1] = finite(raw(at) * slopeScale + slopeOffset);
            at += size;
        }
        curve.keys.push_back(key);
    }
    (void)scale;
}

std::vector<Curve> readCurves(const View &v, qint64 at, int count, bool v10)
{
    std::vector<Curve> curves;
    for (int c = 0; c < count; ++c) {
        const qint64 e = at + qint64(c) * 0x30;
        const qint64 frameArray = v.ptr(e);
        const qint64 keyArray = v.ptr(e + 8);
        const quint16 flags = v.u16(e + 0x10);
        const int n = v.count(v.u16(e + 0x12));
        Curve curve;
        curve.target = v.u32(e + 0x14);
        curve.start = v.f32(e + 0x18);
        curve.end = v.f32(e + 0x1C);
        const float scale = v.f32(e + 0x20);
        const float offset = v.f32(e + 0x24);
        curve.type = (flags >> 4) & 7;
        const int frameType = flags & 3;
        const int keyType = (flags >> 2) & 3;
        const int perKey = curve.type == 0 ? 4 : curve.type == 1 ? 2 : 1;
        const int keySize = keyType == 0 ? 4 : keyType == 1 ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            if (frameType == 0)
                curve.frames.push_back(v.f32(frameArray + qint64(i) * 4));
            else if (frameType == 1)
                curve.frames.push_back(v.u16(frameArray + qint64(i) * 2) / 32.f);
            else
                curve.frames.push_back(v.u8(frameArray + i));
        }
        if (v10 && (curve.type == 0 || curve.type == 1 || curve.type == 3)) {
            readCurveKeysV10(v, curve, keyArray, keyType, n, scale, offset, v.f32(e + 0x28));
            curves.push_back(std::move(curve));
            continue;
        }
        const bool scaled = curve.type <= 2;
        for (int i = 0; i < n; ++i) {
            std::array<float, 4> key{};
            for (int j = 0; j < perKey; ++j) {
                const qint64 k = keyArray + (qint64(i) * perKey + j) * keySize;
                float value = keyType == 0   ? (curve.type == 6 ? float(v.u32(k)) : v.f32(k))
                            : keyType == 1 ? float(v.s16(k))
                                           : float(v.s8(k));
                if (scaled && keyType != 0) {
                    value *= scale;
                    if (j == 0)
                        value += offset;
                }
                key[size_t(j)] = std::isfinite(value) ? value : 0.f;
            }
            curve.keys.push_back(key);
        }
        curves.push_back(std::move(curve));
    }
    return curves;
}

struct BoneTrack
{
    QString name;
    quint32 flags = 0;
    int curveCount = 0;
    std::array<float, 10> base{1, 1, 1, 0, 0, 0, 1, 0, 0, 0};
    bool hasBase[3] = {false, false, false};
    std::vector<Curve> curves;
};

struct Clip
{
    QString name;
    quint32 flags = 0;
    int frames = 0;
    std::vector<BoneTrack> tracks;
};

Clip readClip(const View &v, const Header &h, int index, bool withCurves)
{
    const Layout &l = h.layout;
    const qint64 at = h.clipArray + qint64(index) * l.pick(0x50, 0x60);
    if (QByteArray(v.raw(at, 4), 4) != "FSKA")
        throw BadOffset();
    Clip clip;
    clip.name = v.strAt(at + l.pick(0x08, 0x10));
    clip.flags = v.u32(at + l.pick(0x04, 0x48));
    const qint64 boneArray = v.ptr(at + l.pick(0x28, 0x30));
    clip.frames = v.count(v.u32(at + l.pick(0x40, 0x4C)));
    const int bones = v.count(v.u16(at + l.pick(0x4C, 0x58)));
    for (int i = 0; i < bones; ++i) {
        const qint64 b = boneArray + qint64(i) * l.pick(0x38, 0x28);
        BoneTrack track;
        track.name = v.strAt(b);
        track.flags = v.u32(b + l.pick(0x28, 0x18));
        track.curveCount = v.u8(b + l.pick(0x2E, 0x1E));
        if (withCurves) {
            const qint64 curves = v.ptr(b + 8);
            const qint64 base = v.ptr(b + 0x10);
            const int curveCount = track.curveCount;
            int next = 0;
            const int sizes[3] = {3, 4, 3};
            const int baseSlot[3] = {0, 3, 7};
            for (int g = 0; g < 3; ++g) {
                if (!(track.flags & (1u << (3 + g))))
                    continue;
                track.hasBase[g] = true;
                for (int k = 0; k < sizes[g]; ++k)
                    track.base[size_t(baseSlot[g] + k)] = v.f32(base + qint64(next + k) * 4);
                next += sizes[g];
            }
            track.curves = readCurves(v, curves, curveCount, l.v10);
        }
        clip.tracks.push_back(std::move(track));
    }
    return clip;
}

int curveSlot(quint32 target)
{
    switch (target) {
    case 0x04: return 0;
    case 0x08: return 1;
    case 0x0C: return 2;
    case 0x20: return 3;
    case 0x24: return 4;
    case 0x28: return 5;
    case 0x2C: return 6;
    case 0x10: return 7;
    case 0x14: return 8;
    case 0x18: return 9;
    default: return -1;
    }
}

ModelAnimation convertClip(const Clip &clip, const Model3D &model)
{
    ModelAnimation animation;
    animation.name = clip.name;
    QHash<QString, int> nodes;
    for (size_t i = 0; i < model.nodes.size(); ++i)
        nodes.insert(model.nodes[i].name, int(i));
    const bool euler = (clip.flags & 0x1000) != 0;
    const int frames = std::max(clip.frames, 0);
    std::vector<float> times(size_t(frames) + 1);
    for (int f = 0; f <= frames; ++f)
        times[size_t(f)] = float(f) / kFramesPerSecond;

    for (const BoneTrack &track : clip.tracks) {
        const auto node = nodes.find(track.name);
        if (node == nodes.end())
            continue;
        std::array<const Curve *, 10> bySlot{};
        for (const Curve &c : track.curves)
            if (const int slot = curveSlot(c.target); slot >= 0)
                bySlot[size_t(slot)] = &c;
        auto value = [&](int slot, float frame) {
            return bySlot[size_t(slot)] ? bySlot[size_t(slot)]->at(frame) : track.base[size_t(slot)];
        };
        const int first[3] = {0, 3, 7};
        const int width[3] = {3, 4, 3};
        const AnimationPath paths[3] = {AnimationPath::Scale, AnimationPath::Rotation, AnimationPath::Translation};
        for (int g = 0; g < 3; ++g) {
            bool animated = false;
            for (int k = 0; k < width[g]; ++k)
                animated = animated || bySlot[size_t(first[g] + k)];
            if (!animated && !track.hasBase[g])
                continue;
            AnimationChannel channel;
            channel.node = *node;
            channel.path = paths[g];
            const int samples = animated ? frames + 1 : 1;
            channel.times.assign(times.begin(), times.begin() + samples);
            std::array<float, 4> previous{0, 0, 0, 1};
            for (int s = 0; s < samples; ++s) {
                const float frame = float(s);
                if (g == 1) {
                    std::array<float, 4> q = euler ? quaternionFromEulerZYX(value(3, frame), value(4, frame), value(5, frame))
                                                   : normalizedQuaternion({value(3, frame), value(4, frame),
                                                                           value(5, frame), value(6, frame)});
                    if (s > 0 && q[0] * previous[0] + q[1] * previous[1] + q[2] * previous[2] + q[3] * previous[3] < 0)
                        for (float &c : q)
                            c = -c;
                    previous = q;
                    channel.values.insert(channel.values.end(), q.begin(), q.end());
                } else {
                    for (int k = 0; k < 3; ++k)
                        channel.values.push_back(value(first[g] + k, frame));
                }
            }
            animation.channels.push_back(std::move(channel));
        }
    }
    return animation;
}

class ModelTable : public SimpleTable
{
public:
    ModelTable(QString name, std::vector<Row> rows, QByteArray data, Header header, int index,
               std::shared_ptr<const EmbeddedTextures> textures, QString path)
        : SimpleTable(std::move(name), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_data(std::move(data)), m_header(std::move(header)), m_index(index), m_textures(std::move(textures)),
          m_path(std::move(path))
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
                const View v(m_data);
                auto model = buildModel(v, readModel(v, m_header, m_index), m_textures);
                if (QFileInfo(m_path).isAbsolute())
                    model->sourcePath = m_path;
                m_model = std::move(model);
            } catch (const std::exception &) {
                m_model.reset();
            }
        });
        return m_model;
    }

private:
    QByteArray m_data;
    Header m_header;
    int m_index;
    std::shared_ptr<const EmbeddedTextures> m_textures;
    QString m_path;
    mutable std::once_flag m_made;
    mutable std::shared_ptr<const Model3D> m_model;
};

class EmbeddedFilesTable : public SimpleTable
{
public:
    EmbeddedFilesTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Embedded files"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("type")}}, std::move(rows)),
          m_archive(std::move(archive))
    {
    }

    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

std::unique_ptr<Document> parse(const QByteArray &data, const QString &path, QString *error)
{
    const View v(data);
    const Header h = readHeader(v, data);
    auto doc = std::make_unique<Document>();
    const auto textures = findEmbeddedTextures(data);

    std::vector<SimpleTable::Row> materialRows;
    for (int i = 0; i < h.models.size(); ++i) {
        const ModelSource source = readModel(v, h, i);
        qint64 vertexTotal = 0, triangles = 0;
        int skinned = 0;
        for (const Shape &s : source.shapes) {
            triangles += qint64(s.indices.size() / 3);
            skinned += s.skinCount > 1 ? 1 : 0;
            if (size_t(s.vertices) < source.vertices.size())
                vertexTotal += source.vertices[size_t(s.vertices)].count;
        }
        QStringList textureNames;
        for (const Material &m : source.materials) {
            for (const QString &t : m.textures)
                if (!textureNames.contains(t))
                    textureNames << t;
            materialRows.push_back({source.name, m.name, QStringLiteral("%1 / %2").arg(m.shaderArchive, m.shadingModel),
                                    m.textures.join(QStringLiteral(", ")), m.samplers.join(QStringLiteral(", ")),
                                    m.infoText("gsys_render_state_mode")});
        }
        std::vector<SimpleTable::Row> rows = {
            {QObject::tr("name"), source.name},
            {QObject::tr("BFRES version"), versionText(h.version)},
            {QObject::tr("bones"), int(source.skeleton.bones.size())},
            {QObject::tr("shapes"), int(source.shapes.size())},
            {QObject::tr("skinned shapes"), skinned},
            {QObject::tr("materials"), int(source.materials.size())},
            {QObject::tr("vertices"), vertexTotal},
            {QObject::tr("triangles"), triangles},
            {QObject::tr("textures"), textureNames.join(QStringLiteral(", "))},
            {QObject::tr("animations in this file"), int(h.clips.size())},
        };
        auto table = std::make_shared<ModelTable>(source.name, std::move(rows), data, h, i, textures, path);
        table->setDescription(QObject::tr("%1: %n bone(s), %2 shapes", nullptr, int(source.skeleton.bones.size()))
                                  .arg(source.name)
                                  .arg(source.shapes.size()));
        doc->tables.push_back(std::move(table));
    }

    if (!h.clips.isEmpty()) {
        std::vector<SimpleTable::Row> rows;
        for (int i = 0; i < h.clips.size(); ++i) {
            const Clip clip = readClip(v, h, i, false);
            QStringList bones;
            for (const BoneTrack &t : clip.tracks)
                bones << t.name;
            rows.push_back({clip.name, clip.frames, (clip.flags & 4) ? QObject::tr("yes") : QObject::tr("no"),
                            int(clip.tracks.size()), bones.join(QStringLiteral(", "))});
        }
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Skeletal animations"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("frames")}, {QObject::tr("loops")},
                                      {QObject::tr("bones")}, {QObject::tr("bone names")}},
            std::move(rows));
        table->setDescription(QObject::tr("%n skeletal animation(s). Export them with a model that has these bones.",
                                          nullptr, int(h.clips.size())));
        doc->tables.push_back(std::move(table));
    }

    if (!h.materialAnims.isEmpty() || !h.boneVisibilityAnims.isEmpty() || !h.shapeAnims.isEmpty()
        || !h.sceneAnims.isEmpty()) {
        std::vector<SimpleTable::Row> rows;
        for (int i = 0; i < h.materialAnims.size(); ++i) {
            QVariant frames, loops;
            if (h.layout.v10 && h.materialAnimArray) {
                const qint64 at = h.materialAnimArray + qint64(i) * 0x70;
                if (v.u32(at) == 0x41414D46u) {
                    frames = v.s32(at + 0x58);
                    loops = (v.u32(at + 4) & 4) ? QObject::tr("yes") : QObject::tr("no");
                }
            }
            rows.push_back({h.materialAnims[i], QObject::tr("material"), frames, loops});
        }
        for (const QString &name : h.boneVisibilityAnims)
            rows.push_back({name, QObject::tr("bone visibility"), QVariant(), QVariant()});
        for (const QString &name : h.shapeAnims)
            rows.push_back({name, QObject::tr("shape"), QVariant(), QVariant()});
        for (const QString &name : h.sceneAnims)
            rows.push_back({name, QObject::tr("scene"), QVariant(), QVariant()});
        const int count = int(rows.size());
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Other animations"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("kind")}, {QObject::tr("frames")},
                                      {QObject::tr("loops")}},
            std::move(rows));
        table->setDescription(QObject::tr("%n material, visibility, shape or scene animation(s).", nullptr, count));
        doc->tables.push_back(std::move(table));
    }

    if (!h.externalFiles.isEmpty() && h.externalArray) {
        auto archive = std::make_shared<Archive>();
        archive->data = data;
        std::vector<SimpleTable::Row> rows;
        for (int i = 0; i < h.externalFiles.size(); ++i) {
            const qint64 at = h.externalArray + qint64(i) * 0x10;
            if (at < 0 || at + 0x10 > data.size())
                break;
            const quint64 offset = qFromLittleEndian<quint64>(data.constData() + at);
            const quint32 size = qFromLittleEndian<quint32>(data.constData() + at + 8);
            if (offset == 0 || offset > quint64(data.size()) || size > quint64(data.size()) - offset)
                continue;
            ArchiveEntry entry;
            entry.name = h.externalFiles[i];
            entry.offset = qsizetype(offset);
            entry.size = qsizetype(size);
            rows.push_back({entry.name, qint64(entry.size),
                            describeContent(QByteArrayView(data).sliced(entry.offset, std::min<qsizetype>(entry.size, 16)))});
            archive->entries.push_back(std::move(entry));
        }
        if (!archive->entries.empty()) {
            const int count = int(archive->entries.size());
            auto table = std::make_shared<EmbeddedFilesTable>(std::move(rows), archive);
            table->setDescription(QObject::tr("%n file(s) stored inside the BFRES", nullptr, count));
            doc->tables.push_back(std::move(table));
        }
    }

    if (!materialRows.empty()) {
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Materials"),
            std::vector<SimpleColumn>{{QObject::tr("model")}, {QObject::tr("material")}, {QObject::tr("shader")},
                                      {QObject::tr("textures")}, {QObject::tr("samplers")}, {QObject::tr("render mode")}},
            std::move(materialRows));
        doc->tables.push_back(std::move(table));
    }

    if (!textures->byName.empty()) {
        QString ignored;
        if (auto textureDoc = BntxReader().readBytes(data, {}, &ignored))
            for (auto &t : textureDoc->tables)
                doc->tables.push_back(std::move(t));
    }

    if (doc->tables.empty()) {
        if (error)
            *error = QObject::tr("The BFRES file has no models, animations, textures or embedded files.");
        return nullptr;
    }
    return doc;
}

}

bool BfresReader::isBfres(QByteArrayView head)
{
    return head.size() >= 0x10 && head.startsWith("FRES    ");
}

int BfresReader::score(const QString &, QByteArrayView head) const
{
    return isBfres(head) ? 110 : 0;
}

std::unique_ptr<Document> BfresReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 1 GB.");
        return nullptr;
    }
    const QByteArray data = file.readAll();
    try {
        return parse(data, path, error);
    } catch (const BadOffset &) {
        if (error)
            *error = QObject::tr("The BFRES file is damaged or uses a layout this app does not know.");
    } catch (const std::runtime_error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
    }
    return nullptr;
}

std::unique_ptr<Document> BfresReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    try {
        return parse(data, fileName, error);
    } catch (const BadOffset &) {
        if (error)
            *error = QObject::tr("The BFRES file is damaged or uses a layout this app does not know.");
    } catch (const std::runtime_error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
    }
    return nullptr;
}

std::vector<BfresClip> BfresReader::skeletalClips(const QByteArray &data, QString *error)
{
    std::vector<BfresClip> clips;
    try {
        const View v(data);
        const Header h = readHeader(v, data);
        for (int i = 0; i < h.clips.size(); ++i) {
            const Clip clip = readClip(v, h, i, false);
            BfresClip out;
            out.name = clip.name;
            out.frames = clip.frames;
            out.loop = (clip.flags & 4) != 0;
            for (const BoneTrack &t : clip.tracks) {
                out.bones << t.name;
                if (t.curveCount > 0)
                    out.animatedBones << t.name;
            }
            clips.push_back(std::move(out));
        }
    } catch (const BadOffset &) {
        if (error)
            *error = QObject::tr("The BFRES file is damaged or uses a layout this app does not know.");
    } catch (const std::runtime_error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
    }
    return clips;
}

std::optional<ModelAnimation> BfresReader::readSkeletalClip(const QByteArray &data, const QString &clip,
                                                            const Model3D &model, QString *error)
{
    try {
        const View v(data);
        const Header h = readHeader(v, data);
        const int index = int(h.clips.indexOf(clip));
        if (index < 0) {
            if (error)
                *error = QObject::tr("There is no animation called %1 in this file.").arg(clip);
            return std::nullopt;
        }
        ModelAnimation animation = convertClip(readClip(v, h, index, true), model);
        if (animation.channels.empty()) {
            if (error)
                *error = QObject::tr("None of the bones in %1 are in the model.").arg(clip);
            return std::nullopt;
        }
        return animation;
    } catch (const BadOffset &) {
        if (error)
            *error = QObject::tr("The BFRES file is damaged or uses a layout this app does not know.");
    } catch (const std::runtime_error &e) {
        if (error)
            *error = QString::fromStdString(e.what());
    }
    return std::nullopt;
}
