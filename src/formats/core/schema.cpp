#include "core/schema.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>

std::vector<SchemaStore::TableSchema>::iterator SchemaStore::find(const QString &format, const QString &table,
                                                                  int recordSize)
{
    return std::find_if(m_tables.begin(), m_tables.end(), [&](const TableSchema &t) {
        return t.format == format && t.table == table && t.recordSize == recordSize;
    });
}

std::vector<SchemaStore::TableSchema>::const_iterator SchemaStore::find(const QString &format, const QString &table,
                                                                        int recordSize) const
{
    return std::find_if(m_tables.begin(), m_tables.end(), [&](const TableSchema &t) {
        return t.format == format && t.table == table && t.recordSize == recordSize;
    });
}

std::vector<FieldDef> SchemaStore::fields(const QString &format, const QString &table, int recordSize) const
{
    const auto it = find(format, table, recordSize);
    return it == m_tables.end() ? std::vector<FieldDef>{} : it->fields;
}

bool SchemaStore::setField(const QString &format, const QString &table, int recordSize, FieldDef field)
{
    if (field.offset < 0 || field.end() > recordSize)
        return false;
    field.userDefined = true;

    auto it = find(format, table, recordSize);
    if (it == m_tables.end()) {
        m_tables.push_back({format, table, recordSize, {}});
        it = std::prev(m_tables.end());
    }
    auto &fields = it->fields;
    std::erase_if(fields, [&](const FieldDef &f) { return f.offset < field.end() && field.offset < f.end(); });
    fields.insert(std::upper_bound(fields.begin(), fields.end(), field,
                                   [](const FieldDef &a, const FieldDef &b) { return a.offset < b.offset; }),
                  field);
    return true;
}

void SchemaStore::removeField(const QString &format, const QString &table, int recordSize, int offset)
{
    const auto it = find(format, table, recordSize);
    if (it == m_tables.end())
        return;
    std::erase_if(it->fields, [&](const FieldDef &f) { return f.offset == offset; });
    if (it->fields.empty())
        m_tables.erase(it);
}

void SchemaStore::clearTable(const QString &format, const QString &table, int recordSize)
{
    if (const auto it = find(format, table, recordSize); it != m_tables.end())
        m_tables.erase(it);
}

bool SchemaStore::load(const QString &path, QString *error, QStringList *warnings)
{
    m_path = path;
    m_gameName = QFileInfo(path).completeBaseName();
    m_tables.clear();

    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!json.isObject()) {
        if (error)
            *error = QObject::tr("%1 is not a valid column file: %2").arg(path, parseError.errorString());
        return false;
    }

    const QJsonObject root = json.object();
    if (const QString game = root["game"].toString(); !game.isEmpty())
        m_gameName = game;

    auto warn = [&](const QString &w) {
        if (warnings)
            *warnings << w;
    };

    for (const QJsonValue &tv : root["tables"].toArray()) {
        const QJsonObject t = tv.toObject();
        const QString format = t["format"].toString();
        const QString table = t["table"].toString();
        const int recordSize = t["recordSize"].toInt(-1);
        if (format.isEmpty() || table.isEmpty() || recordSize <= 0) {
            warn(QObject::tr("Skipped a table entry without format, table name or record size."));
            continue;
        }
        for (const QJsonValue &fv : t["fields"].toArray()) {
            const QJsonObject f = fv.toObject();
            const std::optional<FieldType> type = fieldTypeFromName(f["type"].toString());
            const int offset = f["offset"].toInt(-1);
            if (!type || !setField(format, table, recordSize, {offset, *type, f["name"].toString(), true, f["size"].toInt()}))
                warn(QObject::tr("Skipped a field at offset %1 in %2.").arg(offset).arg(table));
        }
    }
    return true;
}

bool SchemaStore::save(QString *error) const
{
    QJsonArray tables;
    for (const TableSchema &t : m_tables) {
        QJsonArray fields;
        for (const FieldDef &f : t.fields) {
            QJsonObject field{{"offset", f.offset}, {"type", fieldTypeName(f.type)}, {"name", f.name}};
            if (f.type == FieldType::Chars)
                field["size"] = f.length();
            fields.append(field);
        }
        tables.append(QJsonObject{{"format", t.format},
                                  {"table", t.table},
                                  {"recordSize", t.recordSize},
                                  {"fields", fields}});
    }
    const QJsonObject root{{"game", m_gameName}, {"tables", tables}};

    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(root).toJson()) < 0 || !out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }
    return true;
}
