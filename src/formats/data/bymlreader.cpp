#include "data/bymlreader.h"

#include "core/storedreader.h"
#include "core/treetable.h"
#include "data/bymlformat.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <bit>

namespace {

using namespace byml;

class Parser : public Bytes
{
public:
    Parser(const QByteArray &data, BymlLimits limits) : Bytes(data), m_limits(limits) {}

    QJsonValue parse(std::vector<TreeRow> *rows)
    {
        m_rows = rows;
        const quint32 keys = u32(4);
        const quint32 strings = u32(8);
        quint32 root = u32(12);
        if (version() < 4 && root != 0 && u8(root) == PathTable && size() >= 20)
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
    void add(const QString &path, const QString &type, const QString &value, QVariant sort = {})
    {
        if (++m_count > m_limits.maxValues)
            throw ParseFailure(QObject::tr("The file has more values than this app can show.").toStdString());
        if (m_rows)
            m_rows->push_back({path, type, value, std::move(sort)});
    }

    static QString join(const QString &path, const QString &key) { return path.isEmpty() ? key : path + u'.' + key; }

    QJsonValue container(qint64 at, const QString &path, int depth)
    {
        if (depth > m_limits.maxDepth)
            throw ParseFailure(QObject::tr("The data is nested too deeply.").toStdString());
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
            throw ParseFailure(QObject::tr("Unknown BYML container type 0x%1.").arg(type, 2, 16, QLatin1Char('0')).toStdString());
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
            const QByteArray bytes = data().mid(start, size);
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

    BymlLimits m_limits;
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
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
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
    } catch (const ParseFailure &f) {
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
    } catch (const ParseFailure &f) {
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
