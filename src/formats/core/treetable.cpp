#include "core/treetable.h"

#include <QObject>

#include <cmath>

TreeTable::TreeTable(QString name, std::vector<TreeRow> rows, std::function<QByteArray()> jsonExporter)
    : m_name(std::move(name)), m_rows(std::move(rows)), m_jsonExporter(std::move(jsonExporter))
{
}

QString TreeTable::columnName(int column) const
{
    static const char *names[] = {"path", "type", "value"};
    return QString::fromLatin1(names[column]);
}

QString TreeTable::text(int row, int column, DisplayMode mode) const
{
    const TreeRow &r = m_rows.at(row);
    switch (column) {
    case 0: return r.path.isEmpty() ? QStringLiteral("(root)") : r.path;
    case 1: return r.type;
    default:
        if (mode == DisplayMode::Hex && r.sortKey.typeId() == QMetaType::Double && r.type != QLatin1String("float")
            && r.type != QLatin1String("double")) {
            const double v = r.sortKey.toDouble();
            if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9.2e18) {
                const qint64 i = qint64(v);
                const quint64 bits = r.type == QLatin1String("int") ? quint64(quint32(qint32(i))) : quint64(i);
                return QStringLiteral("0x") + QString::number(bits, 16).toUpper();
            }
        }
        return r.value;
    }
}

QVariant TreeTable::sortKey(int row, int column) const
{
    if (column == 2 && m_rows.at(row).sortKey.isValid())
        return m_rows.at(row).sortKey;
    return text(row, column, DisplayMode::Auto);
}

QString TreeTable::description() const
{
    return QObject::tr("%1 values").arg(m_rows.size());
}
