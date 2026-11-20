#pragma once

#include <QByteArray>
#include <QString>
#include <QVariant>

#include <optional>
#include <vector>

struct CriUtfColumn
{
    QString name;
    quint8 type = 0;
    bool perRow = false;
};

struct CriUtfTable
{
    QString name;
    std::vector<CriUtfColumn> columns;
    std::vector<std::vector<QVariant>> rows;

    int column(const QString &name) const;
    QVariant value(int row, const QString &column) const;
};

std::optional<CriUtfTable> readCriUtf(QByteArrayView data, QString *error);
QString criUtfTypeName(quint8 type);
