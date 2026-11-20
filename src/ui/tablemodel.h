#pragma once

#include "formats/core/format.h"

#include <QAbstractTableModel>

class TableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    static constexpr int SortRole = Qt::UserRole;

    explicit TableModel(QObject *parent = nullptr);

    void setTable(std::shared_ptr<const Table> table);
    void clear() { setTable(nullptr); }
    void setDisplayMode(DisplayMode mode);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::shared_ptr<const Table> m_table;
    DisplayMode m_mode = DisplayMode::Auto;
};
