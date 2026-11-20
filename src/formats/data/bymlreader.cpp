#include "data/bymlreader.h"

#include "core/treetable.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <bit>
#include <stdexcept>

namespace {

enum Type : quint8 {
    HashDict = 0x20,
    HashDictRemap = 0x21,
    String = 0xA0,
    Binary = 0xA1,
    FileData = 0xA2,
    Array = 0xC0,
    Dict = 0xC1,
    StringTable = 0xC2,
    PathTable = 0xC3,
    Bool = 0xD0,
    Int = 0xD1,
    Float = 0xD2,
    UInt = 0xD3,
    Int64 = 0xD4,
    UInt64 = 0xD5,
    Double = 0xD6,
    Null = 0xFF,
};

struct Failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Parser
{
public:
    Parser(const QByteArray &data, BymlLimits limits) : m_data(data), m_limits(limits)
    {
        m_little = data.startsWith("YB");
        m_version = u16(2);
    }

    QJsonValue parse(std::vector<TreeRow> *rows)
    {
        m_rows = rows;
        const quint32 keys = u32(4);
        const quint32 strings = u32(8);
        quint32 root = u32(12);
        if (m_version < 4 && root != 0 && u8(root) == PathTable && m_data.size() >= 20)
            root = u32(16);
        if (keys)
            m_keys = stringTable(keys);
        if (strings)
            m_strings = stringTable(strings);
        if (root == 0) {
            add({}, QStringLiteral("null"), QStringLiteral("null"));
            return QJsonValue();
        }
        return container(root, {}, 0);
    }

private:
    void need(qint64 at, qint64 size) const
    {
        if (at < 0 || size < 0 || at > m_data.size() - size)
            throw Failure(QObject::tr("An offset points outside the file.").toStdString());
    }
    quint8 u8(qint64 at) const
    {
        need(at, 1);
        return quint8(m_data[at]);
    }
    quint16 u16(qint64 at) const
    {
        need(at, 2);
        return m_little ? qFromLittleEndian<quint16>(m_data.constData() + at) : qFromBigEndian<quint16>(m_data.constData() + at);
    }
    quint32 u24(qint64 at) const
    {
        need(at, 3);
        const auto *p = reinterpret_cast<const uchar *>(m_data.constData() + at);
        return m_little ? quint32(p[0] | p[1] << 8 | p[2] << 16) : quint32(p[2] | p[1] << 8 | p[0] << 16);
    }
    quint32 u32(qint64 at) const
    {
        need(at, 4);
        return m_little ? qFromLittleEndian<quint32>(m_data.constData() + at) : qFromBigEndian<quint32>(m_data.constData() + at);
    }
    quint64 u64(qint64 at) const
    {
        need(at, 8);
        return m_little ? qFromLittleEndian<quint64>(m_data.constData() + at) : qFromBigEndian<quint64>(m_data.constData() + at);
    }

    QStringList stringTable(qint64 at) const
    {
        if (u8(at) != StringTable)
            throw Failure(QObject::tr("A string table has the wrong type.").toStdString());
        const quint32 count = u24(at + 1);
        need(at + 4, qint64(count) * 4);
        QStringList out;
        out.reserve(count);
        for (quint32 i = 0; i < count; ++i) {
            const qint64 start = at + u32(at + 4 + qint64(i) * 4);
            need(start, 1);
            const char *p = m_data.constData() + start;
            out << QString::fromUtf8(p, qsizetype(qstrnlen(p, uint(m_data.size() - start))));
        }
        return out;
    }

    void add(const QString &path, const QString &type, const QString &value, QVariant sort = {})
    {
        if (++m_count > m_limits.maxValues)
            throw Failure(QObject::tr("The file has more values than this app can show.").toStdString());
        if (m_rows)
            m_rows->push_back({path, type, value, std::move(sort)});
    }

    static QString join(const QString &path, const QString &key) { return path.isEmpty() ? key : path + u'.' + key; }

    QJsonValue container(qint64 at, const QString &path, int depth)
    {
        if (depth > m_limits.maxDepth)
            throw Failure(QObject::tr("The data is nested too deeply.").toStdString());
        const quint8 type = u8(at);
        const quint32 count = u24(at + 1);
        switch (type) {
        case Array: {
            need(at + 4, count);
            const qint64 values = at + 4 + ((qint64(count) + 3) & ~qint64(3));
            need(values, qint64(count) * 4);
            add(path, QStringLiteral("array"), QObject::tr("[%1 items]").arg(count));
            QJsonArray out;
            for (quint32 i = 0; i < count; ++i)
                out.append(value(u8(at + 4 + i), values + qint64(i) * 4, QStringLiteral("%1[%2]").arg(path).arg(i), depth));
            return out;
        }
        case Dict: {
            need(at + 4, qint64(count) * 8);
            add(path, QStringLiteral("dictionary"), QObject::tr("{%1 keys}").arg(count));
            QJsonObject out;
            for (quint32 i = 0; i < count; ++i) {
                const qint64 e = at + 4 + qint64(i) * 8;
                const quint32 key = u24(e);
                const QString name = key < quint32(m_keys.size()) ? m_keys[key] : QStringLiteral("#%1").arg(key);
                out.insert(name, value(u8(e + 3), e + 4, join(path, name), depth));
            }
            return out;
        }
        case HashDict:
        case HashDictRemap: {
            const qint64 stride = type == HashDict ? 8 : 12;
            need(at + 4, qint64(count) * stride + count);
            const qint64 types = at + 4 + qint64(count) * stride;
            add(path, QStringLiteral("hash dictionary"), QObject::tr("{%1 keys}").arg(count));
            QJsonObject out;
            for (quint32 i = 0; i < count; ++i) {
                const qint64 e = at + 4 + qint64(i) * stride;
                const QString name = QStringLiteral("0x%1").arg(QString::number(u32(e), 16).rightJustified(8, u'0'));
                out.insert(name, value(u8(types + i), e + 4, join(path, name), depth));
            }
            return out;
        }
        default:
            throw Failure(QObject::tr("Unknown BYML container type 0x%1.").arg(type, 2, 16, QLatin1Char('0')).toStdString());
        }
    }

    QJsonValue value(quint8 type, qint64 at, const QString &path, int depth)
    {
        const quint32 raw = u32(at);
        switch (type) {
        case Array:
        case Dict:
        case HashDict:
        case HashDictRemap:
            return container(raw, path, depth + 1);
        case String: {
            const QString s = raw < quint32(m_strings.size()) ? m_strings[raw] : QString();
            add(path, QStringLiteral("string"), QString(s).replace(u'\n', QStringLiteral(" ↵ ")));
            return s;
        }
        case Binary:
        case FileData: {
            const quint32 size = u32(raw);
            const qint64 start = raw + (type == FileData ? 8 : 4);
            need(start, size);
            const QByteArray bytes = m_data.mid(start, size);
            add(path, QStringLiteral("binary"),
                QObject::tr("%n byte(s): %1", nullptr, int(size)).arg(QString::fromLatin1(bytes.left(32).toHex(' '))));
            return QString::fromLatin1(bytes.toBase64());
        }
        case Bool:
            add(path, QStringLiteral("bool"), raw ? QStringLiteral("true") : QStringLiteral("false"));
            return raw != 0;
        case Int:
            add(path, QStringLiteral("int"), QString::number(qint32(raw)), double(qint32(raw)));
            return qint32(raw);
        case UInt:
            add(path, QStringLiteral("uint"), QString::number(raw), double(raw));
            return qint64(raw);
        case Float: {
            const float f = std::bit_cast<float>(raw);
            add(path, QStringLiteral("float"), QString::number(f, 'g', 9), double(f));
            return double(f);
        }
        case Int64: {
            const qint64 v = qint64(u64(raw));
            add(path, QStringLiteral("int64"), QString::number(v), double(v));
            return qAbs(v) < (qint64(1) << 53) ? QJsonValue(v) : QJsonValue(QString::number(v));
        }
        case UInt64: {
            const quint64 v = u64(raw);
            add(path, QStringLiteral("uint64"), QString::number(v), double(v));
            return v < (quint64(1) << 53) ? QJsonValue(qint64(v)) : QJsonValue(QString::number(v));
        }
        case Double: {
            const double d = std::bit_cast<double>(u64(raw));
            add(path, QStringLiteral("double"), QString::number(d, 'g', 17), d);
            return d;
        }
        case Null:
            add(path, QStringLiteral("null"), QStringLiteral("null"));
            return QJsonValue();
        default:
            add(path, QStringLiteral("type 0x%1").arg(type, 2, 16, QLatin1Char('0')),
                QStringLiteral("0x%1").arg(raw, 8, 16, QLatin1Char('0')), double(raw));
            return QJsonValue();
        }
    }

    const QByteArray &m_data;
    BymlLimits m_limits;
    bool m_little = true;
    int m_version = 0;
    QStringList m_keys;
    QStringList m_strings;
    std::vector<TreeRow> *m_rows = nullptr;
    qsizetype m_count = 0;
};

}

bool BymlReader::isByml(QByteArrayView head)
{
    if (head.size() < 16 || !(head.startsWith("YB") || head.startsWith("BY")))
        return false;
    const bool little = head.startsWith("YB");
    const quint16 version = little ? qFromLittleEndian<quint16>(head.data() + 2) : qFromBigEndian<quint16>(head.data() + 2);
    return version >= 1 && version <= 10;
}

int BymlReader::score(const QString &fileName, QByteArrayView head) const
{
    if (!isByml(head))
        return 0;
    const QString lower = fileName.toLower();
    return lower.endsWith(".byml") || lower.endsWith(".byaml") || lower.endsWith(".bgyml") ? 95 : 85;
}

std::unique_ptr<Document> BymlReader::read(const QString &path, QString *error) const
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

std::optional<QJsonValue> BymlReader::toJson(const QByteArray &data, QString *error, BymlLimits limits)
{
    if (!isByml(data)) {
        if (error)
            *error = QObject::tr("Not a BYML file.");
        return std::nullopt;
    }
    try {
        return Parser(data, limits).parse(nullptr);
    } catch (const Failure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
    }
    return std::nullopt;
}

std::unique_ptr<Document> BymlReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    if (!isByml(data)) {
        if (error)
            *error = QObject::tr("Not a BYML file.");
        return nullptr;
    }
    std::vector<TreeRow> rows;
    try {
        Parser(data, {}).parse(&rows);
    } catch (const Failure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
        return nullptr;
    }
    QString name = QFileInfo(fileName).completeBaseName();
    auto table = std::make_shared<TreeTable>(name.isEmpty() ? QStringLiteral("BYML") : name, std::move(rows), [data] {
        const std::optional<QJsonValue> json = toJson(data);
        if (!json)
            return QByteArray();
        if (json->isObject())
            return QJsonDocument(json->toObject()).toJson();
        if (json->isArray())
            return QJsonDocument(json->toArray()).toJson();
        return QJsonDocument(QJsonArray{*json}).toJson();
    });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
