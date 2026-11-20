#include "data/jsonreader.h"
#include "core/storedreader.h"
#include "core/treetable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

namespace {

constexpr qint64 kMaxSize = qint64{64} * 1024 * 1024;

void flatten(const QJsonValue &v, const QString &path, std::vector<TreeRow> &rows)
{
    switch (v.type()) {
    case QJsonValue::Object: {
        const QJsonObject o = v.toObject();
        rows.push_back({path, "object", QObject::tr("{%1 keys}").arg(o.size()), {}});
        for (auto it = o.begin(); it != o.end(); ++it)
            flatten(it.value(), path.isEmpty() ? it.key() : path + '.' + it.key(), rows);
        break;
    }
    case QJsonValue::Array: {
        const QJsonArray a = v.toArray();
        rows.push_back({path, "array", QObject::tr("[%1 items]").arg(a.size()), {}});
        for (qsizetype i = 0; i < a.size(); ++i)
            flatten(a.at(i), QStringLiteral("%1[%2]").arg(path).arg(i), rows);
        break;
    }
    case QJsonValue::String: {
        QString s = v.toString();
        rows.push_back({path, "string", s.replace('\n', QStringLiteral(" ↵ ")), {}});
        break;
    }
    case QJsonValue::Double:
        rows.push_back({path, "number", QString::number(v.toDouble(), 'g', 15), v.toDouble()});
        break;
    case QJsonValue::Bool:
        rows.push_back({path, "bool", v.toBool() ? "true" : "false", {}});
        break;
    default:
        rows.push_back({path, "null", "null", {}});
        break;
    }
}

}

int JsonReader::score(const QString &fileName, QByteArrayView head) const
{
    if (fileName.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        return 60;
    const QByteArray trimmed = head.toByteArray().trimmed();
    return (trimmed.startsWith('{') || trimmed.startsWith('[')) ? 20 : 0;
}

std::unique_ptr<Document> JsonReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxSize, error);
    return bytes ? readBytes(*bytes, path, error) : nullptr;
}

std::unique_ptr<Document> JsonReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    if (data.size() > kMaxSize) {
        if (error)
            *error = fileTooLargeMessage(kMaxSize);
        return nullptr;
    }
    QJsonParseError parseError;
    QByteArray bytes = data;
    if (const DetectedEncoding detected = detectTextEncoding(bytes);
        detected.encoding != TextEncoding::Utf8 || detected.bomSize > 0)
        bytes = decodeText(QByteArrayView(bytes).sliced(detected.bomSize), detected.encoding).toUtf8();
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (json.isNull()) {
        if (error)
            *error = QObject::tr("Not valid JSON: %1 (at byte %2)")
                         .arg(parseError.errorString())
                         .arg(parseError.offset);
        return nullptr;
    }

    std::vector<TreeRow> rows;
    flatten(json.isObject() ? QJsonValue(json.object()) : QJsonValue(json.array()), {}, rows);

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_unique<TreeTable>(QStringLiteral("JSON"), std::move(rows)));
    return doc;
}
