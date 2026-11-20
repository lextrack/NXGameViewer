#pragma once

#include "core/records.h"

#include <QStringList>

class SchemaStore
{
public:
    bool load(const QString &path, QString *error, QStringList *warnings = nullptr);
    bool save(QString *error) const;

    const QString &path() const { return m_path; }
    void setPath(const QString &path) { m_path = path; }
    const QString &gameName() const { return m_gameName; }
    void setGameName(const QString &name) { m_gameName = name; }

    std::vector<FieldDef> fields(const QString &format, const QString &table, int recordSize) const;

    bool setField(const QString &format, const QString &table, int recordSize, FieldDef field);
    void removeField(const QString &format, const QString &table, int recordSize, int offset);
    void clearTable(const QString &format, const QString &table, int recordSize);

    int tableCount() const { return int(m_tables.size()); }

private:
    struct TableSchema
    {
        QString format;
        QString table;
        int recordSize = 0;
        std::vector<FieldDef> fields;
    };

    std::vector<TableSchema>::iterator find(const QString &format, const QString &table, int recordSize);
    std::vector<TableSchema>::const_iterator find(const QString &format, const QString &table, int recordSize) const;

    QString m_path;
    QString m_gameName;
    std::vector<TableSchema> m_tables;
};
