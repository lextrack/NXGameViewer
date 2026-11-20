#include "data/bymlwriter.h"

#include "data/bymlformat.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <vector>

namespace {

using namespace byml;

constexpr int kMaxDepth = 256;
constexpr qint64 kMaxOutput = qint64{0xFFFFFFFF};

[[noreturn]] void failAt(const QString &path, const QString &message)
{
    throw ParseFailure((path.isEmpty() ? message : QObject::tr("%1: %2").arg(path, message)).toStdString());
}

struct Node
{
    quint8 type = Null;
    quint64 bits = 0;
    QString text;
    QByteArray bytes;
    quint32 alignment = 0;
    std::vector<Node> items;
    std::vector<QString> keys;
    std::vector<quint32> hashes;
    std::vector<quint32> remap;
};

class TreeReader : public Bytes
{
public:
    using Bytes::Bytes;

    Node root()
    {
        const quint32 keys = u32(4);
        const quint32 strings = u32(8);
        const quint32 root = u32(12);
        if (version() < 4 && root != 0 && u8(root) == PathTable)
            failAt({}, QObject::tr("BYML files with a path table cannot be written yet."));
        if (keys)
            m_keys = stringTable(keys);
        if (strings)
            m_strings = stringTable(strings);
        if (root == 0)
            return {};
        return container(root, 0);
    }

private:
    Node container(qint64 at, int depth)
    {
        if (depth > kMaxDepth)
            failAt({}, QObject::tr("The original file is nested too deeply."));
        Node n;
        n.type = u8(at);
        const quint32 count = u24(at + 1);
        switch (n.type) {
        case Array: {
            need(at + 4, count);
            const qint64 values = at + 4 + ((qint64(count) + 3) & ~qint64(3));
            need(values, qint64(count) * 4);
            for (quint32 i = 0; i < count; ++i)
                n.items.push_back(value(u8(at + 4 + i), values + qint64(i) * 4, depth));
            break;
        }
        case Dict:
            need(at + 4, qint64(count) * 8);
            for (quint32 i = 0; i < count; ++i) {
                const qint64 e = at + 4 + qint64(i) * 8;
                const quint32 key = u24(e);
                if (key >= quint32(m_keys.size()))
                    failAt({}, QObject::tr("A key of the original file is missing from its key table."));
                n.keys.push_back(m_keys[key]);
                n.items.push_back(value(u8(e + 3), e + 4, depth));
            }
            break;
        case HashDict:
        case HashDictRemap: {
            const qint64 stride = n.type == HashDict ? 8 : 12;
            need(at + 4, qint64(count) * stride + count);
            const qint64 types = at + 4 + qint64(count) * stride;
            for (quint32 i = 0; i < count; ++i) {
                const qint64 e = at + 4 + qint64(i) * stride;
                n.hashes.push_back(u32(e));
                if (n.type == HashDictRemap)
                    n.remap.push_back(u32(e + 8));
                n.items.push_back(value(u8(types + i), e + 4, depth));
            }
            break;
        }
        default:
            failAt({}, QObject::tr("Unknown BYML container type 0x%1.").arg(n.type, 2, 16, QLatin1Char('0')));
        }
        return n;
    }

    Node value(quint8 type, qint64 at, int depth)
    {
        const quint32 raw = u32(at);
        if (isContainer(type))
            return container(raw, depth + 1);
        Node n;
        n.type = type;
        switch (type) {
        case String:
            if (raw >= quint32(m_strings.size()))
                failAt({}, QObject::tr("A string of the original file is missing from its string table."));
            n.text = m_strings[raw];
            break;
        case Binary:
        case FileData: {
            const quint32 size = u32(raw);
            const qint64 start = raw + (type == FileData ? 8 : 4);
            if (type == FileData)
                n.alignment = u32(raw + 4);
            need(start, size);
            n.bytes = data().mid(start, size);
            break;
        }
        case Int64:
        case UInt64:
        case Double:
            n.bits = u64(raw);
            break;
        default:
            n.bits = raw;
            break;
        }
        return n;
    }

    QStringList m_keys;
    QStringList m_strings;
};

QString childPath(const QString &path, const QString &key)
{
    return path.isEmpty() ? key : path + u'.' + key;
}

std::optional<qint64> signedWhole(const QJsonValue &v)
{
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().trimmed().toLongLong(&ok);
        return ok ? std::optional<qint64>(n) : std::nullopt;
    }
    if (!v.isDouble())
        return std::nullopt;
    const double d = v.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < -9.2233720368547758e18 || d >= 9.2233720368547758e18)
        return std::nullopt;
    return qint64(d);
}

std::optional<quint64> unsignedWhole(const QJsonValue &v)
{
    if (v.isString()) {
        bool ok = false;
        const quint64 n = v.toString().trimmed().toULongLong(&ok);
        return ok ? std::optional<quint64>(n) : std::nullopt;
    }
    if (!v.isDouble())
        return std::nullopt;
    const double d = v.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < 0 || d >= 1.8446744073709552e19)
        return std::nullopt;
    return quint64(d);
}

Node infer(const QJsonValue &v, const QString &path, int depth);
Node merge(const QJsonValue &v, const Node *schema, const QString &path, int depth);

Node infer(const QJsonValue &v, const QString &path, int depth)
{
    if (depth > kMaxDepth)
        failAt(path, QObject::tr("The data is nested too deeply."));
    Node n;
    if (v.isBool()) {
        n.type = Bool;
        n.bits = v.toBool() ? 1 : 0;
    } else if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::floor(d) == d && d >= std::numeric_limits<qint32>::min() && d <= std::numeric_limits<qint32>::max()) {
            n.type = Int;
            n.bits = quint32(qint32(d));
        } else {
            n.type = Float;
            n.bits = std::bit_cast<quint32>(float(d));
        }
    } else if (v.isString()) {
        n.type = String;
        n.text = v.toString();
    } else if (v.isArray()) {
        n.type = Array;
        const QJsonArray a = v.toArray();
        for (qsizetype i = 0; i < a.size(); ++i)
            n.items.push_back(merge(a[i], nullptr, QStringLiteral("%1[%2]").arg(path).arg(i), depth + 1));
    } else if (v.isObject()) {
        n.type = Dict;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            n.keys.push_back(it.key());
            n.items.push_back(merge(it.value(), nullptr, childPath(path, it.key()), depth + 1));
        }
    }
    return n;
}

std::optional<quint32> hashKey(const QString &key)
{
    QString digits = key.trimmed();
    if (digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        digits = digits.mid(2);
    bool ok = false;
    const quint32 hash = digits.toUInt(&ok, 16);
    return ok && !digits.isEmpty() ? std::optional<quint32>(hash) : std::nullopt;
}

Node merge(const QJsonValue &v, const Node *schema, const QString &path, int depth)
{
    if (depth > kMaxDepth)
        failAt(path, QObject::tr("The data is nested too deeply."));
    if (!schema || (schema->type == Null && !v.isNull()))
        return infer(v, path, depth);
    Node n;
    n.type = schema->type;
    switch (schema->type) {
    case Array: {
        if (!v.isArray())
            failAt(path, QObject::tr("should be a list [ … ], like in the game's file."));
        const QJsonArray a = v.toArray();
        const bool sameType = !schema->items.empty()
                           && std::all_of(schema->items.begin(), schema->items.end(),
                                          [&](const Node &item) { return item.type == schema->items.front().type; });
        for (qsizetype i = 0; i < a.size(); ++i) {
            const Node *child = size_t(i) < schema->items.size() ? &schema->items[size_t(i)]
                              : sameType                         ? &schema->items.back()
                                                                 : nullptr;
            n.items.push_back(merge(a[i], child, QStringLiteral("%1[%2]").arg(path).arg(i), depth + 1));
        }
        return n;
    }
    case Dict: {
        if (!v.isObject())
            failAt(path, QObject::tr("should be an object { … }, like in the game's file."));
        QHash<QString, size_t> known;
        for (size_t i = 0; i < schema->keys.size(); ++i)
            known.insert(schema->keys[i], i);
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const auto found = known.constFind(it.key());
            n.keys.push_back(it.key());
            n.items.push_back(merge(it.value(), found == known.cend() ? nullptr : &schema->items[*found],
                                    childPath(path, it.key()), depth + 1));
        }
        return n;
    }
    case HashDict:
    case HashDictRemap: {
        if (!v.isObject())
            failAt(path, QObject::tr("should be an object { … }, like in the game's file."));
        QHash<quint32, size_t> known;
        for (size_t i = 0; i < schema->hashes.size(); ++i)
            known.insert(schema->hashes[i], i);
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const std::optional<quint32> hash = hashKey(it.key());
            if (!hash)
                failAt(childPath(path, it.key()), QObject::tr("keys of this table are hashes, written like 0x1234abcd."));
            const auto found = known.constFind(*hash);
            if (found == known.cend() && schema->type == HashDictRemap)
                failAt(childPath(path, it.key()), QObject::tr("new keys cannot be added to this kind of hash table."));
            n.hashes.push_back(*hash);
            if (schema->type == HashDictRemap)
                n.remap.push_back(schema->remap[*found]);
            n.items.push_back(merge(it.value(), found == known.cend() ? nullptr : &schema->items[*found],
                                    childPath(path, it.key()), depth + 1));
        }
        return n;
    }
    case Bool:
        if (!v.isBool())
            failAt(path, QObject::tr("should be true or false."));
        n.bits = v.toBool() ? 1 : 0;
        return n;
    case Int: {
        const std::optional<qint64> x = signedWhole(v);
        if (!x || *x < std::numeric_limits<qint32>::min() || *x > std::numeric_limits<qint32>::max())
            failAt(path, QObject::tr("should be a whole number from -2147483648 to 2147483647."));
        n.bits = quint32(qint32(*x));
        return n;
    }
    case UInt: {
        const std::optional<quint64> x = unsignedWhole(v);
        if (!x || *x > std::numeric_limits<quint32>::max())
            failAt(path, QObject::tr("should be a whole number from 0 to 4294967295."));
        n.bits = *x;
        return n;
    }
    case Int64: {
        const std::optional<qint64> x = signedWhole(v);
        if (!x)
            failAt(path, QObject::tr("should be a whole number (64-bit)."));
        n.bits = quint64(*x);
        return n;
    }
    case UInt64: {
        const std::optional<quint64> x = unsignedWhole(v);
        if (!x)
            failAt(path, QObject::tr("should be a whole number of 0 or more (64-bit)."));
        n.bits = *x;
        return n;
    }
    case Float:
        if (v.isNull())
            return *schema;
        if (!v.isDouble())
            failAt(path, QObject::tr("should be a number."));
        n.bits = std::bit_cast<quint32>(float(v.toDouble()));
        return n;
    case Double:
        if (v.isNull())
            return *schema;
        if (!v.isDouble())
            failAt(path, QObject::tr("should be a number."));
        n.bits = std::bit_cast<quint64>(v.toDouble());
        return n;
    case String:
        if (!v.isString())
            failAt(path, QObject::tr("should be text in quotes."));
        n.text = v.toString();
        return n;
    case Binary:
    case FileData: {
        if (!v.isString())
            failAt(path, QObject::tr("should be binary data written as base64 text."));
        const auto decoded = QByteArray::fromBase64Encoding(v.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            failAt(path, QObject::tr("is not valid base64 text."));
        n.bytes = *decoded;
        n.alignment = schema->alignment;
        return n;
    }
    case Null:
        return n;
    default:
        if (v.isNull())
            return *schema;
        failAt(path, QObject::tr("has a type this app cannot change (0x%1).").arg(schema->type, 2, 16, QLatin1Char('0')));
    }
}

class Writer
{
public:
    Writer(bool little, quint16 version) : m_little(little), m_version(version) {}

    QByteArray write(const Node &root)
    {
        if (root.type != Null && !isContainer(root.type))
            failAt({}, QObject::tr("The top of a BYML file must be a list or an object."));
        std::set<QByteArray> keys, strings;
        collect(root, keys, strings, 0);
        m_out = QByteArray(16, '\0');
        m_out.replace(0, 2, m_little ? "YB" : "BY");
        put16(2, m_version);
        if (!keys.empty())
            put32(4, table(keys, m_keyIndex));
        if (!strings.empty())
            put32(8, table(strings, m_stringIndex));
        if (root.type != Null)
            put32(12, container(root));
        return m_out;
    }

private:
    void collect(const Node &n, std::set<QByteArray> &keys, std::set<QByteArray> &strings, int depth)
    {
        if (depth > kMaxDepth)
            failAt({}, QObject::tr("The data is nested too deeply."));
        if (n.type == String)
            strings.insert(n.text.toUtf8());
        for (const QString &k : n.keys)
            keys.insert(k.toUtf8());
        for (const Node &c : n.items)
            collect(c, keys, strings, depth + 1);
    }

    void align(int to)
    {
        while (m_out.size() % to)
            m_out.append('\0');
        if (m_out.size() > kMaxOutput)
            failAt({}, QObject::tr("The BYML file would be larger than 4 GB."));
    }
    void put16(qsizetype at, quint16 v)
    {
        m_little ? qToLittleEndian(v, m_out.data() + at) : qToBigEndian(v, m_out.data() + at);
    }
    void put32(qsizetype at, quint32 v)
    {
        m_little ? qToLittleEndian(v, m_out.data() + at) : qToBigEndian(v, m_out.data() + at);
    }
    void put64(qsizetype at, quint64 v)
    {
        m_little ? qToLittleEndian(v, m_out.data() + at) : qToBigEndian(v, m_out.data() + at);
    }
    void put24(qsizetype at, quint32 v)
    {
        auto *p = reinterpret_cast<uchar *>(m_out.data() + at);
        if (m_little) {
            p[0] = uchar(v);
            p[1] = uchar(v >> 8);
            p[2] = uchar(v >> 16);
        } else {
            p[0] = uchar(v >> 16);
            p[1] = uchar(v >> 8);
            p[2] = uchar(v);
        }
    }
    qsizetype grow(qsizetype bytes)
    {
        const qsizetype at = m_out.size();
        m_out.append(QByteArray(bytes, '\0'));
        if (m_out.size() > kMaxOutput)
            failAt({}, QObject::tr("The BYML file would be larger than 4 GB."));
        return at;
    }
    void header(qsizetype at, quint8 type, size_t count)
    {
        if (count > 0xFFFFFF)
            failAt({}, QObject::tr("A list or object has more than 16777215 entries."));
        m_out[at] = char(type);
        put24(at + 1, quint32(count));
    }

    quint32 table(const std::set<QByteArray> &values, std::map<QByteArray, quint32> &index)
    {
        align(4);
        const qsizetype start = grow(4 + qsizetype(values.size() + 1) * 4);
        header(start, StringTable, values.size());
        quint32 i = 0;
        for (const QByteArray &v : values) {
            put32(start + 4 + qsizetype(i) * 4, quint32(m_out.size() - start));
            index.emplace(v, i++);
            m_out.append(v);
            m_out.append('\0');
        }
        put32(start + 4 + qsizetype(i) * 4, quint32(m_out.size() - start));
        align(4);
        return quint32(start);
    }

    quint32 container(const Node &n)
    {
        align(4);
        const size_t count = n.items.size();
        std::vector<size_t> order(count);
        std::iota(order.begin(), order.end(), size_t(0));
        if (n.type == Array) {
            const qsizetype at = grow(4 + ((qsizetype(count) + 3) & ~qsizetype(3)) + qsizetype(count) * 4);
            header(at, Array, count);
            const qsizetype values = at + 4 + ((qsizetype(count) + 3) & ~qsizetype(3));
            for (size_t i = 0; i < count; ++i)
                m_out[at + 4 + qsizetype(i)] = char(n.items[i].type);
            for (size_t i = 0; i < count; ++i) {
                const quint32 v = value(n.items[i]);
                put32(values + qsizetype(i) * 4, v);
            }
            return quint32(at);
        }
        if (n.type == Dict) {
            std::vector<QByteArray> utf8;
            for (const QString &k : n.keys)
                utf8.push_back(k.toUtf8());
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return utf8[a] < utf8[b]; });
            for (size_t i = 1; i < count; ++i)
                if (utf8[order[i]] == utf8[order[i - 1]])
                    failAt(n.keys[order[i]], QObject::tr("this key appears twice."));
            const qsizetype at = grow(4 + qsizetype(count) * 8);
            header(at, Dict, count);
            for (size_t i = 0; i < count; ++i) {
                const size_t k = order[i];
                const qsizetype e = at + 4 + qsizetype(i) * 8;
                put24(e, m_keyIndex.at(utf8[k]));
                m_out[e + 3] = char(n.items[k].type);
                const quint32 v = value(n.items[k]);
                put32(e + 4, v);
            }
            return quint32(at);
        }
        const qsizetype stride = n.type == HashDict ? 8 : 12;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return n.hashes[a] < n.hashes[b]; });
        for (size_t i = 1; i < count; ++i)
            if (n.hashes[order[i]] == n.hashes[order[i - 1]])
                failAt(QStringLiteral("0x%1").arg(n.hashes[order[i]], 8, 16, QLatin1Char('0')), QObject::tr("this key appears twice."));
        const qsizetype at = grow(4 + qsizetype(count) * stride + ((qsizetype(count) + 3) & ~qsizetype(3)));
        header(at, n.type, count);
        const qsizetype types = at + 4 + qsizetype(count) * stride;
        for (size_t i = 0; i < count; ++i) {
            const size_t k = order[i];
            const qsizetype e = at + 4 + qsizetype(i) * stride;
            put32(e, n.hashes[k]);
            if (n.type == HashDictRemap)
                put32(e + 8, n.remap[k]);
            m_out[types + qsizetype(i)] = char(n.items[k].type);
            const quint32 v = value(n.items[k]);
            put32(e + 4, v);
        }
        return quint32(at);
    }

    quint32 value(const Node &n)
    {
        if (isContainer(n.type))
            return container(n);
        switch (n.type) {
        case String: return m_stringIndex.at(n.text.toUtf8());
        case Int64:
        case UInt64:
        case Double: {
            align(8);
            const qsizetype at = grow(8);
            put64(at, n.bits);
            return quint32(at);
        }
        case Binary: {
            align(4);
            const qsizetype at = grow(4);
            put32(at, quint32(n.bytes.size()));
            m_out.append(n.bytes);
            return quint32(at);
        }
        case FileData: {
            const qsizetype to = n.alignment > 1 && n.alignment <= 0x10000 ? qsizetype(n.alignment) : 4;
            align(4);
            while ((m_out.size() + 8) % to)
                m_out.append('\0');
            const qsizetype at = grow(8);
            put32(at, quint32(n.bytes.size()));
            put32(at + 4, n.alignment);
            m_out.append(n.bytes);
            return quint32(at);
        }
        case Null: return 0;
        default: return quint32(n.bits);
        }
    }

    bool m_little;
    quint16 m_version;
    QByteArray m_out;
    std::map<QByteArray, quint32> m_keyIndex;
    std::map<QByteArray, quint32> m_stringIndex;
};

}

std::optional<QByteArray> bymlFromJson(const QJsonValue &edited, const QByteArray &original, QString *error)
{
    if (original.size() < 16 || !(original.startsWith("YB") || original.startsWith("BY"))) {
        if (error)
            *error = QObject::tr("The game's file is not a BYML file.");
        return std::nullopt;
    }
    try {
        TreeReader reader(original);
        const Node schema = reader.root();
        const bool emptyRoot = schema.type == Null && edited.isArray() && edited.toArray().size() == 1
                            && edited.toArray().first().isNull();
        const Node merged = emptyRoot ? Node{} : merge(edited, &schema, {}, 0);
        return Writer(reader.little(), reader.version()).write(merged);
    } catch (const ParseFailure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory to write this BYML file.");
    }
    return std::nullopt;
}
