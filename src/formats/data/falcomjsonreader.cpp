#include "data/falcomjsonreader.h"
#include "core/storedreader.h"
#include "core/treetable.h"

#include <QObject>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <cstring>

namespace {

struct Node
{
    enum class Type { String, Number, Bool, Object, Array };

    Type type = Type::Object;
    bool hasKey = false;
    QString key;
    QString text;
    double number = 0;
    bool flag = false;
    std::vector<Node> children;
};

QString hex(qsizetype v)
{
    return QStringLiteral("0x") + QString::number(v, 16).toUpper();
}

class Parser
{
public:
    Parser(QByteArray data, FalcomJsonLimits limits) : m_d(std::move(data)), m_limits(limits) {}

    bool parse(Node &root, QString *error)
    {
        quint32 rootOffset = 0;
        const bool ok = m_d.size() >= 16 && m_d.startsWith("JSON") && readU32(8, rootOffset)
                     && node(rootOffset, 0, root);
        if (!ok && error)
            *error = m_error.isEmpty() ? QObject::tr("Not a Falcom binary JSON file.") : m_error;
        return ok;
    }

private:
    bool fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
        return false;
    }

    bool readU32(qsizetype pos, quint32 &value)
    {
        if (pos < 0 || pos + 4 > m_d.size())
            return fail(QObject::tr("Unexpected end of file at %1.").arg(hex(pos)));
        value = qFromLittleEndian<quint32>(m_d.constData() + pos);
        return true;
    }

    bool readText(qsizetype pos, QString &text)
    {
        if (pos < 0 || pos >= m_d.size())
            return fail(QObject::tr("Text at %1 is outside the file.").arg(hex(pos)));
        const char *begin = m_d.constData() + pos;
        const void *end = std::memchr(begin, 0, size_t(m_d.size() - pos));
        if (!end)
            return fail(QObject::tr("Text at %1 has no end.").arg(hex(pos)));
        text = QString::fromUtf8(begin, static_cast<const char *>(end) - begin);
        return true;
    }

    bool node(qsizetype offset, int depth, Node &out)
    {
        if (depth > m_limits.maxDepth)
            return fail(QObject::tr("Values are nested too deeply (broken file or a loop)."));
        if (offset < 16 || offset >= m_d.size())
            return fail(QObject::tr("A value points outside the file (%1).").arg(hex(offset)));

        const auto tag = quint8(m_d.at(offset));
        if (tag & 0xE0)
            return fail(QObject::tr("Unknown value tag 0x%1 at %2.").arg(tag, 2, 16, QLatin1Char('0')).arg(hex(offset)));

        qsizetype p = offset + 1;
        out.hasKey = !(tag & 0x10);
        if (out.hasKey) {
            quint32 keyOffset = 0;
            if (!readU32(p, keyOffset) || !readText(qsizetype(keyOffset) + 4, out.key))
                return false;
            p += 4;
        }

        switch (tag & 0x0F) {
        case 2:
            out.type = Node::Type::String;
            return readText(p, out.text);
        case 3:
            if (p + 8 > m_d.size())
                return fail(QObject::tr("Unexpected end of file at %1.").arg(hex(p)));
            out.type = Node::Type::Number;
            out.number = std::bit_cast<double>(qFromLittleEndian<quint64>(m_d.constData() + p));
            return true;
        case 6:
            if (p >= m_d.size())
                return fail(QObject::tr("Unexpected end of file at %1.").arg(hex(p)));
            out.type = Node::Type::Bool;
            out.flag = m_d.at(p) != 0;
            return true;
        case 4:
        case 5: {
            out.type = (tag & 0x0F) == 4 ? Node::Type::Object : Node::Type::Array;
            quint32 count = 0;
            if (!readU32(p, count))
                return false;
            p += 4;
            if (p + qsizetype(count) * 4 > m_d.size())
                return fail(QObject::tr("A list at %1 runs past the end of the file.").arg(hex(offset)));
            m_valueCount += count;
            if (m_valueCount > m_limits.maxValues)
                return fail(QObject::tr("Too many values (more than %L1).").arg(m_limits.maxValues));
            out.children.resize(count);
            for (quint32 i = 0; i < count; ++i) {
                quint32 child = 0;
                if (!readU32(p + qsizetype(i) * 4, child) || !node(child, depth + 1, out.children[i]))
                    return false;
            }
            return true;
        }
        default:
            return fail(QObject::tr("Unknown value type 0x%1 at %2.").arg(tag, 2, 16, QLatin1Char('0')).arg(hex(offset)));
        }
    }

    const QByteArray m_d;
    const FalcomJsonLimits m_limits;
    QString m_error;
    qsizetype m_valueCount = 1;
};

QString numberText(double v)
{
    return QString::number(v, 'g', 15);
}

void addRows(const Node &n, const QString &path, std::vector<TreeRow> &rows)
{
    switch (n.type) {
    case Node::Type::String: {
        QString s = n.text;
        rows.push_back({path, "string", s.replace('\n', QStringLiteral(" ↵ ")), {}});
        return;
    }
    case Node::Type::Number:
        rows.push_back({path, "number", numberText(n.number), n.number});
        return;
    case Node::Type::Bool:
        rows.push_back({path, "bool", n.flag ? "true" : "false", {}});
        return;
    case Node::Type::Object:
        rows.push_back({path, "object", QObject::tr("{%1 keys}").arg(n.children.size()), {}});
        break;
    case Node::Type::Array:
        rows.push_back({path, "array", QObject::tr("[%1 items]").arg(n.children.size()), {}});
        break;
    }
    for (size_t i = 0; i < n.children.size(); ++i) {
        const Node &c = n.children[i];
        QString childPath;
        if (n.type == Node::Type::Object && c.hasKey)
            childPath = path.isEmpty() ? c.key : path + '.' + c.key;
        else
            childPath = QStringLiteral("%1[%2]").arg(path).arg(i);
        addRows(c, childPath, rows);
    }
}

void writeString(const QString &s, QByteArray &out)
{
    QString escaped;
    escaped.reserve(s.size() + 2);
    escaped += u'"';
    for (const QChar ch : s) {
        switch (ch.unicode()) {
        case u'"':  escaped += QLatin1String("\\\""); break;
        case u'\\': escaped += QLatin1String("\\\\"); break;
        case u'\n': escaped += QLatin1String("\\n"); break;
        case u'\r': escaped += QLatin1String("\\r"); break;
        case u'\t': escaped += QLatin1String("\\t"); break;
        case u'\b': escaped += QLatin1String("\\b"); break;
        case u'\f': escaped += QLatin1String("\\f"); break;
        default:
            if (ch.unicode() < 0x20)
                escaped += QStringLiteral("\\u%1").arg(uint(ch.unicode()), 4, 16, QLatin1Char('0'));
            else
                escaped += ch;
        }
    }
    escaped += u'"';
    out += escaped.toUtf8();
}

void writeJson(const Node &n, QByteArray &out, int indent)
{
    switch (n.type) {
    case Node::Type::String:
        writeString(n.text, out);
        return;
    case Node::Type::Number:
        if (!std::isfinite(n.number))
            out += "null";
        else if (n.number == std::trunc(n.number) && std::fabs(n.number) < 1e15)
            out += QByteArray::number(qint64(n.number));
        else
            out += QByteArray::number(n.number, 'g', 17);
        return;
    case Node::Type::Bool:
        out += n.flag ? "true" : "false";
        return;
    case Node::Type::Object:
    case Node::Type::Array: {
        const bool isObject = n.type == Node::Type::Object;
        if (n.children.empty()) {
            out += isObject ? "{}" : "[]";
            return;
        }
        out += isObject ? "{\n" : "[\n";
        const QByteArray pad(indent + 2, ' ');
        for (size_t i = 0; i < n.children.size(); ++i) {
            out += pad;
            if (isObject) {
                writeString(n.children[i].key, out);
                out += ": ";
            }
            writeJson(n.children[i], out, indent + 2);
            out += i + 1 < n.children.size() ? ",\n" : "\n";
        }
        out += QByteArray(indent, ' ') + (isObject ? "}" : "]");
        return;
    }
    }
}

}

int FalcomJsonReader::score(const QString &, QByteArrayView head) const
{
    return head.size() >= 16 && head.startsWith("JSON") && head.sliced(4, 4) == QByteArrayView("\0\0\0\0", 4)
             ? 100
             : 0;
}

std::unique_ptr<Document> FalcomJsonReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return fromBytes(*bytes, error);
}

std::unique_ptr<Document> FalcomJsonReader::fromBytes(const QByteArray &data, QString *error,
                                                      FalcomJsonLimits limits)
{
    auto root = std::make_shared<Node>();
    if (!Parser(data, limits).parse(*root, error))
        return nullptr;

    std::vector<TreeRow> rows;
    addRows(*root, {}, rows);

    auto exporter = [root] {
        QByteArray out;
        writeJson(*root, out, 0);
        out += '\n';
        return out;
    };

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_unique<TreeTable>(QStringLiteral("Data"), std::move(rows), exporter));
    return doc;
}
