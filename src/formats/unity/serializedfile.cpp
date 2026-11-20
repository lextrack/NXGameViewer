#include "unity/serializedfile.h"

#include "unity/builtintypetrees.h"

#include <QHash>
#include <QObject>
#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace {

constexpr char kCommonStrings[] =
    "AABB\0" "AnimationClip\0" "AnimationCurve\0" "AnimationState\0" "Array\0" "Base\0" "BitField\0" "bitset\0"
    "bool\0" "char\0" "ColorRGBA\0" "Component\0" "data\0" "deque\0" "double\0" "dynamic_array\0"
    "FastPropertyName\0" "first\0" "float\0" "Font\0" "GameObject\0" "Generic Mono\0" "GradientNEW\0" "GUID\0"
    "GUIStyle\0" "int\0" "list\0" "long long\0" "map\0" "Matrix4x4f\0" "MdFour\0" "MonoBehaviour\0" "MonoScript\0"
    "m_ByteSize\0" "m_Curve\0" "m_EditorClassIdentifier\0" "m_EditorHideFlags\0" "m_Enabled\0" "m_ExtensionPtr\0"
    "m_GameObject\0" "m_Index\0" "m_IsArray\0" "m_IsStatic\0" "m_MetaFlag\0" "m_Name\0" "m_ObjectHideFlags\0"
    "m_PrefabInternal\0" "m_PrefabParentObject\0" "m_Script\0" "m_StaticEditorFlags\0" "m_Type\0" "m_Version\0"
    "Object\0" "pair\0" "PPtr<Component>\0" "PPtr<GameObject>\0" "PPtr<Material>\0" "PPtr<MonoBehaviour>\0"
    "PPtr<MonoScript>\0" "PPtr<Object>\0" "PPtr<Prefab>\0" "PPtr<Sprite>\0" "PPtr<TextAsset>\0" "PPtr<Texture>\0"
    "PPtr<Texture2D>\0" "PPtr<Transform>\0" "Prefab\0" "Quaternionf\0" "Rectf\0" "RectInt\0" "RectOffset\0"
    "second\0" "set\0" "short\0" "size\0" "SInt16\0" "SInt32\0" "SInt64\0" "SInt8\0" "staticvector\0" "string\0"
    "TextAsset\0" "TextMesh\0" "Texture\0" "Texture2D\0" "Transform\0" "TypelessData\0" "UInt16\0" "UInt32\0"
    "UInt64\0" "UInt8\0" "unsigned int\0" "unsigned long long\0" "unsigned short\0" "vector\0" "Vector2f\0"
    "Vector3f\0" "Vector4f\0" "m_ScriptingClassIdentifier\0" "Gradient\0" "Type*\0" "int2_storage\0"
    "int3_storage\0" "BoundsInt\0" "m_CorrespondingSourceObject\0" "m_PrefabInstance\0" "m_PrefabAsset\0"
    "FileSize\0" "Hash128\0" "RenderingLayerMask\0" "fixed_array\0" "EntityId\0" "LoadableObjectId\0"
    "LoadableSceneId\0";

QString commonString(quint32 offset)
{
    if (offset >= sizeof(kCommonStrings) - 1)
        return QString::number(offset);
    return QString::fromLatin1(kCommonStrings + offset);
}

class Reader
{
public:
    Reader(QByteArrayView data, bool bigEndian) : m_data(data), m_big(bigEndian) {}

    qsizetype pos() const { return m_pos; }
    qsizetype size() const { return m_data.size(); }
    void setBigEndian(bool big) { m_big = big; }
    void seek(qsizetype pos)
    {
        if (pos < 0 || pos > m_data.size())
            throw std::runtime_error("seek");
        m_pos = pos;
    }
    void align(qsizetype to = 4) { seek(std::min<qsizetype>((m_pos + to - 1) / to * to, m_data.size())); }
    QByteArrayView take(qsizetype n)
    {
        if (n < 0 || n > m_data.size() - m_pos)
            throw std::runtime_error("short");
        const QByteArrayView out = m_data.sliced(m_pos, n);
        m_pos += n;
        return out;
    }
    template <typename T> T read()
    {
        const char *p = take(sizeof(T)).data();
        return m_big ? qFromBigEndian<T>(p) : qFromLittleEndian<T>(p);
    }
    template <typename T> T be() { return qFromBigEndian<T>(take(sizeof(T)).data()); }
    float readFloat()
    {
        const quint32 bits = read<quint32>();
        float out;
        std::memcpy(&out, &bits, 4);
        return out;
    }
    double readDouble()
    {
        const quint64 bits = read<quint64>();
        double out;
        std::memcpy(&out, &bits, 8);
        return out;
    }
    QString cstring()
    {
        const qsizetype end = m_data.indexOf('\0', m_pos);
        if (end < 0)
            throw std::runtime_error("string");
        const QString out = QString::fromUtf8(m_data.sliced(m_pos, end - m_pos));
        m_pos = end + 1;
        return out;
    }

private:
    QByteArrayView m_data;
    bool m_big;
    qsizetype m_pos = 0;
};

int primitiveSize(const QString &type)
{
    static const QHash<QString, int> sizes = {
        {QStringLiteral("bool"), 1},          {QStringLiteral("SInt8"), 1},
        {QStringLiteral("UInt8"), 1},         {QStringLiteral("char"), 1},
        {QStringLiteral("SInt16"), 2},        {QStringLiteral("short"), 2},
        {QStringLiteral("UInt16"), 2},        {QStringLiteral("unsigned short"), 2},
        {QStringLiteral("SInt32"), 4},        {QStringLiteral("int"), 4},
        {QStringLiteral("UInt32"), 4},        {QStringLiteral("unsigned int"), 4},
        {QStringLiteral("Type*"), 4},         {QStringLiteral("float"), 4},
        {QStringLiteral("SInt64"), 8},        {QStringLiteral("long long"), 8},
        {QStringLiteral("UInt64"), 8},        {QStringLiteral("unsigned long long"), 8},
        {QStringLiteral("FileSize"), 8},      {QStringLiteral("double"), 8},
    };
    return sizes.value(type, 0);
}

bool isSigned(const QString &type)
{
    return type == QLatin1String("SInt8") || type == QLatin1String("SInt16") || type == QLatin1String("short")
        || type == QLatin1String("SInt32") || type == QLatin1String("int") || type == QLatin1String("SInt64")
        || type == QLatin1String("long long");
}

void readTypeTree(Reader &in, quint32 version, std::vector<TypeTreeNode> &nodes)
{
    const qint32 count = in.read<qint32>();
    const qint32 stringSize = in.read<qint32>();
    if (count < 0 || count > 1'000'000 || stringSize < 0)
        throw std::runtime_error("tree");
    struct Raw { quint8 level; quint32 type, name; qint32 size; quint32 meta; };
    std::vector<Raw> raw;
    raw.reserve(size_t(count));
    for (qint32 i = 0; i < count; ++i) {
        Raw r;
        in.read<quint16>();
        r.level = in.read<quint8>();
        in.read<quint8>();
        r.type = in.read<quint32>();
        r.name = in.read<quint32>();
        r.size = in.read<qint32>();
        in.read<qint32>();
        r.meta = in.read<quint32>();
        if (version >= 19)
            in.read<quint64>();
        raw.push_back(r);
    }
    const QByteArrayView strings = in.take(stringSize);
    auto text = [&](quint32 offset) {
        if (offset & 0x80000000u)
            return commonString(offset & 0x7FFFFFFFu);
        if (offset >= quint32(strings.size()))
            return QString::number(offset);
        const qsizetype end = strings.indexOf('\0', offset);
        return QString::fromUtf8(strings.sliced(offset, (end < 0 ? strings.size() : end) - offset));
    };
    nodes.clear();
    nodes.reserve(raw.size());
    std::vector<int> parents;
    for (const Raw &r : raw) {
        TypeTreeNode node;
        node.type = text(r.type);
        node.name = text(r.name);
        node.level = r.level;
        node.byteSize = r.size;
        node.metaFlags = r.meta;
        while (!parents.empty() && nodes[size_t(parents.back())].level >= node.level)
            parents.pop_back();
        const int index = int(nodes.size());
        if (!parents.empty())
            nodes[size_t(parents.back())].children.push_back(index);
        else if (index != 0)
            throw std::runtime_error("tree");
        nodes.push_back(std::move(node));
        parents.push_back(index);
    }
}

SerializedType readType(Reader &in, const SerializedFile &file, bool refType)
{
    SerializedType type;
    type.classId = in.read<qint32>();
    const quint32 v = file.version;
    if (v >= 16)
        in.read<quint8>();
    qint16 scriptIndex = -1;
    if (v >= 17)
        scriptIndex = in.read<qint16>();
    if (v >= 13) {
        if ((refType && scriptIndex >= 0) || (v < 16 && type.classId < 0) || (v >= 16 && type.classId == 114))
            in.take(16);
        in.take(16);
    }
    if (file.hasTypeTrees) {
        if (v < 12 && v != 10)
            throw std::runtime_error("old");
        readTypeTree(in, v, type.nodes);
        if (v >= 21) {
            if (refType) {
                in.cstring();
                in.cstring();
                in.cstring();
            } else {
                const qint32 deps = in.read<qint32>();
                if (deps < 0)
                    throw std::runtime_error("deps");
                in.take(qsizetype(deps) * 4);
            }
        }
    }
    return type;
}

class ValueReader
{
public:
    ValueReader(const std::vector<TypeTreeNode> &nodes, Reader &in) : m_nodes(nodes), m_in(in) {}

    UValue read(int index, int depth)
    {
        if (depth > 64)
            throw std::runtime_error("deep");
        const TypeTreeNode &node = m_nodes.at(size_t(index));
        UValue value;
        value.name = node.name;
        value.type = node.type;
        const int size = primitiveSize(node.type);
        if (size > 0 && node.children.empty()) {
            readPrimitive(node.type, size, value);
        } else if (node.type == QLatin1String("string")) {
            const qint32 length = m_in.read<qint32>();
            value.kind = UValue::Kind::String;
            value.bytes = m_in.take(length).toByteArray();
            if (!node.children.empty() && m_nodes.at(size_t(node.children.front())).aligned())
                m_in.align();
        } else if (node.type == QLatin1String("TypelessData")) {
            const qint32 length = m_in.read<qint32>();
            value.kind = UValue::Kind::Bytes;
            value.elementType = QStringLiteral("UInt8");
            value.elementSize = 1;
            value.bytes = m_in.take(length).toByteArray();
        } else if (!node.children.empty() && m_nodes.at(size_t(node.children.front())).type == QLatin1String("Array")) {
            const TypeTreeNode &array = m_nodes.at(size_t(node.children.front()));
            readArray(array, value, depth);
            if (array.aligned())
                m_in.align();
        } else if (node.type == QLatin1String("Array") && node.children.size() == 2) {
            readArray(node, value, depth);
        } else {
            value.kind = UValue::Kind::Struct;
            value.children.reserve(node.children.size());
            for (int c : node.children)
                value.children.push_back(read(c, depth + 1));
        }
        if (node.aligned())
            m_in.align();
        return value;
    }

    std::optional<QString> readName()
    {
        const TypeTreeNode &root = m_nodes.front();
        for (int c : root.children) {
            const TypeTreeNode &node = m_nodes.at(size_t(c));
            UValue value = read(c, 1);
            if (node.name == QLatin1String("m_Name"))
                return value.kind == UValue::Kind::String ? std::optional<QString>(value.toString()) : std::nullopt;
        }
        return std::nullopt;
    }

private:
    void readPrimitive(const QString &type, int size, UValue &value)
    {
        if (type == QLatin1String("bool")) {
            value.kind = UValue::Kind::Bool;
            value.i = m_in.read<quint8>() != 0;
        } else if (type == QLatin1String("float")) {
            value.kind = UValue::Kind::Float;
            value.f = m_in.readFloat();
        } else if (type == QLatin1String("double")) {
            value.kind = UValue::Kind::Float;
            value.f = m_in.readDouble();
        } else if (isSigned(type)) {
            value.kind = UValue::Kind::Int;
            switch (size) {
            case 1: value.i = m_in.read<qint8>(); break;
            case 2: value.i = m_in.read<qint16>(); break;
            case 4: value.i = m_in.read<qint32>(); break;
            default: value.i = m_in.read<qint64>(); break;
            }
        } else {
            value.kind = UValue::Kind::UInt;
            switch (size) {
            case 1: value.u = m_in.read<quint8>(); break;
            case 2: value.u = m_in.read<quint16>(); break;
            case 4: value.u = m_in.read<quint32>(); break;
            default: value.u = m_in.read<quint64>(); break;
            }
            value.i = qint64(value.u);
        }
    }

    void readArray(const TypeTreeNode &array, UValue &value, int depth)
    {
        if (array.children.size() < 2)
            throw std::runtime_error("array");
        const qint32 count = m_in.read<qint32>();
        if (count < 0 || count > m_in.size())
            throw std::runtime_error("count");
        const int elementIndex = array.children[1];
        const TypeTreeNode &element = m_nodes.at(size_t(elementIndex));
        const int size = primitiveSize(element.type);
        if (size > 0 && element.children.empty()) {
            value.kind = UValue::Kind::Bytes;
            value.elementType = element.type;
            value.elementSize = size;
            value.bytes = m_in.take(qsizetype(count) * size).toByteArray();
            if (element.aligned())
                m_in.align();
            return;
        }
        value.kind = UValue::Kind::Array;
        value.elementType = element.type;
        value.children.reserve(size_t(count));
        for (qint32 i = 0; i < count; ++i)
            value.children.push_back(read(elementIndex, depth + 1));
    }

    const std::vector<TypeTreeNode> &m_nodes;
    Reader &m_in;
};

}

QString SerializedType::className() const
{
    return nodes.empty() ? unityClassName(classId) : nodes.front().type;
}

const SerializedType *SerializedFile::typeOf(const SerializedObject &object) const
{
    if (object.typeIndex < 0 || size_t(object.typeIndex) >= types.size())
        return nullptr;
    return &types[size_t(object.typeIndex)];
}

const SerializedObject *SerializedFile::find(qint64 pathId) const
{
    for (const SerializedObject &o : objects)
        if (o.pathId == pathId)
            return &o;
    return nullptr;
}

bool looksLikeSerializedFile(QByteArrayView head)
{
    if (head.size() < 0x30)
        return false;
    const quint32 version = qFromBigEndian<quint32>(head.data() + 8);
    if (version < 9 || version > 50)
        return false;
    const qsizetype at = version >= 22 ? 0x30 : 0x14;
    if (head.size() < at + 4 || uchar(head[0x10]) > 1)
        return false;
    const QByteArrayView text = head.sliced(at);
    int digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
        ++digits;
    return digits >= 1 && digits <= 4 && digits < text.size() && text[digits] == '.';
}

namespace {

void applyBuiltinTypeTrees(SerializedFile &file)
{
    for (size_t t = 0; t < file.types.size(); ++t) {
        SerializedType &type = file.types[t];
        if (!type.nodes.empty())
            continue;
        std::vector<TypeTreeNode> nodes = builtinTypeTree(type.classId, file.unityVersion);
        if (nodes.empty())
            continue;
        bool fits = false;
        for (const SerializedObject &object : file.objects) {
            if (object.typeIndex != int(t))
                continue;
            qsizetype consumed = -1;
            const QByteArray data = file.objectData(object);
            if (!readTypeTreeValue(nodes, data, file.bigEndian, nullptr, &consumed) || consumed != data.size()) {
                fits = false;
                break;
            }
            fits = true;
        }
        if (fits) {
            type.nodes = std::move(nodes);
            type.builtin = true;
        }
    }
}

}

std::optional<SerializedFile> readSerializedFile(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<SerializedFile> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!looksLikeSerializedFile(QByteArrayView(data).first(std::min<qsizetype>(data.size(), 64))))
        return fail(QObject::tr("Not a Unity serialized file."));
    try {
        Reader in(data, true);
        SerializedFile file;
        in.be<quint32>();
        in.be<quint32>();
        file.version = in.be<quint32>();
        qint64 dataOffset = in.be<quint32>();
        file.bigEndian = in.read<quint8>() != 0;
        in.take(3);
        if (file.version >= 22) {
            in.be<quint32>();
            in.be<qint64>();
            dataOffset = in.be<qint64>();
            in.be<qint64>();
        }
        in.setBigEndian(file.bigEndian);
        file.unityVersion = in.cstring();
        file.platform = in.read<qint32>();
        file.hasTypeTrees = in.read<quint8>() != 0;
        if (file.version < 13)
            return fail(QObject::tr("Unity serialized file version %1 is too old.").arg(file.version));

        const qint32 typeCount = in.read<qint32>();
        if (typeCount < 0 || typeCount > 100'000)
            return fail(QObject::tr("The type list is damaged."));
        for (qint32 i = 0; i < typeCount; ++i)
            file.types.push_back(readType(in, file, false));
        if (file.version < 14)
            in.read<qint32>();

        const qint32 objectCount = in.read<qint32>();
        if (objectCount < 0 || objectCount > 10'000'000)
            return fail(QObject::tr("The object list is damaged."));
        file.objects.reserve(size_t(objectCount));
        for (qint32 i = 0; i < objectCount; ++i) {
            SerializedObject object;
            if (file.version >= 14) {
                in.align();
                object.pathId = in.read<qint64>();
            } else {
                object.pathId = in.read<qint32>();
            }
            object.offset = (file.version >= 22 ? in.read<qint64>() : qint64(in.read<quint32>())) + dataOffset;
            object.size = in.read<quint32>();
            object.typeIndex = in.read<qint32>();
            if (file.version < 16)
                in.read<quint16>();
            if (file.version < 17)
                in.read<quint16>();
            if (file.version == 15 || file.version == 16)
                in.read<quint8>();
            if (object.offset < 0 || object.offset + object.size > data.size())
                return fail(QObject::tr("Object %1 points outside the file.").arg(object.pathId));
            file.objects.push_back(object);
        }

        const qint32 scriptCount = in.read<qint32>();
        if (scriptCount < 0 || scriptCount > 1'000'000)
            return fail(QObject::tr("The script list is damaged."));
        for (qint32 i = 0; i < scriptCount; ++i) {
            in.read<qint32>();
            in.align();
            in.read<qint64>();
        }
        const qint32 externalCount = in.read<qint32>();
        if (externalCount < 0 || externalCount > 1'000'000)
            return fail(QObject::tr("The list of linked files is damaged."));
        for (qint32 i = 0; i < externalCount; ++i) {
            in.cstring();
            in.take(20);
            file.externals << in.cstring();
        }
        file.data = data;
        if (!file.hasTypeTrees)
            applyBuiltinTypeTrees(file);
        return file;
    } catch (const std::runtime_error &) {
        return fail(QObject::tr("The Unity file ends too early or is damaged."));
    }
}

const UValue *UValue::child(QStringView field) const
{
    for (const UValue &c : children)
        if (c.name == field)
            return &c;
    return nullptr;
}

const UValue *UValue::at(QStringView path) const
{
    const UValue *v = this;
    for (const QStringView part : path.split(u'.')) {
        v = v->child(part);
        if (!v)
            return nullptr;
    }
    return v;
}

qint64 UValue::toInt() const
{
    switch (kind) {
    case Kind::Float: return qint64(f);
    case Kind::UInt: return qint64(u);
    default: return i;
    }
}

double UValue::toDouble() const
{
    switch (kind) {
    case Kind::Float: return f;
    case Kind::UInt: return double(u);
    default: return double(i);
    }
}

QString UValue::toString() const
{
    return kind == Kind::String ? QString::fromUtf8(bytes) : QString();
}

qint64 UValue::count() const
{
    if (kind == Kind::Bytes)
        return elementSize > 0 ? bytes.size() / elementSize : 0;
    return qint64(children.size());
}

std::optional<UValue> readTypeTreeValue(const std::vector<TypeTreeNode> &nodes, QByteArrayView data, bool bigEndian,
                                        QString *error, qsizetype *consumed)
{
    if (nodes.empty()) {
        if (error)
            *error = QObject::tr("This Unity file has no type information, so its objects cannot be read.");
        return std::nullopt;
    }
    try {
        Reader in(data, bigEndian);
        ValueReader reader(nodes, in);
        UValue value = reader.read(0, 0);
        if (consumed)
            *consumed = in.pos();
        return value;
    } catch (const std::exception &) {
        if (error)
            *error = QObject::tr("The object does not match its type description.");
        return std::nullopt;
    }
}

QString objectName(const UValue &value)
{
    if (const UValue *name = value.child(u"m_Name"); name && name->kind == UValue::Kind::String)
        return name->toString();
    return {};
}

QString readObjectName(const std::vector<TypeTreeNode> &nodes, QByteArrayView data, bool bigEndian)
{
    if (nodes.empty())
        return {};
    bool hasName = false;
    for (int child : nodes.front().children)
        hasName = hasName || nodes.at(size_t(child)).name == QLatin1String("m_Name");
    if (!hasName)
        return {};
    try {
        Reader in(data, bigEndian);
        ValueReader reader(nodes, in);
        return reader.readName().value_or(QString());
    } catch (const std::exception &) {
        return {};
    }
}

QString unityClassName(qint32 classId)
{
    static const QHash<qint32, QString> names = {
        {1, QStringLiteral("GameObject")}, {4, QStringLiteral("Transform")}, {20, QStringLiteral("Camera")},
        {21, QStringLiteral("Material")}, {23, QStringLiteral("MeshRenderer")}, {28, QStringLiteral("Texture2D")},
        {33, QStringLiteral("MeshFilter")}, {43, QStringLiteral("Mesh")}, {48, QStringLiteral("Shader")},
        {49, QStringLiteral("TextAsset")}, {54, QStringLiteral("Rigidbody")}, {64, QStringLiteral("MeshCollider")},
        {65, QStringLiteral("BoxCollider")}, {74, QStringLiteral("AnimationClip")}, {82, QStringLiteral("AudioSource")},
        {83, QStringLiteral("AudioClip")}, {84, QStringLiteral("RenderTexture")}, {89, QStringLiteral("Cubemap")},
        {91, QStringLiteral("AnimatorController")}, {95, QStringLiteral("Animator")}, {104, QStringLiteral("RenderSettings")},
        {108, QStringLiteral("Light")}, {114, QStringLiteral("MonoBehaviour")}, {115, QStringLiteral("MonoScript")},
        {128, QStringLiteral("Font")}, {137, QStringLiteral("SkinnedMeshRenderer")}, {141, QStringLiteral("BuildSettings")},
        {142, QStringLiteral("AssetBundle")}, {150, QStringLiteral("PreloadData")}, {157, QStringLiteral("LightmapSettings")},
        {196, QStringLiteral("NavMeshSettings")}, {198, QStringLiteral("ParticleSystem")},
        {199, QStringLiteral("ParticleSystemRenderer")}, {212, QStringLiteral("SpriteRenderer")},
        {213, QStringLiteral("Sprite")}, {222, QStringLiteral("CanvasRenderer")}, {223, QStringLiteral("Canvas")},
        {224, QStringLiteral("RectTransform")}, {225, QStringLiteral("CanvasGroup")}, {328, QStringLiteral("VideoPlayer")},
        {329, QStringLiteral("VideoClip")}, {687078895, QStringLiteral("SpriteAtlas")},
    };
    return names.value(classId, QObject::tr("class %1").arg(classId));
}
