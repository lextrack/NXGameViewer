#include "tablemodel.h"

#include "contenticons.h"

#include <QFontDatabase>

TableModel::TableModel(QObject *parent) : QAbstractTableModel(parent) {}

void TableModel::setTable(std::shared_ptr<const Table> table)
{
    beginResetModel();
    m_table = std::move(table);
    endResetModel();
}

void TableModel::setDisplayMode(DisplayMode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    if (rowCount() > 0 && columnCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole});
}

int TableModel::rowCount(const QModelIndex &parent) const
{
    return (m_table && !parent.isValid()) ? m_table->rowCount() : 0;
}

int TableModel::columnCount(const QModelIndex &parent) const
{
    return (m_table && !parent.isValid()) ? m_table->columnCount() : 0;
}

QVariant TableModel::data(const QModelIndex &index, int role) const
{
    if (!m_table || !index.isValid() || index.row() >= m_table->rowCount() || index.column() >= m_table->columnCount())
        return {};

    switch (role) {
    case Qt::DisplayRole:
        return m_table->text(index.row(), index.column(), m_mode);
    case Qt::ToolTipRole: {
        const QString tip = m_table->toolTip(index.row(), index.column());
        return tip.isEmpty() ? QVariant() : QVariant(tip);
    }
    case SortRole:
        return m_table->sortKey(index.row(), index.column());
    case Qt::TextAlignmentRole:
        if (m_table->isNumeric(index.column()))
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case Qt::DecorationRole:
        if (index.column() == 0)
            if (const auto archive = m_table->archive(); archive && size_t(index.row()) < archive->entries.size()) {
                return contentIcon(contentCategory(archive->entryHead(size_t(index.row()), 16)));
            }
        return {};
    case Qt::FontRole:
        if (m_table->monospace())
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        return {};
    default:
        return {};
    }
}

QVariant TableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!m_table || section < 0
        || section >= (orientation == Qt::Horizontal ? m_table->columnCount() : m_table->rowCount()))
        return {};
    if (role == Qt::FontRole && m_table->monospace())
        return QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (role == Qt::ToolTipRole && orientation == Qt::Horizontal) {
        const QString tip = m_table->columnToolTip(section);
        return (tip.isEmpty() ? m_table->columnName(section) : tip) + tr("\nClick to sort by this column");
    }
    if (role != Qt::DisplayRole)
        return {};
    return orientation == Qt::Horizontal ? m_table->columnName(section) : m_table->rowLabel(section);
}
