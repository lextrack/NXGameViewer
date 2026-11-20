#include "core/registry.h"
#include "textures/texturedecode.h"
#include "unity/lz4block.h"
#include "unity/serializedfile.h"
#include "unity/builtintypetrees.h"
#include "unity/unitybundle.h"
#include "unity/unitymesh.h"
#include "unity/unityreader.h"
#include "unity/unityscene.h"
#include "models/gltfwriter.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <zlib.h>

namespace {

class Bytes
{
public:
    QByteArray data;

    template <typename T> Bytes &le(T v)
    {
        char raw[sizeof(T)];
        qToLittleEndian(v, raw);
        data.append(raw, sizeof(T));
        return *this;
    }
    template <typename T> Bytes &be(T v)
    {
        char raw[sizeof(T)];
        qToBigEndian(v, raw);
        data.append(raw, sizeof(T));
        return *this;
    }
    Bytes &cstr(const QByteArray &s)
    {
        data += s;
        data += '\0';
        return *this;
    }
    Bytes &raw(const QByteArray &s)
    {
        data += s;
        return *this;
    }
    Bytes &align(int to)
    {
        while (data.size() % to)
            data += '\0';
        return *this;
    }
};

constexpr quint32 kCommon = 0x80000000u;
constexpr quint32 kInt = kCommon | 222, kString = kCommon | 840, kName = kCommon | 427, kArray = kCommon | 49,
                  kChar = kCommon | 81, kUInt8 = kCommon | 928, kTypeless = kCommon | 894, kVector = kCommon | 981,
                  kUInt = kCommon | 934, kUInt64 = kCommon | 921, kSize = kCommon | 795, kData = kCommon | 106;

struct Node
{
    int level;
    QByteArray type, name;
    quint32 commonType = 0, commonName = 0;
    int size = -1;
    quint32 meta = 0;
};

QByteArray typeTree(const std::vector<Node> &nodes)
{
    QByteArray strings;
    Bytes out;
    out.le<qint32>(qint32(nodes.size()));
    Bytes body;
    auto text = [&](quint32 common, const QByteArray &s) {
        if (common)
            return common;
        const quint32 at = quint32(strings.size());
        strings += s + '\0';
        return at;
    };
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node &n = nodes[i];
        body.le<quint16>(1).le<quint8>(quint8(n.level)).le<quint8>(0);
        body.le<quint32>(text(n.commonType, n.type)).le<quint32>(text(n.commonName, n.name));
        body.le<qint32>(n.size).le<qint32>(qint32(i)).le<quint32>(n.meta).le<quint64>(0);
    }
    out.le<qint32>(qint32(strings.size()));
    return out.data + body.data + strings;
}

std::vector<Node> stringNode(int level, quint32 name, const QByteArray &nameText = {})
{
    return {{level, {}, nameText, kString, name, -1, 0},
            {level + 1, {}, {}, kArray, kArray, -1, 0x4000},
            {level + 2, {}, {}, kInt, kSize, 4, 0},
            {level + 2, {}, {}, kChar, kData, 1, 0}};
}

std::vector<Node> textureNodes()
{
    std::vector<Node> n = {{0, "Texture2D", "Base", 0, 0, -1, 0}};
    auto add = [&](const std::vector<Node> &more) { n.insert(n.end(), more.begin(), more.end()); };
    add(stringNode(1, kName));
    add({{1, {}, "m_Width", kInt, 0, 4, 0}, {1, {}, "m_Height", kInt, 0, 4, 0}, {1, {}, "m_TextureFormat", kInt, 0, 4, 0}});
    add({{1, {}, "m_PlatformBlob", kVector, 0, -1, 0},
         {2, {}, {}, kArray, kArray, -1, 0x4000},
         {3, {}, {}, kInt, kSize, 4, 0},
         {3, {}, {}, kUInt8, kData, 1, 0}});
    add({{1, {}, "image data", kTypeless, 0, -1, 0x4000}, {2, {}, {}, kInt, kSize, 4, 0}, {2, {}, {}, kUInt8, kData, 1, 0}});
    add({{1, "StreamingInfo", "m_StreamData", 0, 0, -1, 0},
         {2, {}, "offset", kUInt64, 0, 8, 0},
         {2, {}, "size", kUInt, 0, 4, 0}});
    add(stringNode(2, 0, "path"));
    return n;
}

std::vector<Node> textAssetNodes()
{
    std::vector<Node> n = {{0, "TextAsset", "Base", 0, 0, -1, 0}};
    for (const auto &more : {stringNode(1, kName), stringNode(1, 0, "m_Script")})
        n.insert(n.end(), more.begin(), more.end());
    return n;
}

QByteArray alignedString(const QByteArray &s)
{
    return Bytes().le<qint32>(qint32(s.size())).raw(s).align(4).data;
}

QByteArray textureObject(const QByteArray &name, int width, int height, int format, const QByteArray &blob,
                         const QByteArray &inlinePixels, quint64 offset, quint32 size, const QByteArray &path)
{
    Bytes o;
    o.raw(alignedString(name)).le<qint32>(width).le<qint32>(height).le<qint32>(format);
    o.le<qint32>(qint32(blob.size())).raw(blob).align(4);
    o.le<qint32>(qint32(inlinePixels.size())).raw(inlinePixels).align(4);
    o.le<quint64>(offset).le<quint32>(size).raw(alignedString(path));
    return o.data;
}

QByteArray textAssetObject(const QByteArray &name, const QByteArray &text)
{
    return alignedString(name) + alignedString(text);
}

struct Object
{
    qint64 pathId;
    int typeIndex;
    QByteArray data;
};

QByteArray serializedFile(qint32 platform, const std::vector<std::pair<int, std::vector<Node>>> &types,
                          const std::vector<Object> &objects, bool withTypeTrees = true)
{
    Bytes meta;
    meta.cstr("2021.3.15f1").le<qint32>(platform).le<quint8>(withTypeTrees).le<qint32>(qint32(types.size()));
    for (const auto &[classId, nodes] : types) {
        meta.le<qint32>(classId).le<quint8>(0).le<qint16>(-1).raw(QByteArray(16, '\0'));
        if (withTypeTrees)
            meta.raw(typeTree(nodes)).le<qint32>(0);
    }
    meta.le<qint32>(qint32(objects.size()));
    const qsizetype headerSize = 0x30;
    std::vector<qint64> starts;
    qint64 cursor = 0;
    for (const Object &o : objects) {
        starts.push_back(cursor);
        cursor = (cursor + o.data.size() + 7) / 8 * 8;
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        meta.align(4);
        meta.le<qint64>(objects[i].pathId).le<qint64>(starts[i]).le<quint32>(quint32(objects[i].data.size()))
            .le<qint32>(objects[i].typeIndex);
    }
    meta.le<qint32>(0).le<qint32>(0).le<qint32>(0).cstr("");
    const qint64 dataOffset = (headerSize + meta.data.size() + 15) / 16 * 16;
    Bytes body;
    for (size_t i = 0; i < objects.size(); ++i) {
        while (body.data.size() < starts[i])
            body.data += '\0';
        body.raw(objects[i].data);
    }
    Bytes file;
    file.be<quint32>(0).be<quint32>(0).be<quint32>(22).be<quint32>(0).le<quint8>(0).raw(QByteArray(3, '\0'));
    file.be<quint32>(quint32(meta.data.size())).be<qint64>(dataOffset + body.data.size()).be<qint64>(dataOffset).be<qint64>(0);
    file.raw(meta.data);
    while (file.data.size() < dataOffset)
        file.data += '\0';
    file.raw(body.data);
    return file.data;
}

QByteArray lz4Literals(const QByteArray &input)
{
    QByteArray out;
    const qsizetype n = input.size();
    out += char(std::min<qsizetype>(n, 15) << 4);
    if (n >= 15) {
        qsizetype rest = n - 15;
        while (rest >= 255) {
            out += char(255);
            rest -= 255;
        }
        out += char(rest);
    }
    return out + input;
}

QByteArray bundle(const std::vector<std::pair<QByteArray, QByteArray>> &nodes, qsizetype blockSize = 0)
{
    QByteArray payload;
    for (const auto &node : nodes)
        payload += node.second;
    std::vector<QByteArray> pieces;
    for (qsizetype at = 0; at < payload.size() || pieces.empty(); at += blockSize > 0 ? blockSize : payload.size() + 1)
        pieces.push_back(payload.mid(at, blockSize > 0 ? blockSize : payload.size()));
    Bytes list;
    list.raw(QByteArray(16, '\0')).be<qint32>(qint32(pieces.size()));
    QByteArray blocks;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const bool compressed = i % 2 == 0;
        const QByteArray stored = compressed ? lz4Literals(pieces[i]) : pieces[i];
        list.be<quint32>(quint32(pieces[i].size())).be<quint32>(quint32(stored.size())).be<quint16>(compressed ? 2 : 0);
        blocks += stored;
    }
    list.be<qint32>(qint32(nodes.size()));
    qint64 at = 0;
    for (const auto &node : nodes) {
        list.be<qint64>(at).be<qint64>(node.second.size()).be<quint32>(4).cstr(node.first);
        at += node.second.size();
    }
    const QByteArray info = lz4Literals(list.data);
    Bytes file;
    file.cstr("UnityFS").be<quint32>(8).cstr("5.x.x").cstr("2021.3.15f1");
    file.be<qint64>(0).be<quint32>(quint32(info.size())).be<quint32>(quint32(list.data.size())).be<quint32>(0x42);
    file.align(16).raw(info).raw(blocks);
    return file.data;
}

QByteArray rgba(const std::vector<quint32> &pixels)
{
    Bytes b;
    for (quint32 p : pixels)
        b.be<quint32>(p);
    return b.data;
}

UValue intValue(const char *name, qint64 v)
{
    UValue out;
    out.kind = UValue::Kind::Int;
    out.name = QString::fromLatin1(name);
    out.i = v;
    return out;
}

UValue structValue(const char *name, std::vector<UValue> children)
{
    UValue out;
    out.kind = UValue::Kind::Struct;
    out.name = QString::fromLatin1(name);
    out.children = std::move(children);
    return out;
}

UValue bytesValue(const char *name, const QByteArray &bytes)
{
    UValue out;
    out.kind = UValue::Kind::Bytes;
    out.name = QString::fromLatin1(name);
    out.bytes = bytes;
    out.elementSize = 1;
    return out;
}

QByteArray le16(std::initializer_list<quint16> values)
{
    QByteArray out;
    for (quint16 v : values) {
        char b[2];
        qToLittleEndian(v, b);
        out.append(b, 2);
    }
    return out;
}

QByteArray leFloats(std::initializer_list<float> values)
{
    QByteArray out;
    for (float v : values) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    }
    return out;
}

UValue meshValue()
{
    std::vector<UValue> channels(14);
    auto channel = [](int stream, int offset, int format, int dimension) {
        return structValue("data", {intValue("stream", stream), intValue("offset", offset), intValue("format", format),
                                    intValue("dimension", dimension)});
    };
    for (auto &c : channels)
        c = channel(0, 0, 0, 0);
    channels[0] = channel(0, 0, 0, 3);
    channels[4] = channel(0, 12, 1, 2);
    channels[3] = channel(1, 0, 2, 4);
    QByteArray stream0;
    const float corners[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (int v = 0; v < 5; ++v) {
        const float *c = corners[v % 4];
        stream0 += leFloats({c[0] + (v == 4 ? 5.f : 0.f), c[1], c[2]});
        stream0 += le16({quint16(v == 2 ? 0x3C00 : 0x3800), 0x0000});
    }
    QByteArray data = stream0 + QByteArray((16 - stream0.size() % 16) % 16, '\0');
    for (int v = 0; v < 5; ++v)
        data += QByteArray("\xff\x00\x80\xff", 4);
    UValue a;
    a.kind = UValue::Kind::Array;
    a.name = QStringLiteral("m_Channels");
    a.children = std::move(channels);
    UValue subs;
    subs.kind = UValue::Kind::Array;
    subs.name = QStringLiteral("m_SubMeshes");
    subs.children = {
        structValue("data", {intValue("firstByte", 0), intValue("indexCount", 6), intValue("topology", 0),
                             intValue("baseVertex", 0), intValue("firstVertex", 0), intValue("vertexCount", 4)}),
        structValue("data", {intValue("firstByte", 12), intValue("indexCount", 4), intValue("topology", 2),
                             intValue("baseVertex", 1), intValue("firstVertex", 1), intValue("vertexCount", 4)}),
        structValue("data", {intValue("firstByte", 20), intValue("indexCount", 2), intValue("topology", 3),
                             intValue("baseVertex", 0), intValue("firstVertex", 0), intValue("vertexCount", 2)}),
    };
    UValue mesh = structValue("Base", {});
    UValue name;
    name.kind = UValue::Kind::String;
    name.name = QStringLiteral("m_Name");
    name.bytes = "quad";
    mesh.children = {
        name,
        std::move(subs),
        intValue("m_IndexFormat", 0),
        bytesValue("m_IndexBuffer", le16({0, 1, 2, 0, 2, 3, 0, 1, 2, 3, 0, 1})),
        structValue("m_VertexData", {intValue("m_VertexCount", 5), std::move(a), bytesValue("m_DataSize", data)}),
    };
    return mesh;
}

UValue floatValue(const char *name, double v)
{
    UValue out;
    out.kind = UValue::Kind::Float;
    out.name = QString::fromLatin1(name);
    out.f = v;
    return out;
}

UValue stringValue(const char *name, const QByteArray &text)
{
    UValue out;
    out.kind = UValue::Kind::String;
    out.name = QString::fromLatin1(name);
    out.bytes = text;
    return out;
}

UValue arrayValue(const char *name, std::vector<UValue> items)
{
    UValue out;
    out.kind = UValue::Kind::Array;
    out.name = QString::fromLatin1(name);
    out.children = std::move(items);
    return out;
}

UValue pointer(const char *name, qint64 pathId)
{
    return structValue(name, {intValue("m_FileID", 0), intValue("m_PathID", pathId)});
}

UValue vector3Value(const char *name, float x, float y, float z)
{
    return structValue(name, {floatValue("x", x), floatValue("y", y), floatValue("z", z)});
}

UValue matrixValue(const std::array<float, 16> &rowMajor)
{
    std::vector<UValue> e;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            UValue v = floatValue("", rowMajor[size_t(r * 4 + c)]);
            v.name = QStringLiteral("e%1%2").arg(r).arg(c);
            e.push_back(v);
        }
    return structValue("data", std::move(e));
}

UValue transformValue(qint64 gameObject, qint64 father, std::vector<qint64> children, std::array<float, 3> position)
{
    std::vector<UValue> kids;
    for (qint64 c : children)
        kids.push_back(pointer("data", c));
    return structValue("Base", {pointer("m_GameObject", gameObject),
                                structValue("m_LocalRotation", {floatValue("x", 0), floatValue("y", 0), floatValue("z", 0),
                                                                floatValue("w", 1)}),
                                vector3Value("m_LocalPosition", position[0], position[1], position[2]),
                                vector3Value("m_LocalScale", 1, 1, 1), arrayValue("m_Children", std::move(kids)),
                                pointer("m_Father", father)});
}

UValue gameObjectValue(const char *name, std::vector<qint64> components)
{
    std::vector<UValue> list;
    for (qint64 c : components)
        list.push_back(structValue("data", {pointer("component", c)}));
    return structValue("Base", {arrayValue("m_Component", std::move(list)), stringValue("m_Name", name)});
}

quint32 crc(const QByteArray &text)
{
    return quint32(crc32(0, reinterpret_cast<const Bytef *>(text.constData()), uInt(text.size())));
}

QByteArray streamedFrames(const std::vector<std::pair<float, std::vector<float>>> &frames)
{
    QByteArray out;
    auto f32 = [&out](float v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    auto i32 = [&out](qint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    f32(-std::numeric_limits<float>::max());
    i32(0);
    for (const auto &[time, values] : frames) {
        f32(time);
        i32(qint32(values.size()));
        for (size_t k = 0; k < values.size(); ++k) {
            i32(qint32(k));
            f32(0);
            f32(0);
            f32(0);
            f32(values[k]);
        }
    }
    f32(std::numeric_limits<float>::max());
    i32(0);
    return out;
}

UValue clipValue(const char *name, quint32 path, const std::vector<std::pair<float, std::vector<float>>> &frames)
{
    UValue streamed = bytesValue("data", streamedFrames(frames));
    streamed.elementSize = 4;
    streamed.elementType = QStringLiteral("unsigned int");
    auto binding = [path](int attribute) {
        return structValue("data", {intValue("path", path), intValue("attribute", attribute), intValue("typeID", 4)});
    };
    return structValue(
        "Base",
        {stringValue("m_Name", name), intValue("m_Legacy", 0),
         structValue("m_MuscleClip",
                     {structValue("m_Clip", {structValue("data", {structValue("m_StreamedClip", {streamed, intValue("curveCount", 7)}),
                                                                  structValue("m_DenseClip", {intValue("m_FrameCount", 0),
                                                                                              intValue("m_CurveCount", 0)})})}),
                      floatValue("m_StopTime", 1)}),
         structValue("m_ClipBindingConstant", {arrayValue("genericBindings", {binding(1), binding(2)})})});
}

void encodeNode(const std::vector<TypeTreeNode> &nodes, int index, Bytes &out, const QHash<QString, QVariant> &values)
{
    const TypeTreeNode &node = nodes[size_t(index)];
    const bool hasArray = !node.children.empty() && nodes[size_t(node.children.front())].type == "Array";
    if (hasArray || node.type == "TypelessData") {
        const QByteArray bytes = values.value(node.name).toByteArray();
        out.le<qint32>(qint32(bytes.size())).raw(bytes);
        if (hasArray && nodes[size_t(node.children.front())].aligned())
            out.align(4);
    } else if (node.children.empty()) {
        const qint64 v = values.value(node.name).toLongLong();
        if (node.byteSize == 8)
            out.le<qint64>(v);
        else if (node.byteSize == 2)
            out.le<qint16>(qint16(v));
        else if (node.byteSize == 1)
            out.le<qint8>(qint8(v));
        else
            out.le<qint32>(qint32(v));
    } else {
        for (int child : node.children)
            encodeNode(nodes, child, out, values);
    }
    if (node.aligned())
        out.align(4);
}

}

class TestUnity : public QObject
{
    Q_OBJECT

    std::shared_ptr<Document> open(const QByteArray &data, const QString &name, QString *reader = nullptr)
    {
        QString error;
        const FormatReader *used = nullptr;
        auto doc = m_registry.openBytes(data, name, &used, &error);
        if (reader)
            *reader = used ? used->name() : QString();
        if (!doc)
            qWarning() << error;
        return doc;
    }

    std::shared_ptr<Document> enter(const Document &doc, const QString &entry)
    {
        for (const auto &table : doc.tables)
            if (auto archive = table->archive())
                if (const int i = archive->find(entry); i >= 0)
                    return open(archive->entryData(size_t(i)), entry);
        return nullptr;
    }

    FormatRegistry m_registry;

private slots:
    void decodesLz4Blocks()
    {
        const QByteArray block = QByteArray::fromHex("4461626364040010") + "x";
        QCOMPARE(lz4BlockDecompress(block, 13), std::optional<QByteArray>("abcdabcdabcdx"));
        QCOMPARE(lz4BlockDecompress(block, 12), std::nullopt);
        QCOMPARE(lz4BlockDecompress(QByteArray::fromHex("4461626364000010") + "x", 13), std::nullopt);
        const QByteArray big(300, 'q');
        QCOMPARE(lz4BlockDecompress(lz4Literals(big), big.size()), std::optional<QByteArray>(big));
    }

    void readsBundlesAndTheirObjects()
    {
        const QByteArray bottomRow = rgba({0xFF0000FF, 0x00FF00FF}), topRow = rgba({0x0000FFFF, 0xFFFFFFFF});
        const QByteArray resource = QByteArray("pad!") + bottomRow + topRow;
        const QByteArray tex = textureObject("tex", 2, 2, 4, {}, {}, 4, 16, "archive:/CAB-test/CAB-test.resS");
        const QByteArray file = serializedFile(
            5, {{28, textureNodes()}, {49, textAssetNodes()}},
            {{10, 0, tex},
             {11, 1, textAssetObject("notes", "hello unity")},
             {12, 1, textAssetObject("notes", "second")}});
        const QByteArray data = bundle({{"CAB-test", file}, {"CAB-test.resS", resource}});

        QString reader;
        auto doc = open(data, "test.bundle", &reader);
        QVERIFY(doc);
        QCOMPARE(reader, QString("Unity asset bundle"));
        const auto &list = doc->tables.front();
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(list->text(0, 0, DisplayMode::Auto), QString("tex.Texture2D"));
        QCOMPARE(list->text(1, 0, DisplayMode::Auto), QString("notes.TextAsset"));
        QCOMPARE(list->text(2, 0, DisplayMode::Auto), QString("notes #12.TextAsset"));
        QVERIFY2(list->description().contains("2021.3.15f1"), qPrintable(list->description()));
        QCOMPARE(list->sortKey(0, 4).toLongLong(), tex.size() + 16);

        auto texture = enter(*doc, "tex.Texture2D");
        QVERIFY(texture);
        QCOMPARE(texture->tables.front()->name(), QString("Texture"));
        const auto image = texture->tables.front()->image();
        QVERIFY(image);
        QCOMPARE(image->width, 2);
        QCOMPARE(image->rgba, topRow + bottomRow);
        const auto &fields = texture->tables.back();
        QVERIFY(fields->canExportJson());
        const QByteArray json = fields->exportJson();
        QVERIFY2(json.contains("\"m_Name\": \"tex\"") && json.contains("\"m_Width\": 2"), json.constData());

        auto text = enter(*doc, "notes.TextAsset");
        QVERIFY(text);
        auto archive = text->tables.front()->archive();
        QVERIFY(archive);
        QCOMPARE(archive->entries.front().name, QString("notes.txt"));
        QCOMPARE(archive->entryData(0), QByteArray("hello unity"));
        QCOMPARE(enter(*doc, "notes #12.TextAsset")->tables.front()->archive()->entryData(0), QByteArray("second"));

        const auto objects = list->archive();
        QVERIFY(objects->extractor);
        const std::optional<ExtractedFile> png = objects->extractor(0);
        QVERIFY(png);
        QCOMPARE(png->name, QString("tex.png"));
        QVERIFY(png->data.startsWith("\x89PNG"));
        const std::optional<ExtractedFile> notes = objects->extractor(1);
        QVERIFY(notes);
        QCOMPARE(notes->name, QString("notes.txt"));
        QCOMPARE(notes->data, QByteArray("hello unity"));
    }

    void readsBundleBlocksOnlyWhenNeeded()
    {
        QByteArray pixels;
        for (int i = 0; i < 8 * 8; ++i)
            pixels += rgba({0x10203000u + quint32(i)});
        const QByteArray resource = QByteArray(37, 'x') + pixels;
        const QByteArray tex = textureObject("tex", 8, 8, 4, {}, {}, 37, pixels.size(), "archive:/CAB-big/CAB-big.resS");
        const QByteArray file = serializedFile(5, {{28, textureNodes()}}, {{10, 0, tex}});
        const QByteArray data = bundle({{"CAB-big", file}, {"CAB-big.resS", resource}}, 23);

        const std::optional<UnityBundle> parsed = readUnityBundle(data, nullptr);
        QVERIFY(parsed);
        QVERIFY(parsed->blocks.size() > 10);
        QCOMPARE(parsed->nodeData(0).value_or(QByteArray()), file);
        QCOMPARE(parsed->read(1, 30, 50).value_or(QByteArray()), resource.mid(30, 50));
        QVERIFY(!parsed->read(1, resource.size() - 2, 5));

        QTemporaryDir dir;
        const QString path = dir.filePath("big.bundle");
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(data);
        out.close();

        QString error;
        auto doc = std::shared_ptr<Document>(UnityBundleReader().read(path, &error));
        QVERIFY2(doc, qPrintable(error));
        auto texture = enter(*doc, "tex.Texture2D");
        QVERIFY(texture);
        auto image = texture->tables.front()->image();
        QVERIFY(image);
        QCOMPARE(image->width, 8);
        QByteArray flipped;
        for (int row = 7; row >= 0; --row)
            flipped += pixels.mid(row * 8 * 4, 8 * 4);
        QCOMPARE(image->rgba, flipped);

        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QByteArray(data.size() + 100, '\0'));
        out.close();
        auto reopened = enter(*doc, "tex.Texture2D");
        if (reopened) {
            const auto changed = reopened->tables.front()->image();
            QVERIFY(!changed || changed->rgba != flipped);
        }
    }

    void buildsModelsFromMeshes()
    {
        QString error;
        const auto model = unityMeshModel(meshValue(), {}, "2021.3.15f1", false, &error);
        QVERIFY2(model, qPrintable(error));
        QCOMPARE(model->primitives.size(), size_t(2));
        const ModelPrimitive &quad = model->primitives[0];
        QCOMPARE(quad.vertexCount(), qsizetype(4));
        QCOMPARE(quad.positions[3], -1.f);
        QCOMPARE(quad.positions[4], 0.f);
        QCOMPARE(quad.indices, (std::vector<quint32>{0, 2, 1, 0, 3, 2}));
        QCOMPARE(quad.uv0[0], 0.5f);
        QCOMPARE(quad.uv0[4], 1.f);
        QCOMPARE(quad.colors.size(), size_t(16));
        QCOMPARE(quad.colors[0], quint8(255));
        QCOMPARE(quad.colors[2], quint8(128));

        const ModelPrimitive &shifted = model->primitives[1];
        QCOMPARE(shifted.vertexCount(), qsizetype(4));
        QCOMPARE(shifted.positions[9], -5.f);
        QCOMPARE(shifted.indices, (std::vector<quint32>{0, 2, 1, 0, 3, 2}));

        UValue streamed = meshValue();
        QByteArray external;
        for (UValue &c : streamed.children)
            if (c.name == "m_VertexData")
                for (UValue &d : c.children)
                    if (d.name == "m_DataSize")
                        external = std::exchange(d.bytes, QByteArray());
        QVERIFY(!unityMeshModel(streamed, {}, "2021.3.15f1", false, &error));
        QVERIFY2(error.contains("missing"), qPrintable(error));
        QVERIFY(unityMeshModel(streamed, external, "2021.3.15f1", false, &error));

        UValue huge = meshValue();
        for (UValue &c : huge.children)
            if (c.name == "m_VertexData")
                for (UValue &d : c.children)
                    if (d.name == "m_VertexCount")
                        d.i = qint64(1) << 40;
        QVERIFY(!unityMeshModel(huge, {}, "2021.3.15f1", false, &error));
    }

    void buildsSkinnedModelsWithMaterialsAndAnimations()
    {
        UValue mesh = meshValue();
        for (UValue &c : mesh.children)
            if (c.name == "m_VertexData")
                for (UValue &v : c.children) {
                    if (v.name == "m_Channels") {
                        auto channel = [](int stream, int offset, int format, int dimension) {
                            return structValue("data", {intValue("stream", stream), intValue("offset", offset),
                                                        intValue("format", format), intValue("dimension", dimension)});
                        };
                        v.children[12] = channel(2, 0, 0, 1);
                        v.children[13] = channel(2, 4, 10, 1);
                    } else if (v.name == "m_DataSize") {
                        v.bytes += QByteArray(112 - v.bytes.size(), '\0');
                        for (int i = 0; i < 5; ++i) {
                            v.bytes += leFloats({1.f});
                            char b[4];
                            qToLittleEndian<quint32>(quint32(i % 2), b);
                            v.bytes.append(b, 4);
                        }
                    }
                }
        mesh.children.push_back(arrayValue("m_BindPose", {matrixValue({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}),
                                                          matrixValue({1, 0, 0, -2, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})}));

        UnityScene scene;
        scene.unityVersion = QStringLiteral("2021.3.15f1");
        scene.objects = {
            {0, 1, unityclass::GameObject, gameObjectValue("hero", {2}), {}},
            {0, 2, unityclass::Transform, transformValue(1, 0, {4}, {0, 0, 0}), {}},
            {0, 3, unityclass::GameObject, gameObjectValue("arm", {4, 5}), {}},
            {0, 4, unityclass::Transform, transformValue(3, 2, {}, {2, 1, 0}), {}},
            {0, 5, unityclass::SkinnedMeshRenderer,
             structValue("Base", {pointer("m_GameObject", 3), arrayValue("m_Materials", {pointer("data", 7)}),
                                  pointer("m_Mesh", 6), arrayValue("m_Bones", {pointer("data", 2), pointer("data", 4)})}),
             {}},
            {0, 6, unityclass::Mesh, mesh, {}},
            {0, 7, unityclass::Material,
             structValue("Base", {stringValue("m_Name", "skin"),
                                  structValue("m_SavedProperties",
                                              {arrayValue("m_TexEnvs", {structValue("data", {stringValue("first", "Texture2D_ABCD"),
                                                                                             structValue("second", {pointer("m_Texture", 8)})}),
                                                                        structValue("data", {stringValue("first", "Texture2D_EF01"),
                                                                                             structValue("second", {pointer("m_Texture", 9)})})})})}),
             {}},
            {0, 8, unityclass::Texture2D, structValue("Base", {stringValue("m_Name", "hero_nml")}), {}},
            {0, 9, unityclass::Texture2D, structValue("Base", {stringValue("m_Name", "hero")}), {}},
            {0, 10, unityclass::AnimationClip,
             clipValue("wave", crc("arm"), {{0.f, {1, 2, 3, 0, 0, 0, 1}}, {0.5f, {1, 4, 3, 0, 0.7071f, 0, 0.7071f}}}), {}},
            {0, 11, unityclass::AnimationClip, clipValue("other", crc("tail"), {{0.f, {1, 2, 3, 0, 0, 0, 1}}}), {}},
        };
        scene.buildIndex();
        UnitySceneSummary summary;
        QString error;
        const auto model = unitySceneModel(scene, 0, 1, &summary, &error);
        QVERIFY2(model, qPrintable(error));
        QCOMPARE(model->nodes.size(), size_t(2));
        QCOMPARE(model->nodes[0].name, QString("hero"));
        QCOMPARE(model->nodes[1].name, QString("arm"));
        QCOMPARE(model->nodes[1].parent, 0);
        QCOMPARE(model->nodes[1].translation[0], -2.f);
        QCOMPARE(summary.skinned, 1);
        QCOMPARE(model->meshes.size(), size_t(1));
        QCOMPARE(model->meshes[0].joints, (std::vector<int>{0, 1}));
        QCOMPARE(model->meshes[0].inverseBinds.size(), size_t(2));
        QCOMPARE(model->meshes[0].inverseBinds[1][12], 2.f);
        const ModelPrimitive &prim = model->primitives.front();
        QCOMPARE(prim.joints.size(), size_t(prim.vertexCount()) * 4);
        QCOMPARE(prim.joints[4], quint16(1));
        QCOMPARE(prim.weights[0], 1.f);
        QCOMPARE(prim.weights[1], 0.f);
        QVERIFY(prim.material >= 0);
        const ModelMaterial &material = model->materials[size_t(prim.material)];
        QCOMPARE(material.name, QString("skin"));
        QVERIFY(material.baseColorTexture >= 0 && material.normalTexture >= 0);
        QCOMPARE(model->textures[size_t(material.baseColorTexture)].name, QString("hero"));
        QCOMPARE(model->textures[size_t(material.normalTexture)].name, QString("hero_nml"));
        QCOMPARE(summary.animations, 1);
        QCOMPARE(summary.skippedAnimations, 1);
        QCOMPARE(model->animations.front().name, QString("wave"));
        const auto &channels = model->animations.front().channels;
        QCOMPARE(channels.size(), size_t(2));
        const AnimationChannel &move = channels[0].path == AnimationPath::Translation ? channels[0] : channels[1];
        const AnimationChannel &turn = channels[0].path == AnimationPath::Translation ? channels[1] : channels[0];
        QCOMPARE(move.node, 1);
        QCOMPARE(move.times, (std::vector<float>{0.f, 0.5f}));
        QCOMPARE(move.values, (std::vector<float>{-1, 2, 3, -1, 4, 3}));
        QCOMPARE(turn.values.size(), size_t(8));
        QVERIFY(std::abs(turn.values[5] + 0.7071f) < 1e-3f);

        const QByteArray glb = writeGlb(*model, {});
        const quint32 jsonLength = qFromLittleEndian<quint32>(glb.constData() + 12);
        const QJsonObject gltf = QJsonDocument::fromJson(glb.mid(20, jsonLength)).object();
        QCOMPARE(gltf.value("animations").toArray().size(), 1);
        QCOMPARE(gltf.value("animations").toArray().first().toObject().value("name").toString(), QString("wave"));
        const QJsonObject skin = gltf.value("skins").toArray().first().toObject();
        QCOMPARE(skin.value("joints").toArray().size(), 2);
        const QJsonObject ibm = gltf.value("accessors").toArray().at(skin.value("inverseBindMatrices").toInt()).toObject();
        QCOMPARE(ibm.value("count").toInt(), 2);
    }

    void findsTexturesByNameWhenMaterialsLeaveThemOut()
    {
        auto texturesOf = [](const QString &materialName, const QStringList &textures) {
            UnityScene scene;
            scene.unityVersion = QStringLiteral("2021.3.15f1");
            scene.objects = {
                {0, 1, unityclass::GameObject, gameObjectValue("prop", {2, 3, 4}), {}},
                {0, 2, unityclass::Transform, transformValue(1, 0, {}, {0, 0, 0}), {}},
                {0, 3, unityclass::MeshFilter, structValue("Base", {pointer("m_GameObject", 1), pointer("m_Mesh", 5)}), {}},
                {0, 4, unityclass::MeshRenderer,
                 structValue("Base", {pointer("m_GameObject", 1), arrayValue("m_Materials", {pointer("data", 6)})}), {}},
                {0, 5, unityclass::Mesh, meshValue(), {}},
                {0, 6, unityclass::Material,
                 structValue("Base", {stringValue("m_Name", materialName.toUtf8()),
                                      structValue("m_SavedProperties",
                                                  {arrayValue("m_TexEnvs", {structValue("data", {stringValue("first", "_BaseMap"),
                                                                                                 structValue("second", {pointer("m_Texture", 0)})})})})}),
                 {}},
            };
            for (qsizetype i = 0; i < textures.size(); ++i)
                scene.objects.push_back({0, 10 + i, unityclass::Texture2D, structValue("Base", {stringValue("m_Name", textures[i].toUtf8())}), {}});
            scene.buildIndex();
            UnitySceneSummary summary;
            QString error;
            const auto model = unitySceneModel(scene, 0, 1, &summary, &error);
            QStringList out;
            if (!model || model->primitives.empty() || model->primitives.front().material < 0)
                return out;
            const ModelMaterial &m = model->materials[size_t(model->primitives.front().material)];
            out << (m.baseColorTexture >= 0 ? model->textures[size_t(m.baseColorTexture)].name : QString())
                << (m.normalTexture >= 0 ? model->textures[size_t(m.normalTexture)].name : QString());
            return out;
        };
        QCOMPARE(texturesOf("p0001_mdl_mario_new_p0001_base", {"p0001_mass", "p0001_nml", "p0001", "p0001_arm_spread", "p0001_emm"}),
                 (QStringList{"p0001", "p0001_nml"}));
        QCOMPARE(texturesOf("p0050_mdl_p0050_base", {"p0059_mass", "p0059", "p0059_nml"}), (QStringList{"p0059", "p0059_nml"}));
        QCOMPARE(texturesOf("p0269_body", {"p0269a", "p0269b", "p0269a_nml"}), (QStringList{QString(), QString()}));
        QVERIFY(unityNameContains("p0063_game_body", "p0063_game"));
        QVERIFY(!unityNameContains("p00630_body", "p0063"));
    }

    void readsSwitchTexturesStoredWithOneGobPerBlock()
    {
        QByteArray linear;
        for (int i = 0; i < 16 * 16; ++i)
            linear += rgba({quint32(i) << 8 | 0xFF});
        const QByteArray swizzled = *swizzleBlockLinear(linear, 16, 16, 4, 1);
        QVERIFY(swizzled != linear);
        const QByteArray blob = Bytes().le<quint32>(0).le<quint32>(32).le<quint32>(0).data;
        const QByteArray file = serializedFile(38, {{28, textureNodes()}},
                                               {{1, 0, textureObject("sw", 16, 16, 4, blob, swizzled, 0, 0, {})}});
        auto doc = open(file, "sharedassets1.assets");
        QVERIFY(doc);
        auto texture = enter(*doc, "sw.Texture2D");
        QVERIFY(texture);
        const auto image = texture->tables.front()->image();
        QVERIFY(image);
        QByteArray flipped;
        for (int y = 15; y >= 0; --y)
            flipped += linear.mid(y * 64, 64);
        QCOMPARE(image->rgba, flipped);
    }

    void readsAssetsFilesWithPixelsInASiblingFile()
    {
        QTemporaryDir dir;
        const QByteArray pixels = rgba({0x11223344, 0x55667788});
        QFile res(dir.filePath("level1.resS"));
        QVERIFY(res.open(QIODevice::WriteOnly));
        res.write(QByteArray(8, 'x') + pixels);
        res.close();
        const QByteArray file =
            serializedFile(5, {{28, textureNodes()}}, {{3, 0, textureObject("wide", 2, 1, 4, {}, {}, 8, 8, "level1.resS")}});
        QFile f(dir.filePath("level1"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(file);
        f.close();
        QString error;
        const FormatReader *used = nullptr;
        auto doc = m_registry.open(dir.filePath("level1"), &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), QString("Unity serialized file"));
        auto texture = enter(*doc, "wide.Texture2D");
        QVERIFY(texture);
        QCOMPARE(texture->tables.front()->image()->rgba, pixels);
    }

    void readsFilesWithoutTypeTreesThroughBuiltinLayouts()
    {
        const std::vector<TypeTreeNode> layout = builtinTypeTree(28, "2021.3.15f1");
        QVERIFY(!layout.empty());
        QVERIFY(builtinTypeTree(28, "2019.4.1f1").empty());
        const QByteArray bottom = rgba({0xFF0000FF, 0x00FF00FF}), top = rgba({0x0000FFFF, 0xFFFFFFFF});
        Bytes object;
        encodeNode(layout, 0, object,
                   {{"m_Name", QByteArray("loose")}, {"m_Width", 2}, {"m_Height", 2}, {"m_TextureFormat", 4},
                    {"m_MipCount", 1}, {"m_ImageCount", 1}, {"m_TextureDimension", 2},
                    {"m_CompleteImageSize", 16}, {"image data", bottom + top}});
        const QByteArray file = serializedFile(5, {{28, {}}, {49, {}}}, {{3, 0, object.data}, {4, 1, "efgh"}}, false);

        auto doc = open(file, "sharedassets0.assets");
        QVERIFY(doc);
        const auto &list = doc->tables.front();
        QVERIFY2(list->description().contains("built-in layouts"), qPrintable(list->description()));
        QCOMPARE(list->text(0, 0, DisplayMode::Auto), QString("loose.Texture2D"));
        auto texture = enter(*doc, "loose.Texture2D");
        QVERIFY(texture);
        const auto image = texture->tables.front()->image();
        QVERIFY(image);
        QCOMPARE(image->rgba, top + bottom);

        Bytes longer = object;
        longer.raw("junk");
        const QByteArray mismatched = serializedFile(5, {{28, {}}}, {{3, 0, longer.data}}, false);
        auto unread = open(mismatched, "sharedassets1.assets");
        QVERIFY(unread);
        QVERIFY(unread->tables.front()->description().contains("cannot be opened"));
    }

    void listsFilesWithoutTypeTrees()
    {
        const QByteArray file = serializedFile(5, {{28, {}}, {49, {}}}, {{3, 0, "abcd"}, {4, 1, "efgh"}}, false);
        QVERIFY(looksLikeSerializedFile(file.first(64)));
        auto doc = open(file, "globalgamemanagers");
        QVERIFY(doc);
        const auto &list = doc->tables.front();
        QCOMPARE(list->rowCount(), 2);
        QCOMPARE(list->text(0, 1, DisplayMode::Auto), QString("Texture2D"));
        QCOMPARE(list->text(1, 1, DisplayMode::Auto), QString("TextAsset"));
        QVERIFY2(list->description().contains("without type information"), qPrintable(list->description()));
        QString reader;
        auto archive = list->archive();
        auto object = open(archive->entryData(0), archive->entries[0].name, &reader);
        QVERIFY(object);
        QCOMPARE(reader, QString("Unity object"));
        QVERIFY2(object->tables.front()->description().contains("type information"),
                 qPrintable(object->tables.front()->description()));
        QCOMPARE(unityClassName(99999), QString("class 99999"));
    }

    void rejectsDamagedBundles()
    {
        const QByteArray file = serializedFile(5, {{49, textAssetNodes()}}, {{1, 0, textAssetObject("a", "b")}});
        QByteArray data = bundle({{"CAB-x", file}});
        QString reader;
        QVERIFY(open(data, "ok.bundle", &reader));
        data.truncate(data.size() - 10);
        QString error;
        QVERIFY(!m_registry.byName("Unity asset bundle")->readBytes(data, "cut.bundle", &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_MAIN(TestUnity)
#include "test_unity.moc"
