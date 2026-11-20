#include "core/simpletable.h"

#include <new>

namespace {

bool isInteger(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

}

SimpleTable::SimpleTable(QString name, std::vector<SimpleColumn> columns, std::vector<Row> rows)
    : m_name(std::move(name)), m_columns(std::move(columns)), m_rows(std::move(rows))
{
    for (Row &row : m_rows)
        row.resize(m_columns.size());
}

QString SimpleTable::text(int row, int column, DisplayMode mode) const
{
    const QVariant &v = m_rows.at(size_t(row)).at(size_t(column));
    if (isInteger(v) && (mode == DisplayMode::Hex || m_columns.at(size_t(column)).alwaysHex)) {
        const bool isSigned = v.typeId() == QMetaType::Int || v.typeId() == QMetaType::LongLong;
        if (isSigned && v.toLongLong() < 0)
            return QStringLiteral("-0x") + QString::number(0 - v.toULongLong(), 16).toUpper();
        return QStringLiteral("0x") + QString::number(v.toULongLong(), 16).toUpper();
    }
    return v.toString();
}

QVariant SimpleTable::sortKey(int row, int column) const
{
    return m_rows.at(size_t(row)).at(size_t(column));
}

bool SimpleTable::isNumeric(int column) const
{
    for (const Row &row : m_rows) {
        if (!row.at(size_t(column)).isNull())
            return isInteger(row.at(size_t(column)));
    }
    return false;
}

std::shared_ptr<const Image> SimpleTable::image() const
{
    if (m_imageMaker) {
        std::call_once(m_imageMade, [this] {
            try {
                Image made = m_imageMaker();
                if (made.width > 0 && made.height > 0)
                    m_image = std::make_shared<const Image>(std::move(made));
            } catch (const std::bad_alloc &) {
                m_image.reset();
            }
        });
    }
    return m_image;
}
