#include "cri/criutf.h"

#include <QObject>
#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace {

enum Type : quint8 { U8, S8, U16, S16, U32, S32, U64, S64, Float, Double, String, Data, U128 };

int typeSize(quint8 type)
{
    switch (type) {
    case U8: case S8: return 1;
    case U16: case S16: return 2;
    case U32: case S32: case Float: case String: return 4;
    case U64: case S64: case Double: case Data: return 8;
    case U128: return 16;
    default: return -1;
    }
}

class Reader
{
public:
    explicit Reader(QByteArrayView data) : m_data(data) {}
    QByteArrayView at(qsizetype pos, qsizetype n) const
    {
        if (pos < 0 || n < 0 || n > m_data.size() - pos)
            throw std::runtime_error("short");
        return m_data.sliced(pos, n);
    }
    template <typename T> T be(qsizetype pos) const { return qFromBigEndian<T>(at(pos, sizeof(T)).data()); }
    qsizetype size() const { return m_data.size(); }

private:
    QByteArrayView m_data;
};

}

int CriUtfTable::column(const QString &columnName) const
{
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == columnName)
            return int(i);
    return -1;
}

QVariant CriUtfTable::value(int row, const QString &columnName) const
{
    const int c = column(columnName);
    if (c < 0 || row < 0 || size_t(row) >= rows.size())
        return {};
    return rows[size_t(row)][size_t(c)];
}

QString criUtfTypeName(quint8 type)
{
    static const char *names[] = {"u8", "s8", "u16", "s16", "u32", "s32", "u64", "s64", "float", "double", "string", "data", "u128"};
    return type <= U128 ? QString::fromLatin1(names[type]) : QStringLiteral("?");
}

std::optional<CriUtfTable> readCriUtf(QByteArrayView data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<CriUtfTable> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!data.startsWith("@UTF"))
        return fail(QObject::tr("Not a CRI @UTF table."));
    try {
        const Reader in(data);
        const qsizetype base = 8;
        const qsizetype end = base + in.be<quint32>(4);
        if (end > data.size())
            return fail(QObject::tr("The @UTF table is cut short."));
        const qsizetype rowsAt = base + in.be<quint16>(0x0A);
        const qsizetype stringsAt = base + in.be<quint32>(0x0C);
        const qsizetype dataAt = base + in.be<quint32>(0x10);
        const quint32 nameOffset = in.be<quint32>(0x14);
        const int columnCount = in.be<quint16>(0x18);
        const int rowWidth = in.be<quint16>(0x1A);
        const quint32 rowCount = in.be<quint32>(0x1C);
        if (rowCount > 10'000'000 || stringsAt > end || dataAt > end)
            return fail(QObject::tr("The @UTF table header is damaged."));

        auto string = [&](quint32 offset) {
            const qsizetype at = stringsAt + offset;
            if (at >= dataAt)
                return QString();
            const qsizetype stop = data.indexOf('\0', at);
            return QString::fromUtf8(data.sliced(at, (stop < 0 || stop > dataAt ? dataAt : stop) - at));
        };
        auto read = [&](quint8 type, qsizetype pos) -> QVariant {
            switch (type) {
            case U8: return int(in.be<quint8>(pos));
            case S8: return int(in.be<qint8>(pos));
            case U16: return int(in.be<quint16>(pos));
            case S16: return int(in.be<qint16>(pos));
            case U32: return qint64(in.be<quint32>(pos));
            case S32: return in.be<qint32>(pos);
            case U64: return in.be<quint64>(pos);
            case S64: return in.be<qint64>(pos);
            case Float: {
                const quint32 bits = in.be<quint32>(pos);
                float f;
                std::memcpy(&f, &bits, 4);
                return double(f);
            }
            case Double: {
                const quint64 bits = in.be<quint64>(pos);
                double d;
                std::memcpy(&d, &bits, 8);
                return d;
            }
            case String: return string(in.be<quint32>(pos));
            case Data: {
                const qsizetype offset = in.be<quint32>(pos), size = in.be<quint32>(pos + 4);
                if (size == 0)
                    return QByteArray();
                return in.at(dataAt + offset, size).toByteArray();
            }
            case U128: return in.at(pos, 16).toByteArray().toHex();
            default: throw std::runtime_error("type");
            }
        };

        CriUtfTable table;
        table.name = string(nameOffset);
        std::vector<QVariant> constants;
        qsizetype pos = 0x20;
        for (int i = 0; i < columnCount; ++i) {
            const quint8 flags = in.be<quint8>(pos++);
            CriUtfColumn column;
            column.type = flags & 0x0F;
            if (typeSize(column.type) < 0)
                return fail(QObject::tr("Column %1 has an unknown type.").arg(i));
            if (flags & 0x10) {
                column.name = string(in.be<quint32>(pos));
                pos += 4;
            }
            QVariant constant;
            if (flags & 0x20) {
                constant = read(column.type, pos);
                pos += typeSize(column.type);
            }
            column.perRow = flags & 0x40;
            table.columns.push_back(column);
            constants.push_back(constant);
        }
        for (quint32 r = 0; r < rowCount; ++r) {
            qsizetype at = rowsAt + qsizetype(r) * rowWidth;
            std::vector<QVariant> row;
            row.reserve(table.columns.size());
            for (size_t c = 0; c < table.columns.size(); ++c) {
                if (table.columns[c].perRow) {
                    row.push_back(read(table.columns[c].type, at));
                    at += typeSize(table.columns[c].type);
                } else {
                    row.push_back(constants[c]);
                }
            }
            table.rows.push_back(std::move(row));
        }
        return table;
    } catch (const std::exception &) {
        return fail(QObject::tr("The @UTF table is damaged."));
    }
}
