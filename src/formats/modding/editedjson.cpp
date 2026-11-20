#include "modding/editedjson.h"

#include "data/bymlreader.h"
#include "data/bymlwriter.h"
#include "data/msbtwriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

namespace {

std::optional<QByteArray> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

}

bool canApplyEditedJson(const QByteArray &leaf)
{
    return BymlReader::isByml(leaf) || leaf.startsWith("MsgStdBn");
}

std::optional<QByteArray> applyEditedJson(const QByteArray &leaf, const QByteArray &json, QString *error)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError) {
        const QByteArray before = json.left(parse.offset);
        const qsizetype line = before.count('\n') + 1;
        const qsizetype column = parse.offset - (before.lastIndexOf('\n') + 1) + 1;
        return fail(error, QObject::tr("The JSON file has a mistake on line %1, column %2: %3.")
                               .arg(line)
                               .arg(column)
                               .arg(parse.errorString()));
    }
    if (leaf.startsWith("MsgStdBn")) {
        if (!doc.isObject())
            return fail(error, QObject::tr("The JSON file should be an object of messages, like the one Extract → Plain "
                                           "JSON saves."));
        return msbtFromJson(doc.object(), leaf, error);
    }
    if (BymlReader::isByml(leaf)) {
        const QJsonValue value = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
        return bymlFromJson(value, leaf, error);
    }
    return fail(error, QObject::tr("Only BYML and MSBT files can be changed from JSON."));
}
