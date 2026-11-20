#include "unity/unityreader.h"

#include "core/contentkind.h"
#include "core/simpletable.h"
#include "core/treetable.h"
#include "models/gltfwriter.h"
#include "models/meshpreview.h"
#include "unity/serializedfile.h"
#include "unity/unitybundle.h"
#include "unity/unitymesh.h"
#include "unity/unityscene.h"
#include "unity/unitytexture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace {

constexpr int kTexture2D = 28;
constexpr int kTextAsset = 49;
constexpr int kAssetBundle = 142;
constexpr int kAudioClip = 83;
constexpr int kSprite = 213;
constexpr int kMesh = 43;
constexpr int kGameObject = 1;
constexpr int kMaxSceneObjects = 50'000;
constexpr int kMaxSceneClips = 2'000;
constexpr int kMaxNamedTextures = 64;
constexpr int kMaxFieldRows = 50'000;
constexpr int kMaxArrayRows = 256;

using ResourceLoader = std::function<QByteArray(const QString &path, qint64 offset, qint64 size)>;

struct ResourceRef
{
    QString path;
    qint64 offset = 0;
    qint64 size = 0;
};

std::optional<ResourceRef> resourceOf(const UValue &value)
{
    if (const UValue *s = value.child(u"m_StreamData")) {
        const UValue *path = s->child(u"path"), *offset = s->child(u"offset"), *size = s->child(u"size");
        if (path && offset && size && !path->toString().isEmpty() && size->toInt() > 0)
            return ResourceRef{path->toString(), offset->toInt(), size->toInt()};
    }
    for (const char16_t *field : {u"m_Resource", u"m_ExternalResources"}) {
        if (const UValue *s = value.child(field)) {
            const UValue *path = s->child(u"m_Source"), *offset = s->child(u"m_Offset"), *size = s->child(u"m_Size");
            if (path && offset && size && !path->toString().isEmpty() && size->toInt() > 0)
                return ResourceRef{path->toString(), offset->toInt(), size->toInt()};
        }
    }
    return std::nullopt;
}

QJsonArray nodesToJson(const std::vector<TypeTreeNode> &nodes)
{
    QJsonArray out;
    for (const TypeTreeNode &n : nodes)
        out.append(QJsonArray{n.level, n.type, n.name, n.byteSize, qint64(n.metaFlags)});
    return out;
}

std::vector<TypeTreeNode> nodesFromJson(const QJsonArray &array)
{
    std::vector<TypeTreeNode> nodes;
    std::vector<int> parents;
    for (const QJsonValue &v : array) {
        const QJsonArray a = v.toArray();
        TypeTreeNode node;
        node.level = a.at(0).toInt();
        node.type = a.at(1).toString();
        node.name = a.at(2).toString();
        node.byteSize = a.at(3).toInt();
        node.metaFlags = quint32(a.at(4).toInteger());
        while (!parents.empty() && nodes[size_t(parents.back())].level >= node.level)
            parents.pop_back();
        const int index = int(nodes.size());
        if (!parents.empty())
            nodes[size_t(parents.back())].children.push_back(index);
        else if (index != 0)
            return {};
        nodes.push_back(std::move(node));
        parents.push_back(index);
    }
    return nodes;
}

QByteArray packObject(const SerializedFile &file, const SerializedObject &object, const QString &label,
                      const QString &container, const QByteArray &resource, const QByteArray &linked,
                      const QJsonObject &extra = {})
{
    const SerializedType *type = file.typeOf(object);
    const QByteArray data = file.objectData(object);
    QJsonObject header{
        {QStringLiteral("unityVersion"), file.unityVersion},
        {QStringLiteral("platform"), file.platform},
        {QStringLiteral("bigEndian"), file.bigEndian},
        {QStringLiteral("classId"), type ? type->classId : 0},
        {QStringLiteral("pathId"), QString::number(object.pathId)},
        {QStringLiteral("file"), label},
        {QStringLiteral("container"), container},
        {QStringLiteral("nodes"), type ? nodesToJson(type->nodes) : QJsonArray()},
        {QStringLiteral("sizes"), QJsonArray{qint64(data.size()), qint64(resource.size()), qint64(linked.size())}},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it)
        header.insert(it.key(), it.value());
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out = UnityObjectReader::kMagic.toByteArray();
    out.append('\0');
    char length[4];
    qToLittleEndian<quint32>(quint32(json.size()), length);
    out.append(length, 4);
    out += json;
    out += data;
    out += resource;
    out += linked;
    return out;
}

struct UnpackedObject
{
    QJsonObject header;
    std::vector<TypeTreeNode> nodes;
    QByteArray data;
    QByteArray resource;
    QByteArray linked;
};

std::optional<UnpackedObject> unpackObject(const QByteArray &blob)
{
    const qsizetype start = UnityObjectReader::kMagic.size() + 1;
    if (blob.size() < start + 4 || !blob.startsWith(UnityObjectReader::kMagic))
        return std::nullopt;
    const quint32 length = qFromLittleEndian<quint32>(blob.constData() + start);
    if (length > quint32(blob.size() - start - 4))
        return std::nullopt;
    UnpackedObject out;
    out.header = QJsonDocument::fromJson(blob.mid(start + 4, length)).object();
    const QJsonArray sizes = out.header.value(QStringLiteral("sizes")).toArray();
    qsizetype at = start + 4 + length;
    QByteArray *parts[] = {&out.data, &out.resource, &out.linked};
    for (int i = 0; i < 3; ++i) {
        const qint64 size = sizes.at(i).toInteger();
        if (size < 0 || size > blob.size() - at)
            return std::nullopt;
        *parts[i] = blob.mid(at, size);
        at += size;
    }
    out.nodes = nodesFromJson(out.header.value(QStringLiteral("nodes")).toArray());
    return out;
}

QString scalarText(const UValue &v)
{
    switch (v.kind) {
    case UValue::Kind::Bool: return v.i ? QStringLiteral("true") : QStringLiteral("false");
    case UValue::Kind::Int: return QString::number(v.i);
    case UValue::Kind::UInt: return QString::number(v.u);
    case UValue::Kind::Float: return QString::number(v.f, 'g', 9);
    default: return {};
    }
}

QString primitiveText(const QString &type, int size, const char *p)
{
    if (type == QLatin1String("float")) {
        float f;
        std::memcpy(&f, p, 4);
        return QString::number(f, 'g', 9);
    }
    if (type == QLatin1String("double")) {
        double d;
        std::memcpy(&d, p, 8);
        return QString::number(d, 'g', 17);
    }
    const bool isSigned = type.startsWith(u'S') || type == QLatin1String("int") || type == QLatin1String("short")
                       || type == QLatin1String("long long");
    switch (size) {
    case 1: return isSigned ? QString::number(qint8(*p)) : QString::number(quint8(*p));
    case 2: return isSigned ? QString::number(qFromLittleEndian<qint16>(p)) : QString::number(qFromLittleEndian<quint16>(p));
    case 4: return isSigned ? QString::number(qFromLittleEndian<qint32>(p)) : QString::number(qFromLittleEndian<quint32>(p));
    default: return isSigned ? QString::number(qFromLittleEndian<qint64>(p)) : QString::number(qFromLittleEndian<quint64>(p));
    }
}

QString stringText(const QByteArray &bytes)
{
    const QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter) || text.contains(QChar(0)))
        return QObject::tr("(%n byte(s) of binary data)", nullptr, int(bytes.size()));
    return text;
}

void flatten(const UValue &v, const QString &path, std::vector<TreeRow> &rows)
{
    if (int(rows.size()) >= kMaxFieldRows)
        return;
    TreeRow row;
    row.path = path;
    row.type = v.type;
    switch (v.kind) {
    case UValue::Kind::String:
        row.value = stringText(v.bytes);
        rows.push_back(std::move(row));
        return;
    case UValue::Kind::Bytes: {
        const qint64 count = v.count();
        QStringList preview;
        for (qint64 i = 0; i < std::min<qint64>(count, 16); ++i)
            preview << primitiveText(v.elementType, v.elementSize, v.bytes.constData() + i * v.elementSize);
        row.type = QStringLiteral("%1[]").arg(v.elementType);
        row.value = QObject::tr("%n item(s)", nullptr, int(count))
                  + (preview.isEmpty() ? QString() : QStringLiteral(": ") + preview.join(QStringLiteral(", "))
                                                         + (count > 16 ? QStringLiteral(", …") : QString()));
        rows.push_back(std::move(row));
        return;
    }
    case UValue::Kind::Array: {
        row.type = QStringLiteral("%1[]").arg(v.elementType);
        row.value = QObject::tr("%n item(s)", nullptr, int(v.children.size()));
        rows.push_back(std::move(row));
        const size_t shown = std::min<size_t>(v.children.size(), kMaxArrayRows);
        for (size_t i = 0; i < shown; ++i)
            flatten(v.children[i], QStringLiteral("%1[%2]").arg(path).arg(i), rows);
        if (shown < v.children.size())
            rows.push_back({QStringLiteral("%1[…]").arg(path), {},
                            QObject::tr("%n more item(s) not shown; export as JSON to see all", nullptr,
                                        int(v.children.size() - shown)),
                            {}});
        return;
    }
    case UValue::Kind::Struct:
        if (!path.isEmpty())
            rows.push_back(std::move(row));
        for (const UValue &c : v.children)
            flatten(c, path.isEmpty() ? c.name : path + u'.' + c.name, rows);
        return;
    default:
        row.value = scalarText(v);
        row.sortKey = v.kind == UValue::Kind::Bool ? QVariant() : QVariant(v.toDouble());
        rows.push_back(std::move(row));
        return;
    }
}

void appendJsonString(QByteArray &out, const QString &text)
{
    out += QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact).chopped(1).mid(1);
}

void appendJson(QByteArray &out, const UValue &v, int indent)
{
    const QByteArray pad(indent * 2, ' '), inner((indent + 1) * 2, ' ');
    switch (v.kind) {
    case UValue::Kind::String: appendJsonString(out, QString::fromUtf8(v.bytes)); return;
    case UValue::Kind::Bool: out += v.i ? "true" : "false"; return;
    case UValue::Kind::Int: out += QByteArray::number(v.i); return;
    case UValue::Kind::UInt: out += QByteArray::number(v.u); return;
    case UValue::Kind::Float:
        out += std::isfinite(v.f) ? QByteArray::number(v.f, 'g', 9) : QByteArray("null");
        return;
    case UValue::Kind::Bytes: {
        out += '[';
        for (qint64 i = 0, n = v.count(); i < n; ++i) {
            if (i)
                out += ',';
            const QString t = primitiveText(v.elementType, v.elementSize, v.bytes.constData() + i * v.elementSize);
            out += (t == QLatin1String("nan") || t == QLatin1String("inf") || t == QLatin1String("-inf")) ? "null" : t.toUtf8();
        }
        out += ']';
        return;
    }
    case UValue::Kind::Array:
        if (v.children.empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (size_t i = 0; i < v.children.size(); ++i) {
            out += inner;
            appendJson(out, v.children[i], indent + 1);
            out += i + 1 < v.children.size() ? ",\n" : "\n";
        }
        out += pad + ']';
        return;
    case UValue::Kind::Struct:
        if (v.children.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        for (size_t i = 0; i < v.children.size(); ++i) {
            out += inner;
            appendJsonString(out, v.children[i].name);
            out += ": ";
            appendJson(out, v.children[i], indent + 1);
            out += i + 1 < v.children.size() ? ",\n" : "\n";
        }
        out += pad + '}';
        return;
    default: out += "null"; return;
    }
}

QString sizeText(qint64 bytes) { return QLocale().formattedDataSize(bytes); }

class ArchiveTable : public SimpleTable
{
public:
    ArchiveTable(QString name, std::vector<SimpleColumn> columns, std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(std::move(name), std::move(columns), std::move(rows)), m_archive(std::move(archive))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

class MeshTable : public SimpleTable
{
public:
    MeshTable(std::vector<Row> rows, std::shared_ptr<const Model3D> model, QString title = QObject::tr("Mesh"))
        : SimpleTable(std::move(title), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_model(std::move(model))
    {
        setImageMaker([model = m_model] { return renderPreview(previewMeshes(*model)); });
    }
    bool hasModel() const override { return true; }
    std::shared_ptr<const Model3D> model() const override { return m_model; }

private:
    std::shared_ptr<const Model3D> m_model;
};

std::shared_ptr<SimpleTable> meshTable(const UValue &value, const QByteArray &streamed, const QString &unityVersion,
                                       bool bigEndian)
{
    QString error;
    std::shared_ptr<const Model3D> model = unityMeshModel(value, streamed, unityVersion, bigEndian, &error);
    const UValue *count = value.at(u"m_VertexData.m_VertexCount");
    const UValue *subMeshes = value.child(u"m_SubMeshes");
    qint64 triangles = 0;
    if (model)
        for (const ModelPrimitive &p : model->primitives)
            triangles += qint64(p.indices.size() / 3);
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("name"), objectName(value)},
        {QObject::tr("vertices"), count ? QVariant(count->toInt()) : QVariant()},
        {QObject::tr("submeshes"), subMeshes ? QVariant(qint64(subMeshes->children.size())) : QVariant()},
        {QObject::tr("triangles"), model ? QVariant(triangles) : QVariant()},
    };
    if (!streamed.isEmpty())
        if (const std::optional<ResourceRef> ref = resourceOf(value))
            rows.push_back({QObject::tr("vertices stored in"), QStringLiteral("%1 (offset %2)").arg(ref->path).arg(ref->offset)});
    if (!model) {
        auto table = std::make_shared<SimpleTable>(QObject::tr("Mesh"),
                                                   std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                                   std::move(rows));
        table->setDescription(error);
        return table;
    }
    auto table = std::make_shared<MeshTable>(std::move(rows), model);
    table->setDescription(QObject::tr("Mesh %1: %n triangle(s)", nullptr, int(std::min<qint64>(triangles, INT_MAX)))
                              .arg(objectName(value)));
    return table;
}

std::vector<std::shared_ptr<const Image>> embeddedTextures(const Model3D &model)
{
    std::vector<std::shared_ptr<const Image>> images(model.textures.size());
    if (model.embeddedTexture)
        for (size_t i = 0; i < model.textures.size(); ++i)
            images[i] = model.embeddedTexture(model.textures[i].name);
    return images;
}

std::shared_ptr<SimpleTable> sceneTable(const UnpackedObject &root, const UValue &value, bool big, qint32 platform)
{
    const QJsonObject &h = root.header;
    UnityScene scene;
    scene.unityVersion = h.value(QStringLiteral("unityVersion")).toString();
    scene.bigEndian = big;
    scene.platform = platform;
    const int rootFile = h.value(QStringLiteral("fileIndex")).toInt();
    const qint64 rootId = h.value(QStringLiteral("pathId")).toString().toLongLong();
    const QJsonObject externals = h.value(QStringLiteral("externals")).toObject();
    for (auto it = externals.begin(); it != externals.end(); ++it) {
        const int file = it.key().toInt();
        if (file < 0 || file > 4096)
            continue;
        if (size_t(file) >= scene.externals.size())
            scene.externals.resize(size_t(file) + 1);
        for (const QJsonValue &v : it.value().toArray())
            scene.externals[size_t(file)] << v.toInt(-1);
    }
    scene.objects.push_back({rootFile, rootId, kGameObject, value, {}});
    const QByteArray &linked = root.linked;
    for (qsizetype at = 0; at + 4 <= linked.size();) {
        const quint32 size = qFromLittleEndian<quint32>(linked.constData() + at);
        at += 4;
        if (size > quint32(linked.size() - at))
            break;
        const std::optional<UnpackedObject> part = unpackObject(linked.mid(at, size));
        at += size;
        if (!part)
            continue;
        std::optional<UValue> partValue = readTypeTreeValue(part->nodes, part->data, big, nullptr);
        if (!partValue)
            continue;
        scene.objects.push_back({part->header.value(QStringLiteral("fileIndex")).toInt(),
                                 part->header.value(QStringLiteral("pathId")).toString().toLongLong(),
                                 part->header.value(QStringLiteral("classId")).toInt(), std::move(*partValue),
                                 part->resource});
    }
    scene.buildIndex();
    UnitySceneSummary summary;
    QString error;
    std::shared_ptr<const Model3D> model = unitySceneModel(scene, rootFile, rootId, &summary, &error);
    if (!model)
        return nullptr;
    qint64 triangles = 0;
    for (const ModelPrimitive &p : model->primitives)
        triangles += qint64(p.indices.size() / 3);
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("name"), objectName(value)},
        {QObject::tr("meshes"), summary.meshes},
        {QObject::tr("skinned meshes"), summary.skinned},
        {QObject::tr("bones and nodes"), summary.bones},
        {QObject::tr("triangles"), triangles},
        {QObject::tr("materials"), summary.materials},
        {QObject::tr("textures"), summary.textures},
        {QObject::tr("animations"), summary.animations},
    };
    if (summary.skippedAnimations > 0)
        rows.push_back({QObject::tr("animations for other models"), summary.skippedAnimations});
    auto table = std::make_shared<MeshTable>(std::move(rows), model, QObject::tr("Model"));
    QString description = QObject::tr("Model %1: %n mesh(es)", nullptr, summary.meshes).arg(objectName(value));
    if (summary.bones > 1 && summary.skinned > 0)
        description += QObject::tr(", %n bone(s)", nullptr, summary.bones);
    if (summary.textures > 0)
        description += QObject::tr(", %n texture(s)", nullptr, summary.textures);
    if (summary.animations > 0)
        description += QObject::tr(", %n animation(s)", nullptr, summary.animations);
    table->setDescription(description);
    return table;
}

struct ObjectRef
{
    int file = 0;
    size_t object = 0;
};

struct UnitySource
{
    std::vector<SerializedFile> files;
    QStringList labels;
    std::vector<QHash<qint64, QString>> containers;
    ResourceLoader loader;
    std::vector<ObjectRef> refs;
};

struct Listing
{
    std::vector<SimpleTable::Row> rows;
    std::shared_ptr<UnitySource> source = std::make_shared<UnitySource>();
    std::shared_ptr<Archive> archive = std::make_shared<Archive>();
    QHash<QString, int> typeCounts;
    QSet<QString> names;
    QStringList problems;
};

QHash<qint64, QString> containerPaths(const SerializedFile &file)
{
    QHash<qint64, QString> out;
    for (const SerializedObject &object : file.objects) {
        const SerializedType *type = file.typeOf(object);
        if (!type || type->classId != kAssetBundle)
            continue;
        const std::optional<UValue> value = readTypeTreeValue(type->nodes, file.objectData(object), file.bigEndian, nullptr);
        const UValue *list = value ? value->child(u"m_Container") : nullptr;
        if (!list)
            continue;
        for (const UValue &pair : list->children) {
            const UValue *first = pair.child(u"first"), *id = pair.at(u"second.asset.m_PathID");
            if (first && id && !out.contains(id->toInt()))
                out.insert(id->toInt(), first->toString());
        }
    }
    return out;
}

QByteArray loadResource(const UValue &value, const ResourceLoader &loader)
{
    const std::optional<ResourceRef> ref = resourceOf(value);
    if (!ref || !loader)
        return {};
    return loader(ref->path, ref->offset, ref->size);
}

std::optional<UValue> objectValue(const SerializedFile &file, const SerializedObject &object)
{
    const SerializedType *type = file.typeOf(object);
    if (!type || type->nodes.empty())
        return std::nullopt;
    return readTypeTreeValue(type->nodes, file.objectData(object), file.bigEndian, nullptr);
}

QList<int> externalFiles(const UnitySource &source, int fileIndex)
{
    QList<int> out;
    for (const QString &external : source.files[size_t(fileIndex)].externals) {
        const QString name = external.section(u'/', -1).toLower();
        int found = -1;
        for (qsizetype i = 0; i < source.labels.size() && found < 0; ++i)
            if (source.labels[i].section(u'/', -1).toLower() == name)
                found = int(i);
        out << found;
    }
    return out;
}

bool isRootGameObject(const UnitySource &source, int fileIndex, const UValue &gameObject)
{
    const SerializedFile &file = source.files[size_t(fileIndex)];
    const UValue *components = gameObject.child(u"m_Component");
    if (!components)
        return false;
    for (const UValue &pair : components->children) {
        const UValue *pointer = pair.child(u"component");
        const UValue *fileId = pointer ? pointer->child(u"m_FileID") : nullptr;
        const UValue *pathId = pointer ? pointer->child(u"m_PathID") : nullptr;
        if (!fileId || !pathId || fileId->toInt() != 0)
            continue;
        const SerializedObject *component = file.find(pathId->toInt());
        const SerializedType *type = component ? file.typeOf(*component) : nullptr;
        if (!type || (type->classId != unityclass::Transform && type->classId != unityclass::RectTransform))
            continue;
        const std::optional<UValue> transform = objectValue(file, *component);
        const UValue *father = transform ? transform->at(u"m_Father.m_PathID") : nullptr;
        return father && father->toInt() == 0;
    }
    return false;
}

QByteArray packScene(const UnitySource &source, const ObjectRef &root, QJsonObject *rootExtra)
{
    struct Pending
    {
        int file;
        qint64 pathId;
    };
    std::map<int, QList<int>> externals;
    auto externalsOf = [&](int file) -> const QList<int> & {
        auto it = externals.find(file);
        if (it == externals.end())
            it = externals.emplace(file, externalFiles(source, file)).first;
        return it->second;
    };
    std::set<std::pair<int, qint64>> seen;
    std::vector<Pending> queue;
    const SerializedFile &rootFile = source.files[size_t(root.file)];
    const qint64 rootId = rootFile.objects[root.object].pathId;
    queue.push_back({root.file, rootId});
    seen.insert({root.file, rootId});
    QByteArray linked;
    bool animator = false;
    int count = 0;
    QStringList bareMaterials;
    auto follow = [&](int file, const UValue *pointer) {
        const UValue *fileId = pointer->child(u"m_FileID"), *pathId = pointer->child(u"m_PathID");
        if (!fileId || !pathId || pathId->toInt() == 0)
            return;
        int target = file;
        if (const qint64 id = fileId->toInt(); id != 0) {
            const QList<int> &map = externalsOf(file);
            if (id < 1 || id > map.size() || map[qsizetype(id - 1)] < 0)
                return;
            target = map[qsizetype(id - 1)];
        }
        if (seen.insert({target, pathId->toInt()}).second)
            queue.push_back({target, pathId->toInt()});
    };
    auto add = [&](int fileIndex, const SerializedObject &object, const UValue *value) {
        const SerializedFile &file = source.files[size_t(fileIndex)];
        const QByteArray blob = packObject(file, object, source.labels[fileIndex], {},
                                           value ? loadResource(*value, source.loader) : QByteArray(), {},
                                           QJsonObject{{QStringLiteral("fileIndex"), fileIndex}});
        char size[4];
        qToLittleEndian<quint32>(quint32(blob.size()), size);
        linked.append(size, 4);
        linked += blob;
    };
    for (size_t at = 0; at < queue.size() && count < kMaxSceneObjects; ++at) {
        const Pending p = queue[at];
        const SerializedFile &file = source.files[size_t(p.file)];
        const SerializedObject *object = file.find(p.pathId);
        const SerializedType *type = object ? file.typeOf(*object) : nullptr;
        if (!type)
            continue;
        if (type->classId == unityclass::AnimationClip) {
            add(p.file, *object, nullptr);
            ++count;
            continue;
        }
        const std::optional<UValue> value = objectValue(file, *object);
        if (!value)
            continue;
        animator = animator || type->classId == unityclass::Animator;
        if (type->classId == unityclass::Material && !unityMaterialHasTextures(*value))
            bareMaterials << objectName(*value);
        for (const UValue *pointer : unityScenePointers(type->classId, *value))
            follow(p.file, pointer);
        if (at > 0) {
            const bool heavy = type->classId == unityclass::Mesh || type->classId == unityclass::Texture2D;
            add(p.file, *object, heavy ? &*value : nullptr);
            ++count;
        }
    }
    if (!bareMaterials.isEmpty()) {
        struct Candidate
        {
            const SerializedObject *object;
            UValue value;
            QString name;
        };
        std::vector<Candidate> candidates;
        for (const SerializedObject &object : rootFile.objects) {
            const SerializedType *type = rootFile.typeOf(object);
            if (!type || type->classId != unityclass::Texture2D || candidates.size() >= size_t(kMaxNamedTextures)
                || seen.count({root.file, object.pathId}))
                continue;
            if (std::optional<UValue> value = objectValue(rootFile, object)) {
                const QString name = objectName(*value);
                candidates.push_back({&object, std::move(*value), name});
            }
        }
        const qsizetype colors = std::count_if(candidates.begin(), candidates.end(),
                                               [](const Candidate &c) { return unityColorTextureName(c.name); });
        for (const Candidate &c : candidates) {
            const QStringList words = unityNameWords(c.name);
            const bool named = !words.isEmpty()
                            && std::any_of(bareMaterials.begin(), bareMaterials.end(),
                                           [&words](const QString &m) { return unityNameContains(m, words.front()); });
            if (!named && colors != 1)
                continue;
            seen.insert({root.file, c.object->pathId});
            add(root.file, *c.object, &c.value);
        }
    }
    if (!animator) {
        int clips = 0;
        for (const SerializedObject &object : rootFile.objects) {
            const SerializedType *type = rootFile.typeOf(object);
            if (!type || type->classId != unityclass::AnimationClip || clips >= kMaxSceneClips)
                continue;
            if (!seen.insert({root.file, object.pathId}).second)
                continue;
            add(root.file, object, nullptr);
            ++clips;
        }
    }
    QJsonObject map;
    for (const auto &[file, list] : externals) {
        QJsonArray array;
        for (int i : list)
            array.append(i);
        map.insert(QString::number(file), array);
    }
    rootExtra->insert(QStringLiteral("fileIndex"), root.file);
    rootExtra->insert(QStringLiteral("externals"), map);
    rootExtra->insert(QStringLiteral("scene"), true);
    return linked;
}

QByteArray packEntry(const UnitySource &source, const ObjectRef &ref)
{
    const SerializedFile &file = source.files[size_t(ref.file)];
    const QString &label = source.labels[ref.file];
    const QHash<qint64, QString> &containers = source.containers[size_t(ref.file)];
    const SerializedObject &object = file.objects[ref.object];
    const SerializedType *type = file.typeOf(object);
    const std::optional<UValue> value =
        type ? readTypeTreeValue(type->nodes, file.objectData(object), file.bigEndian, nullptr) : std::nullopt;
    QByteArray resource, linked;
    if (value && type->classId == kGameObject && isRootGameObject(source, ref.file, *value)) {
        QJsonObject extra;
        linked = packScene(source, ref, &extra);
        return packObject(file, object, label, containers.value(object.pathId), {}, linked, extra);
    }
    if (value) {
        resource = loadResource(*value, source.loader);
        const UValue *fileId = value->at(u"m_RD.texture.m_FileID"), *id = value->at(u"m_RD.texture.m_PathID");
        if (type->classId == kSprite && fileId && id && fileId->toInt() == 0) {
            if (const SerializedObject *texture = file.find(id->toInt())) {
                const SerializedType *textureType = file.typeOf(*texture);
                const std::optional<UValue> tv =
                    textureType ? readTypeTreeValue(textureType->nodes, file.objectData(*texture), file.bigEndian, nullptr)
                                : std::nullopt;
                if (tv)
                    linked = packObject(file, *texture, label, containers.value(texture->pathId),
                                        loadResource(*tv, source.loader), {});
            }
        }
    }
    return packObject(file, object, label, containers.value(object.pathId), resource, linked);
}

void listObjects(SerializedFile file, const QString &label, Listing &listing)
{
    UnitySource &source = *listing.source;
    const int fileIndex = int(source.files.size());
    source.containers.push_back(containerPaths(file));
    const QHash<qint64, QString> &containers = source.containers.back();
    for (size_t i = 0; i < file.objects.size(); ++i) {
        const SerializedObject &object = file.objects[i];
        const SerializedType *type = file.typeOf(object);
        const QString className = type ? type->className() : QObject::tr("unknown class");
        const QByteArray data = file.objectData(object);
        QString name;
        qint64 size = object.size;
        if (type && !type->nodes.empty() && (type->classId == kTexture2D || type->classId == kAudioClip)) {
            const std::optional<UValue> value = readTypeTreeValue(type->nodes, data, file.bigEndian, nullptr);
            if (!value)
                listing.problems << QStringLiteral("%1 #%2").arg(className).arg(object.pathId);
            if (value) {
                name = objectName(*value);
                if (const std::optional<ResourceRef> ref = resourceOf(*value))
                    size += ref->size;
            }
        } else if (type) {
            name = readObjectName(type->nodes, data, file.bigEndian);
        }
        QString entryName = (name.isEmpty() ? QStringLiteral("#%1").arg(object.pathId) : name) + u'.' + className;
        if (listing.names.contains(entryName))
            entryName = QStringLiteral("%1 #%2.%3").arg(name.isEmpty() ? QStringLiteral("object") : name).arg(object.pathId).arg(className);
        listing.names.insert(entryName);
        listing.archive->entries.push_back({entryName, 0, size, UnityObjectReader::kMagic.toByteArray()});
        source.refs.push_back({fileIndex, i});
        listing.typeCounts[className] += 1;
        listing.rows.push_back({entryName, className, name, object.pathId, size, containers.value(object.pathId), label});
    }
    source.files.push_back(std::move(file));
    source.labels << label;
}

std::unique_ptr<Document> listingDocument(Listing listing, const QString &summary, const QString &note = {})
{
    listing.archive->loader = [source = listing.source](size_t index) {
        return index < source->refs.size() ? packEntry(*source, source->refs[index]) : QByteArray();
    };
    listing.archive->extractor = [archive = std::weak_ptr<Archive>(listing.archive)](size_t index) -> std::optional<ExtractedFile> {
        const std::shared_ptr<Archive> self = archive.lock();
        if (!self || index >= self->entries.size())
            return std::nullopt;
        const QString name = self->entries[index].name;
        const std::unique_ptr<Document> doc = UnityObjectReader().readBytes(self->entryData(index), name, nullptr);
        if (!doc || doc->tables.empty())
            return std::nullopt;
        const QString base = name.section(u'.', 0, -2);
        for (const auto &table : doc->tables) {
            if (table->hasModel()) {
                if (const std::shared_ptr<const Model3D> model = table->model())
                    return ExtractedFile{base + QStringLiteral(".glb"), writeGlb(*model, embeddedTextures(*model))};
                continue;
            }
            if (table->hasImage()) {
                const std::shared_ptr<const Image> image = table->image();
                if (image && image->width > 0)
                    return ExtractedFile{base + QStringLiteral(".png"), encodePng(*image)};
            }
            if (const auto inner = table->archive(); inner && inner->entries.size() == 1)
                return ExtractedFile{inner->entries.front().name, inner->entryData(0)};
        }
        for (const auto &table : doc->tables)
            if (table->canExportJson())
                return ExtractedFile{name + QStringLiteral(".json"), table->exportJson()};
        return std::nullopt;
    };
    std::vector<SimpleColumn> columns = {
        {QObject::tr("entry"), false, QObject::tr("Double-click to open the object")},
        {QObject::tr("type"), false, QObject::tr("The Unity class of the object")},
        {QObject::tr("name")},
        {QObject::tr("path id"), false, QObject::tr("The object's ID inside its file")},
        {QObject::tr("size"), false, QObject::tr("Bytes, including pixel or sound data stored next to it")},
        {QObject::tr("asset path"), false, QObject::tr("The name the game loads this asset by, when the bundle lists one")},
        {QObject::tr("file"), false, QObject::tr("The serialized file inside the bundle that holds the object")},
    };
    auto table = std::make_shared<ArchiveTable>(QObject::tr("Objects"), std::move(columns), std::move(listing.rows),
                                                listing.archive);
    QStringList kinds;
    QList<QPair<int, QString>> counts;
    for (auto it = listing.typeCounts.cbegin(); it != listing.typeCounts.cend(); ++it)
        counts.append({it.value(), it.key()});
    std::sort(counts.begin(), counts.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (int i = 0; i < std::min<qsizetype>(counts.size(), 6); ++i)
        kinds << QStringLiteral("%1 %2").arg(counts[i].first).arg(counts[i].second);
    QString description = summary + QStringLiteral(": ") + QObject::tr("%n object(s)", nullptr, int(listing.archive->entries.size()));
    if (!kinds.isEmpty())
        description += QStringLiteral(" (") + kinds.join(QStringLiteral(", ")) + QStringLiteral(")");
    if (!listing.problems.isEmpty())
        description += QObject::tr("; %n object(s) could not be read", nullptr, int(listing.problems.size()));
    if (!note.isEmpty())
        description += QStringLiteral("; ") + note;
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

QByteArray readFileSlice(const QString &path, qint64 offset, qint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || offset < 0 || size < 0 || offset + size > file.size() || !file.seek(offset))
        return {};
    return file.read(size);
}

std::unique_ptr<Document> readAssets(const QByteArray &data, const QString &fileName, const ResourceLoader &loader,
                                     QString *error)
{
    std::optional<SerializedFile> file = readSerializedFile(data, error);
    if (!file)
        return nullptr;
    Listing listing;
    listing.source->loader = loader;
    const QString version = file->unityVersion;
    const bool typeTrees = file->hasTypeTrees;
    QStringList builtin;
    for (const SerializedType &type : file->types)
        if (type.builtin)
            builtin << type.className();
    listObjects(std::move(*file), QFileInfo(fileName).fileName(), listing);
    QString note;
    if (!typeTrees && builtin.isEmpty())
        note = QObject::tr("built without type information, so objects are listed but cannot be opened; the game's "
                           "asset bundles include it");
    else if (!typeTrees)
        note = QObject::tr("built without type information; %1 objects are read with the app's built-in layouts, "
                           "the others are only listed")
                   .arg(builtin.join(QStringLiteral(", ")));
    return listingDocument(std::move(listing), QObject::tr("Unity %1 file").arg(version), note);
}

std::shared_ptr<SimpleTable> textureTable(const UValue &value, const QByteArray &pixels, qint32 platform,
                                          const QString &title, const std::optional<std::array<double, 4>> &crop)
{
    const UValue *w = value.child(u"m_Width"), *h = value.child(u"m_Height"), *f = value.child(u"m_TextureFormat"),
                 *mips = value.child(u"m_MipCount");
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("name"), objectName(value)},
        {QObject::tr("size"), w && h ? QStringLiteral("%1 × %2").arg(w->toInt()).arg(h->toInt()) : QString()},
        {QObject::tr("format"), f ? unityTextureFormatName(int(f->toInt())) : QString()},
        {QObject::tr("mip levels"), mips ? QVariant(mips->toInt()) : QVariant()},
        {QObject::tr("data size"), sizeText(pixels.isEmpty() ? (value.child(u"image data") ? value.child(u"image data")->bytes.size() : 0) : pixels.size())},
    };
    if (const int gobs = switchGobsPerBlock(value, platform); gobs > 0)
        rows.push_back({QObject::tr("Switch layout"), QObject::tr("block linear, %n GOB(s) per block", nullptr, gobs)});
    if (const std::optional<ResourceRef> ref = resourceOf(value))
        rows.push_back({QObject::tr("pixels stored in"), QStringLiteral("%1 (offset %2)").arg(ref->path).arg(ref->offset)});
    auto table = std::make_shared<SimpleTable>(title, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                               std::move(rows));
    table->setDescription(QObject::tr("%1 texture").arg(f ? unityTextureFormatName(int(f->toInt())) : QString()));
    if (!w || !h || w->toInt() <= 0 || h->toInt() <= 0) {
        table->setDescription(QObject::tr("Empty texture: the game fills it while running (for example a font's glyphs)"));
        return table;
    }
    auto shared = std::make_shared<UValue>(value);
    table->setImageMaker([shared, pixels, platform, crop] {
        QByteArray data = pixels;
        if (data.isEmpty())
            if (const UValue *inline_ = shared->child(u"image data"))
                data = inline_->bytes;
        std::optional<Image> image = decodeUnityTexture(*shared, data, platform, nullptr);
        if (!image)
            return Image{};
        if (crop) {
            const auto &r = *crop;
            const int x = int(std::floor(r[0])), width = int(std::ceil(r[2]));
            const int height = int(std::ceil(r[3]));
            const int y = image->height - int(std::floor(r[1])) - height;
            return cropImage(*image, x, y, width, height);
        }
        return *image;
    });
    return table;
}

std::unique_ptr<Document> listBundle(std::shared_ptr<const UnityBundle> bundle, QString *error)
{
    auto resources = std::make_shared<QHash<QString, size_t>>();
    Listing listing;
    int serialized = 0;
    for (size_t i = 0; i < bundle->nodes.size(); ++i) {
        const QString &path = bundle->nodes[i].path;
        QString nodeError;
        const std::optional<QByteArray> head =
            bundle->read(i, 0, std::min<qint64>(bundle->nodes[i].size, 64), &nodeError);
        if (!head) {
            listing.problems << path;
            continue;
        }
        if (!looksLikeSerializedFile(*head)) {
            resources->insert(path.section(u'/', -1).toLower(), i);
            continue;
        }
        std::optional<QByteArray> nodeData = bundle->nodeData(i, &nodeError);
        std::optional<SerializedFile> file = nodeData ? readSerializedFile(*nodeData, &nodeError) : std::nullopt;
        nodeData.reset();
        if (!file) {
            listing.problems << path;
            continue;
        }
        ++serialized;
        listObjects(std::move(*file), path, listing);
    }
    listing.source->loader = [bundle, resources](const QString &path, qint64 offset, qint64 size) {
        const auto it = resources->constFind(path.section(u'/', -1).toLower());
        if (it == resources->cend())
            return QByteArray();
        return bundle->read(*it, offset, size).value_or(QByteArray());
    };
    if (serialized == 0) {
        if (error)
            *error = QObject::tr("The bundle holds no Unity objects that can be read.");
        return nullptr;
    }
    const QString version = bundle->unityVersion.startsWith(QLatin1String("5.x")) ? bundle->unityRevision
                                                                                  : bundle->unityVersion;
    return listingDocument(std::move(listing), QObject::tr("Unity %1 bundle").arg(version));
}

}

int UnityBundleReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("UnityFS") ? 100 : 0;
}

std::unique_ptr<Document> UnityBundleReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 2 GB.");
        return nullptr;
    }
    file.close();
    std::optional<UnityBundle> bundle = readUnityBundleFile(path, error);
    if (!bundle)
        return nullptr;
    return listBundle(std::make_shared<const UnityBundle>(std::move(*bundle)), error);
}

std::unique_ptr<Document> UnityBundleReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    std::optional<UnityBundle> bundle = readUnityBundle(data, error);
    if (!bundle)
        return nullptr;
    return listBundle(std::make_shared<const UnityBundle>(std::move(*bundle)), error);
}

std::unique_ptr<Document> UnityBundleReader::readStored(const StoredReader &read, qint64 size, const QString &,
                                                        QString *error) const
{
    if (size > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 2 GB.");
        return nullptr;
    }
    std::optional<UnityBundle> bundle = readUnityBundleStored(read, size, error);
    if (!bundle)
        return nullptr;
    return listBundle(std::make_shared<const UnityBundle>(std::move(*bundle)), error);
}

int UnityAssetsReader::score(const QString &, QByteArrayView head) const
{
    return looksLikeSerializedFile(head) ? 90 : 0;
}

std::unique_ptr<Document> UnityAssetsReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 2 GB.");
        return nullptr;
    }
    const QString folder = QFileInfo(path).absolutePath();
    const ResourceLoader loader = [folder](const QString &resource, qint64 offset, qint64 size) {
        return readFileSlice(QDir(folder).filePath(resource.section(u'/', -1)), offset, size);
    };
    return readAssets(file.readAll(), path, loader, error);
}

std::unique_ptr<Document> UnityAssetsReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return readAssets(data, fileName, {}, error);
}

int UnityObjectReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith(kMagic) ? 100 : 0;
}

std::unique_ptr<Document> UnityObjectReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> UnityObjectReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    const std::optional<UnpackedObject> object = unpackObject(data);
    if (!object) {
        if (error)
            *error = QObject::tr("The Unity object is damaged.");
        return nullptr;
    }
    const QJsonObject &h = object->header;
    const bool big = h.value(QStringLiteral("bigEndian")).toBool();
    const qint32 platform = h.value(QStringLiteral("platform")).toInt();
    const int classId = h.value(QStringLiteral("classId")).toInt();
    QString reason;
    std::optional<UValue> value = readTypeTreeValue(object->nodes, object->data, big, &reason);
    if (!value) {
        std::vector<SimpleTable::Row> rows = {
            {QObject::tr("class"), unityClassName(classId)},
            {QObject::tr("path id"), h.value(QStringLiteral("pathId")).toString()},
            {QObject::tr("file"), h.value(QStringLiteral("file")).toString()},
            {QObject::tr("size"), qint64(object->data.size())},
            {QObject::tr("fields"), reason},
        };
        auto table = std::make_shared<SimpleTable>(QObject::tr("Object"),
                                                   std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                                   std::move(rows));
        table->setDescription(reason);
        auto doc = std::make_unique<Document>();
        doc->tables.push_back(std::move(table));
        return doc;
    }
    auto shared = std::make_shared<const UValue>(std::move(*value));
    const QString className = object->nodes.empty() ? QString() : object->nodes.front().type;

    auto doc = std::make_unique<Document>();
    if (classId == kGameObject && h.value(QStringLiteral("scene")).toBool()) {
        if (std::shared_ptr<SimpleTable> scene = sceneTable(*object, *shared, big, platform))
            doc->tables.push_back(std::move(scene));
    } else if (classId == kTexture2D) {
        doc->tables.push_back(textureTable(*shared, object->resource, platform, QObject::tr("Texture"), std::nullopt));
    } else if (classId == kMesh) {
        doc->tables.push_back(meshTable(*shared, object->resource, h.value(QStringLiteral("unityVersion")).toString(), big));
    } else if (classId == kSprite && !object->linked.isEmpty()) {
        const std::optional<UnpackedObject> texture = unpackObject(object->linked);
        const UValue *rect = shared->at(u"m_RD.textureRect");
        std::optional<UValue> textureValue =
            texture ? readTypeTreeValue(texture->nodes, texture->data, big, nullptr) : std::nullopt;
        if (textureValue && rect && rect->child(u"x") && rect->child(u"y") && rect->child(u"width") && rect->child(u"height")) {
            const std::array<double, 4> r = {rect->child(u"x")->toDouble(), rect->child(u"y")->toDouble(),
                                             rect->child(u"width")->toDouble(), rect->child(u"height")->toDouble()};
            auto table = textureTable(*textureValue, texture->resource, platform, QObject::tr("Sprite"), r);
            table->setDescription(QObject::tr("Sprite %1, %2 × %3 from texture %4")
                                      .arg(objectName(*shared))
                                      .arg(r[2])
                                      .arg(r[3])
                                      .arg(objectName(*textureValue)));
            doc->tables.push_back(std::move(table));
        }
    } else if (classId == kTextAsset) {
        if (const UValue *script = shared->child(u"m_Script"); script && script->kind == UValue::Kind::String) {
            QString name = objectName(*shared);
            if (name.isEmpty())
                name = QStringLiteral("text");
            if (!name.contains(u'.'))
                name += stringText(script->bytes) == QString::fromUtf8(script->bytes) ? QStringLiteral(".txt")
                                                                                     : QStringLiteral(".bytes");
            auto archive = std::make_shared<Archive>();
            archive->data = script->bytes;
            archive->entries.push_back({name, 0, script->bytes.size()});
            auto table = std::make_shared<ArchiveTable>(
                QObject::tr("Contents"),
                std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("type")}},
                std::vector<SimpleTable::Row>{{name, qint64(script->bytes.size()),
                                               describeContent(QByteArrayView(script->bytes).first(
                                                   std::min<qsizetype>(script->bytes.size(), 16)))}},
                archive);
            table->setDescription(QObject::tr("Text asset %1").arg(name));
            doc->tables.push_back(std::move(table));
        }
    }

    std::vector<TreeRow> rows;
    flatten(*shared, {}, rows);
    auto fields = std::make_shared<TreeTable>(QObject::tr("Fields (%1)").arg(className), std::move(rows), [shared] {
        QByteArray out;
        appendJson(out, *shared, 0);
        out += '\n';
        return out;
    });
    doc->tables.push_back(std::move(fields));
    return doc;
}
