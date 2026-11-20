#include "data/nrbfreader.h"

#include "core/simpletable.h"
#include "core/treetable.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimeZone>
#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace {

enum class Primitive : quint8 {
    Boolean = 1, Byte = 2, Char = 3, Decimal = 5, Double = 6, Int16 = 7, Int32 = 8, Int64 = 9, SByte = 10,
    Single = 11, TimeSpan = 12, DateTime = 13, UInt16 = 14, UInt32 = 15, UInt64 = 16, Null = 17, String = 18,
};

struct Value
{
    enum class Kind { Null, Primitive, Reference };
    Kind kind = Kind::Null;
    QVariant primitive;
    qint32 id = 0;
};

struct ClassShape
{
    QString name;
    QStringList members;
    std::vector<quint8> binaryTypes;
    std::vector<quint8> primitiveTypes;
    bool typed = false;
};

struct Object
{
    enum class Kind { Class, Array, String };
    Kind kind = Kind::Class;
    QString className;
    QStringList members;
    std::vector<Value> values;
    QString text;
};

struct Graph
{
    QHash<qint32, Object> objects;
    qint32 root = 0;
};

class Parser
{
public:
    explicit Parser(QByteArrayView data) : m_data(data) {}

    Graph parse()
    {
        m_graph = Graph();
        bool started = false;
        while (true) {
            const quint8 type = u8();
            if (type == 0) {
                m_graph.root = i32();
                take(12);
                started = true;
                continue;
            }
            if (!started)
                throw std::runtime_error("header");
            if (type == 11)
                break;
            Value ignored;
            record(type, ignored);
        }
        return std::move(m_graph);
    }

private:
    quint8 u8() { return quint8(*take(1).data()); }
    qint32 i32() { return qFromLittleEndian<qint32>(take(4).data()); }
    QByteArrayView take(qsizetype n)
    {
        if (n < 0 || n > m_data.size() - m_pos)
            throw std::runtime_error("short");
        const QByteArrayView out = m_data.sliced(m_pos, n);
        m_pos += n;
        return out;
    }
    QString string()
    {
        qint64 length = 0;
        for (int shift = 0;; shift += 7) {
            if (shift > 28)
                throw std::runtime_error("length");
            const quint8 b = u8();
            length |= qint64(b & 0x7F) << shift;
            if (!(b & 0x80))
                break;
        }
        return QString::fromUtf8(take(length));
    }

    QVariant primitive(quint8 type)
    {
        switch (Primitive(type)) {
        case Primitive::Boolean: return u8() != 0;
        case Primitive::Byte: return int(u8());
        case Primitive::SByte: return int(qint8(u8()));
        case Primitive::Char: {
            const quint8 first = quint8(m_data.at(m_pos));
            const int length = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
            return QString::fromUtf8(take(length));
        }
        case Primitive::Decimal: return string();
        case Primitive::Double: {
            const quint64 bits = qFromLittleEndian<quint64>(take(8).data());
            double d;
            std::memcpy(&d, &bits, 8);
            return d;
        }
        case Primitive::Single: {
            const quint32 bits = qFromLittleEndian<quint32>(take(4).data());
            float f;
            std::memcpy(&f, &bits, 4);
            return double(f);
        }
        case Primitive::Int16: return int(qFromLittleEndian<qint16>(take(2).data()));
        case Primitive::UInt16: return int(qFromLittleEndian<quint16>(take(2).data()));
        case Primitive::Int32: return i32();
        case Primitive::UInt32: return qint64(qFromLittleEndian<quint32>(take(4).data()));
        case Primitive::Int64: return qFromLittleEndian<qint64>(take(8).data());
        case Primitive::UInt64: return qFromLittleEndian<quint64>(take(8).data());
        case Primitive::TimeSpan: return qFromLittleEndian<qint64>(take(8).data());
        case Primitive::DateTime: {
            const quint64 raw = qFromLittleEndian<quint64>(take(8).data());
            const qint64 ticks = qint64(raw & 0x3FFFFFFFFFFFFFFFull);
            return QDateTime::fromMSecsSinceEpoch((ticks - 621355968000000000LL) / 10000, QTimeZone::UTC)
                .toString(Qt::ISODateWithMs);
        }
        case Primitive::Null: return {};
        case Primitive::String: return string();
        }
        throw std::runtime_error("primitive");
    }

    ClassShape classInfo(qint32 &id)
    {
        ClassShape shape;
        id = i32();
        shape.name = string();
        const qint32 count = i32();
        if (count < 0 || count > 100'000)
            throw std::runtime_error("members");
        for (qint32 i = 0; i < count; ++i)
            shape.members << string();
        return shape;
    }

    void memberTypes(ClassShape &shape)
    {
        const qsizetype count = shape.members.size();
        shape.typed = true;
        for (qsizetype i = 0; i < count; ++i)
            shape.binaryTypes.push_back(u8());
        shape.primitiveTypes.assign(size_t(count), 0);
        for (qsizetype i = 0; i < count; ++i) {
            switch (shape.binaryTypes[size_t(i)]) {
            case 0:
            case 7: shape.primitiveTypes[size_t(i)] = u8(); break;
            case 3: string(); break;
            case 4: string(); i32(); break;
            default: break;
            }
        }
    }

    void additionalInfo(quint8 binaryType, quint8 &primitiveType)
    {
        switch (binaryType) {
        case 0:
        case 7: primitiveType = u8(); break;
        case 3: string(); break;
        case 4: string(); i32(); break;
        default: break;
        }
    }

    int nulls = 0;

    Value element()
    {
        if (nulls > 0) {
            --nulls;
            return {};
        }
        Value v;
        record(u8(), v);
        return v;
    }

    void classValues(qint32 id, const ClassShape &shape)
    {
        Object object;
        object.kind = Object::Kind::Class;
        object.className = shape.name;
        object.members = shape.members;
        for (qsizetype i = 0; i < shape.members.size(); ++i) {
            if (shape.typed && shape.binaryTypes[size_t(i)] == 0)
                object.values.push_back({Value::Kind::Primitive, primitive(shape.primitiveTypes[size_t(i)]), 0});
            else
                object.values.push_back(element());
        }
        m_graph.objects.insert(id, std::move(object));
    }

    void record(quint8 type, Value &out)
    {
        if (++m_depth > 512)
            throw std::runtime_error("deep");
        switch (type) {
        case 1: {
            const qint32 id = i32(), shapeId = i32();
            if (!m_shapes.contains(shapeId))
                throw std::runtime_error("shape");
            const ClassShape shape = m_shapes.value(shapeId);
            m_shapes.insert(id, shape);
            classValues(id, shape);
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        case 2:
        case 3:
        case 4:
        case 5: {
            qint32 id = 0;
            ClassShape shape = classInfo(id);
            if (type == 4 || type == 5)
                memberTypes(shape);
            if (type == 3 || type == 5)
                i32();
            m_shapes.insert(id, shape);
            classValues(id, shape);
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        case 6: {
            const qint32 id = i32();
            Object s;
            s.kind = Object::Kind::String;
            s.text = string();
            m_graph.objects.insert(id, std::move(s));
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        case 7: {
            const qint32 id = i32();
            const quint8 arrayType = u8();
            const qint32 rank = i32();
            if (rank < 1 || rank > 32)
                throw std::runtime_error("rank");
            qint64 total = 1;
            for (qint32 r = 0; r < rank; ++r) {
                const qint32 length = i32();
                if (length < 0)
                    throw std::runtime_error("length");
                total *= length;
                if (total > m_data.size())
                    throw std::runtime_error("length");
            }
            if (arrayType >= 3)
                take(qsizetype(rank) * 4);
            const quint8 binaryType = u8();
            quint8 primitiveType = 0;
            additionalInfo(binaryType, primitiveType);
            Object array;
            array.kind = Object::Kind::Array;
            for (qint64 i = 0; i < total; ++i)
                array.values.push_back(binaryType == 0 ? Value{Value::Kind::Primitive, primitive(primitiveType), 0} : element());
            m_graph.objects.insert(id, std::move(array));
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        case 8: {
            const quint8 p = u8();
            out = {Value::Kind::Primitive, primitive(p), 0};
            break;
        }
        case 9: out = {Value::Kind::Reference, {}, i32()}; break;
        case 10: out = {}; break;
        case 12: {
            i32();
            string();
            record(u8(), out);
            break;
        }
        case 13: nulls = int(u8()) - 1; out = {}; break;
        case 14: nulls = i32() - 1; out = {}; break;
        case 15: {
            const qint32 id = i32(), length = i32();
            const quint8 p = u8();
            if (length < 0 || length > m_data.size())
                throw std::runtime_error("length");
            Object array;
            array.kind = Object::Kind::Array;
            array.values.reserve(size_t(length));
            for (qint32 i = 0; i < length; ++i)
                array.values.push_back({Value::Kind::Primitive, primitive(p), 0});
            m_graph.objects.insert(id, std::move(array));
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        case 16:
        case 17: {
            const qint32 id = i32(), length = i32();
            if (length < 0 || length > m_data.size())
                throw std::runtime_error("length");
            Object array;
            array.kind = Object::Kind::Array;
            for (qint32 i = 0; i < length; ++i)
                array.values.push_back(element());
            m_graph.objects.insert(id, std::move(array));
            out = {Value::Kind::Reference, {}, id};
            break;
        }
        default:
            throw std::runtime_error("record");
        }
        --m_depth;
    }

    QByteArrayView m_data;
    qsizetype m_pos = 0;
    int m_depth = 0;
    Graph m_graph;
    QHash<qint32, ClassShape> m_shapes;
};

QString shortClassName(const QString &name)
{
    const qsizetype generic = name.indexOf(u'`');
    QString base = generic >= 0 ? name.left(generic) : name;
    base = base.section(u'.', -1).section(u'+', -1);
    const qsizetype open = name.indexOf(QLatin1String("[["));
    if (open >= 0) {
        const QString inner = name.mid(open + 2).section(u',', 0, 0);
        return QStringLiteral("%1<%2>").arg(base, shortClassName(inner));
    }
    return base;
}

class View
{
public:
    explicit View(const Graph &graph) : m_graph(graph) {}

    const Object *object(const Value &v) const
    {
        if (v.kind != Value::Kind::Reference)
            return nullptr;
        auto it = m_graph.objects.constFind(v.id);
        return it == m_graph.objects.cend() ? nullptr : &*it;
    }

    const Object *unwrapList(const Object *o) const
    {
        if (o && o->kind == Object::Kind::Class && o->members.contains(QLatin1String("_items"))
            && o->members.contains(QLatin1String("_size"))) {
            const Value &items = o->values[size_t(o->members.indexOf(QLatin1String("_items")))];
            return object(items);
        }
        return o;
    }

    qint64 listSize(const Object *o) const
    {
        if (o && o->kind == Object::Kind::Class && o->members.contains(QLatin1String("_size")))
            return o->values[size_t(o->members.indexOf(QLatin1String("_size")))].primitive.toLongLong();
        return -1;
    }

    QString cell(const Value &v, int depth = 0) const
    {
        if (v.kind == Value::Kind::Null)
            return {};
        if (v.kind == Value::Kind::Primitive)
            return v.primitive.toString();
        const Object *o = object(v);
        if (!o)
            return QObject::tr("(missing #%1)").arg(v.id);
        if (o->kind == Object::Kind::String)
            return o->text;
        const qint64 size = listSize(o);
        const Object *items = unwrapList(o);
        if (items && items->kind == Object::Kind::Array) {
            const qsizetype count = size >= 0 ? qsizetype(size) : qsizetype(items->values.size());
            if (depth > 1)
                return QObject::tr("[%n item(s)]", nullptr, int(count));
            QStringList parts;
            for (qsizetype i = 0; i < std::min<qsizetype>(count, 32); ++i)
                parts << cell(items->values[size_t(i)], depth + 1);
            return QStringLiteral("[") + parts.join(QStringLiteral(", ")) + (count > 32 ? QStringLiteral(", …]") : QStringLiteral("]"));
        }
        if (depth > 1)
            return QStringLiteral("{…}");
        QStringList parts;
        for (qsizetype i = 0; i < o->members.size(); ++i)
            parts << QStringLiteral("%1: %2").arg(cleanMember(o->members[i]), cell(o->values[size_t(i)], depth + 1));
        return QStringLiteral("{") + parts.join(QStringLiteral(", ")) + QStringLiteral("}");
    }

    static QString cleanMember(const QString &name)
    {
        if (name.startsWith(u'<') && name.endsWith(QLatin1String(">k__BackingField")))
            return name.mid(1, name.size() - 17);
        return name;
    }

    QJsonValue json(const Value &v, QSet<qint32> &path) const
    {
        if (v.kind == Value::Kind::Null)
            return QJsonValue::Null;
        if (v.kind == Value::Kind::Primitive)
            return QJsonValue::fromVariant(v.primitive);
        const Object *o = object(v);
        if (!o || path.contains(v.id))
            return QStringLiteral("#%1").arg(v.id);
        if (o->kind == Object::Kind::String)
            return o->text;
        path.insert(v.id);
        QJsonValue out;
        const qint64 size = listSize(o);
        const Object *items = unwrapList(o);
        if (items && items->kind == Object::Kind::Array) {
            QJsonArray array;
            const qsizetype count = size >= 0 ? std::min<qsizetype>(size, items->values.size()) : items->values.size();
            for (qsizetype i = 0; i < count; ++i)
                array.append(json(items->values[size_t(i)], path));
            out = array;
        } else {
            QJsonObject object;
            for (qsizetype i = 0; i < o->members.size(); ++i)
                object.insert(cleanMember(o->members[i]), json(o->values[size_t(i)], path));
            out = object;
        }
        path.remove(v.id);
        return out;
    }

    void flatten(const Value &v, const QString &path, std::vector<TreeRow> &rows, QSet<qint32> &seen) const
    {
        if (rows.size() >= 200'000)
            return;
        const Object *o = object(v);
        if (!o || o->kind == Object::Kind::String || v.kind != Value::Kind::Reference) {
            const QString text = cell(v);
            QVariant key;
            if (v.kind == Value::Kind::Primitive && v.primitive.canConvert<double>() && v.primitive.typeId() != QMetaType::QString
                && v.primitive.typeId() != QMetaType::Bool)
                key = v.primitive.toDouble();
            rows.push_back({path, v.kind == Value::Kind::Null ? QStringLiteral("null")
                                  : v.kind == Value::Kind::Primitive ? QString::fromLatin1(v.primitive.typeName())
                                                                     : QStringLiteral("string"),
                            text, key});
            return;
        }
        if (seen.contains(v.id)) {
            rows.push_back({path, QStringLiteral("reference"), QStringLiteral("#%1").arg(v.id), {}});
            return;
        }
        seen.insert(v.id);
        const qint64 size = listSize(o);
        const Object *items = unwrapList(o);
        if (items && items->kind == Object::Kind::Array) {
            const qsizetype count = size >= 0 ? std::min<qsizetype>(size, items->values.size()) : items->values.size();
            rows.push_back({path, QStringLiteral("array"), QObject::tr("%n item(s)", nullptr, int(count)), {}});
            for (qsizetype i = 0; i < count; ++i)
                flatten(items->values[size_t(i)], QStringLiteral("%1[%2]").arg(path).arg(i), rows, seen);
        } else {
            rows.push_back({path, shortClassName(o->className), {}, {}});
            for (qsizetype i = 0; i < o->members.size(); ++i)
                flatten(o->values[size_t(i)], path.isEmpty() ? cleanMember(o->members[i]) : path + u'.' + cleanMember(o->members[i]),
                        rows, seen);
        }
    }

    QString mostCommonRecordClass() const
    {
        QHash<QString, qsizetype> counts;
        for (auto it = m_graph.objects.cbegin(); it != m_graph.objects.cend(); ++it)
            if (it->kind == Object::Kind::Array)
                for (const Value &v : it->values)
                    if (const Object *o = object(v); o && o->kind == Object::Kind::Class)
                        ++counts[o->className];
        QString best;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            if (best.isEmpty() || it.value() > counts.value(best))
                best = it.key();
        return best;
    }

    void collect(const Value &v, const QString &className, const QString &label,
                 std::vector<std::pair<const Object *, QString>> &out, QSet<qint32> &seen) const
    {
        const Object *o = object(v);
        if (!o || o->kind == Object::Kind::String || seen.contains(v.id))
            return;
        seen.insert(v.id);
        if (o->kind == Object::Kind::Array) {
            for (const Value &e : o->values)
                collect(e, className, label, out, seen);
            return;
        }
        if (o->className == className) {
            out.emplace_back(o, label);
            return;
        }
        QString inner = label;
        for (const Value &m : o->values)
            if (const Object *s = object(m); s && s->kind == Object::Kind::String) {
                inner = s->text;
                break;
            } else if (m.kind == Value::Kind::Primitive && m.primitive.typeId() == QMetaType::QString) {
                inner = m.primitive.toString();
                break;
            }
        for (const Value &m : o->values)
            collect(m, className, inner, out, seen);
    }

private:
    const Graph &m_graph;
};

QByteArray rootJson(const Graph &graph, const Value &root)
{
    QSet<qint32> path;
    const QJsonValue value = View(graph).json(root, path);
    return value.isArray() ? QJsonDocument(value.toArray()).toJson() : QJsonDocument(value.toObject()).toJson();
}

}

int NrbfReader::score(const QString &, QByteArrayView head) const
{
    if (head.size() < 17 || head[0] != 0)
        return 0;
    const qint32 major = qFromLittleEndian<qint32>(head.data() + 9), minor = qFromLittleEndian<qint32>(head.data() + 13);
    return major == 1 && minor == 0 ? 95 : 0;
}

std::unique_ptr<Document> NrbfReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 256 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> NrbfReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    auto graph = std::make_shared<Graph>();
    try {
        *graph = Parser(data).parse();
    } catch (const std::exception &) {
        if (error)
            *error = QObject::tr("The BinaryFormatter data is damaged or uses records this viewer does not read.");
        return nullptr;
    }
    View view(*graph);
    const Value root{Value::Kind::Reference, {}, graph->root};
    auto doc = std::make_unique<Document>();

    const QString className = view.mostCommonRecordClass();
    std::vector<std::pair<const Object *, QString>> records;
    QSet<qint32> visited;
    if (!className.isEmpty())
        view.collect(root, className, {}, records, visited);
    if (!records.empty()) {
        const QStringList members = records.front().first->members;
        QSet<QString> groups;
        for (const auto &r : records)
            groups.insert(r.second);
        const bool grouped = groups.size() > 1;
        std::vector<SimpleColumn> columns;
        if (grouped)
            columns.push_back({QObject::tr("group"), false, QObject::tr("The first text field of the object that holds this record")});
        for (const QString &m : members)
            columns.push_back({View::cleanMember(m)});
        std::vector<SimpleTable::Row> rows;
        rows.reserve(records.size());
        for (const auto &[o, group] : records) {
            SimpleTable::Row row;
            if (grouped)
                row.push_back(group);
            for (qsizetype i = 0; i < members.size(); ++i) {
                const Value &cell = i < o->members.size() ? o->values[size_t(i)] : Value{};
                if (cell.kind == Value::Kind::Primitive && cell.primitive.typeId() != QMetaType::Bool)
                    row.push_back(cell.primitive);
                else
                    row.push_back(view.cell(cell));
            }
            rows.push_back(std::move(row));
        }
        auto table = std::make_shared<SimpleTable>(shortClassName(className), std::move(columns), std::move(rows));
        QString description = QObject::tr("%n %1 record(s)", nullptr, int(table->rowCount())).arg(shortClassName(className));
        if (grouped)
            description += QObject::tr(" in %n group(s)", nullptr, int(groups.size()));
        table->setDescription(description);
        table->setJsonExporter([graph, root] { return rootJson(*graph, root); });
        doc->tables.push_back(std::move(table));
    }

    std::vector<TreeRow> rows;
    QSet<qint32> seen;
    view.flatten(root, {}, rows, seen);
    doc->tables.push_back(std::make_shared<TreeTable>(QObject::tr("All values"), std::move(rows),
                                                      [graph, root] { return rootJson(*graph, root); }));
    return doc;
}
