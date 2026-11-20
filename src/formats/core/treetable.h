#pragma once

#include "core/format.h"

#include <functional>

// One value in a tree-shaped file (JSON and similar), flattened to a row.
struct TreeRow
{
    QString path;      // e.g. "Layout.items[3].name"; empty for the root
    QString type;      // object, array, string, number, bool, null
    QString value;     // text shown in the table
    QVariant sortKey;  // numbers sort as numbers; invalid = sort by text
};

// Shows tree-shaped data as "path / type / value" rows. Shared by the JSON readers.
class TreeTable : public Table
{
public:
    TreeTable(QString name, std::vector<TreeRow> rows, std::function<QByteArray()> jsonExporter = {});

    QString name() const override { return m_name; }
    int rowCount() const override { return int(m_rows.size()); }
    int columnCount() const override { return 3; }
    QString columnName(int column) const override;
    QString text(int row, int column, DisplayMode mode) const override;
    QVariant sortKey(int row, int column) const override;
    QString description() const override;
    bool canExportJson() const override { return bool(m_jsonExporter); }
    QByteArray exportJson() const override { return m_jsonExporter ? m_jsonExporter() : QByteArray(); }

private:
    QString m_name;
    std::vector<TreeRow> m_rows;
    std::function<QByteArray()> m_jsonExporter;
};
