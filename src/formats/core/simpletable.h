#pragma once

#include "core/format.h"

#include <functional>
#include <mutex>

// A ready-made Table for readers that just have rows of values: give it the columns and
// the cells, and optionally a picture, a status-bar description and a JSON export.
// Integer cells sort as numbers and follow the "Hex numbers" switch.

struct SimpleColumn
{
    QString name;
    bool alwaysHex = false;  // integers in this column are always shown as 0x…
    QString toolTip = {};
};

class SimpleTable : public Table
{
public:
    using Row = std::vector<QVariant>;

    SimpleTable(QString name, std::vector<SimpleColumn> columns, std::vector<Row> rows);

    void setDescription(QString description) { m_description = std::move(description); }
    void setImage(std::shared_ptr<const Image> image) { m_image = std::move(image); }
    // For pictures that take a while to make: made the first time they are asked for.
    void setImageMaker(std::function<Image()> maker) { m_imageMaker = std::move(maker); }
    void setJsonExporter(std::function<QByteArray()> exporter) { m_jsonExporter = std::move(exporter); }
    void setTextureSlot(TextureSlot slot) { m_textureSlot = std::move(slot); }

    QString name() const override { return m_name; }
    int rowCount() const override { return int(m_rows.size()); }
    int columnCount() const override { return int(m_columns.size()); }
    QString columnName(int column) const override { return m_columns.at(size_t(column)).name; }
    QString columnToolTip(int column) const override { return m_columns.at(size_t(column)).toolTip; }
    QString text(int row, int column, DisplayMode mode) const override;
    QVariant sortKey(int row, int column) const override;
    bool isNumeric(int column) const override;
    QString description() const override { return m_description; }
    bool canExportJson() const override { return bool(m_jsonExporter); }
    QByteArray exportJson() const override { return m_jsonExporter ? m_jsonExporter() : QByteArray(); }
    // With a maker, m_image is written by image() (maybe on another thread): don't read it.
    bool hasImage() const override { return m_imageMaker ? true : bool(m_image); }
    std::shared_ptr<const Image> image() const override;
    std::optional<TextureSlot> textureSlot() const override { return m_textureSlot; }

private:
    QString m_name;
    std::vector<SimpleColumn> m_columns;
    std::vector<Row> m_rows;
    QString m_description;
    mutable std::shared_ptr<const Image> m_image;
    std::function<Image()> m_imageMaker;
    mutable std::once_flag m_imageMade;
    std::function<QByteArray()> m_jsonExporter;
    std::optional<TextureSlot> m_textureSlot;
};
