#pragma once

#include "core/format.h"

#include <QByteArrayView>

#include <algorithm>
#include <optional>
#include <vector>

enum class FieldType { Auto32, U8, I8, U16, I16, U32, I32, F32, U64, Text, Chars };

int fieldSize(FieldType type);
QString fieldTypeName(FieldType type);
std::optional<FieldType> fieldTypeFromName(const QString &name);
const std::vector<FieldType> &allFieldTypes();

struct FieldDef
{
    int offset = 0;
    FieldType type = FieldType::U8;
    QString name;
    bool userDefined = false;
    int size = 0;

    int length() const { return type == FieldType::Chars ? std::max(size, 1) : fieldSize(type); }
    int end() const { return offset + length(); }
    friend bool operator==(const FieldDef &, const FieldDef &) = default;
};

class RecordSource
{
public:
    RecordSource() = default;
    virtual ~RecordSource() = default;
    RecordSource(const RecordSource &) = delete;
    RecordSource &operator=(const RecordSource &) = delete;
    RecordSource(RecordSource &&) = delete;
    RecordSource &operator=(RecordSource &&) = delete;

    virtual QString recordTypeName() const = 0;
    virtual int recordSize() const = 0;
    virtual int recordCount() const = 0;
    virtual QByteArrayView record(int row) const = 0;
    virtual std::vector<FieldDef> guessFields() const { return {}; }
    virtual bool hasTextPointers() const { return false; }
    virtual std::optional<QString> textAt(quint64 /*pointer*/) const { return std::nullopt; }

    virtual QString description() const { return {}; }
};

std::vector<FieldDef> buildLayout(int recordSize, const std::vector<FieldDef> &userFields,
                                  const std::vector<FieldDef> &guessed);

class RecordTable : public Table
{
public:
    explicit RecordTable(std::shared_ptr<const RecordSource> source,
                         const std::vector<FieldDef> &userFields = {});

    QString name() const override { return m_source->recordTypeName(); }
    int rowCount() const override { return m_source->recordCount(); }
    int columnCount() const override { return int(m_layout.size()); }
    QString columnName(int column) const override;
    QString columnToolTip(int column) const override;
    QString text(int row, int column, DisplayMode mode) const override;
    QVariant sortKey(int row, int column) const override;
    QString toolTip(int row, int column) const override;
    bool isNumeric(int column) const override;
    QString description() const override;
    std::shared_ptr<const RecordSource> recordSource() const override { return m_source; }

    const std::vector<FieldDef> &layout() const { return m_layout; }
    static QString defaultLabel(const FieldDef &field);

private:
    const char *fieldData(int row, int column) const;

    std::shared_ptr<const RecordSource> m_source;
    std::vector<FieldDef> m_layout;
};
