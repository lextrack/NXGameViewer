#include "core/records.h"

#include <QObject>
#include <QtEndian>

#include <array>
#include <bit>
#include <cmath>

namespace {

struct TypeInfo
{
    FieldType type;
    const char *name;
    int size;
};

constexpr std::array kTypes{
    TypeInfo{FieldType::Auto32, "auto", 4}, TypeInfo{FieldType::U8, "u8", 1},
    TypeInfo{FieldType::I8, "i8", 1},       TypeInfo{FieldType::U16, "u16", 2},
    TypeInfo{FieldType::I16, "i16", 2},     TypeInfo{FieldType::U32, "u32", 4},
    TypeInfo{FieldType::I32, "i32", 4},     TypeInfo{FieldType::F32, "f32", 4},
    TypeInfo{FieldType::U64, "u64", 8},     TypeInfo{FieldType::Text, "text", 8},
    TypeInfo{FieldType::Chars, "chars", 1},
};

const TypeInfo &info(FieldType type)
{
    for (const TypeInfo &t : kTypes) {
        if (t.type == type)
            return t;
    }
    Q_UNREACHABLE();
}

QString hex(quint64 value, int digits)
{
    return QStringLiteral("0x") + QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}

template <typename T>
T load(const char *p)
{
    return qFromLittleEndian<T>(p);
}

enum class NumberKind { Int, Float, Hex };

NumberKind guessKind(quint32 raw)
{
    const auto asInt = static_cast<qint32>(raw);
    if (asInt > -1'000'000 && asInt < 1'000'000)
        return NumberKind::Int;
    const float f = std::bit_cast<float>(raw);
    const float a = std::fabs(f);
    if (std::isfinite(f) && a > 1e-4f && a < 1e7f)
        return NumberKind::Float;
    return NumberKind::Hex;
}

}

int fieldSize(FieldType type) { return info(type).size; }

QString fieldTypeName(FieldType type) { return QString::fromLatin1(info(type).name); }

std::optional<FieldType> fieldTypeFromName(const QString &name)
{
    for (const TypeInfo &t : kTypes) {
        if (name == QLatin1String(t.name))
            return t.type;
    }
    return std::nullopt;
}

const std::vector<FieldType> &allFieldTypes()
{
    static const std::vector<FieldType> types = [] {
        std::vector<FieldType> v;
        for (const TypeInfo &t : kTypes)
            v.push_back(t.type);
        return v;
    }();
    return types;
}

std::vector<FieldDef> buildLayout(int recordSize, const std::vector<FieldDef> &userFields,
                                  const std::vector<FieldDef> &guessed)
{
    std::vector<FieldDef> users;
    for (const FieldDef &f : userFields) {
        if (f.offset < 0 || f.end() > recordSize)
            continue;
        const bool overlaps = std::any_of(users.begin(), users.end(), [&](const FieldDef &u) {
            return f.offset < u.end() && u.offset < f.end();
        });
        if (!overlaps)
            users.push_back(f);
    }
    std::sort(users.begin(), users.end(), [](const FieldDef &a, const FieldDef &b) { return a.offset < b.offset; });

    std::vector<const FieldDef *> guessAt(size_t(std::max(recordSize, 0)), nullptr);
    for (const FieldDef &g : guessed) {
        if (g.offset >= 0 && g.end() <= recordSize)
            guessAt[size_t(g.offset)] = &g;
    }

    std::vector<FieldDef> layout;
    size_t next = 0;
    int pos = 0;
    while (pos < recordSize) {
        if (next < users.size() && users[next].offset == pos) {
            FieldDef f = users[next++];
            f.userDefined = true;
            layout.push_back(f);
            pos = f.end();
            continue;
        }
        const int limit = next < users.size() ? users[next].offset : recordSize;
        if (const FieldDef *g = guessAt[size_t(pos)]; g && g->end() <= limit) {
            FieldDef f = *g;
            f.userDefined = false;
            layout.push_back(f);
            pos = f.end();
            continue;
        }
        layout.push_back({pos, FieldType::U8, {}, false});
        ++pos;
    }
    return layout;
}

RecordTable::RecordTable(std::shared_ptr<const RecordSource> source, const std::vector<FieldDef> &userFields)
    : m_source(std::move(source))
{
    Q_ASSERT(m_source);
    std::vector<FieldDef> users;
    for (const FieldDef &f : userFields) {
        if (f.type != FieldType::Text || m_source->hasTextPointers())
            users.push_back(f);
    }
    m_layout = buildLayout(m_source->recordSize(), users, m_source->guessFields());
}

QString RecordTable::defaultLabel(const FieldDef &field)
{
    return QStringLiteral("+%1 %2").arg(hex(quint64(field.offset), 2), fieldTypeName(field.type));
}

QString RecordTable::columnName(int column) const
{
    const FieldDef &f = m_layout.at(size_t(column));
    return f.name.isEmpty() ? defaultLabel(f) : f.name;
}

QString RecordTable::columnToolTip(int column) const
{
    const FieldDef &f = m_layout.at(size_t(column));
    QString tip = QObject::tr("Offset %1, %2 (%3 bytes)")
                      .arg(hex(quint64(f.offset), 2), fieldTypeName(f.type))
                      .arg(f.length());
    tip += f.userDefined ? QObject::tr("\nSet by you") : QObject::tr("\nGuessed");
    return tip + QObject::tr("\nRight-click to rename or change the type");
}

const char *RecordTable::fieldData(int row, int column) const
{
    const QByteArrayView record = m_source->record(row);
    const FieldDef &f = m_layout.at(size_t(column));
    Q_ASSERT(record.size() >= f.end());
    return record.data() + f.offset;
}

QString RecordTable::text(int row, int column, DisplayMode mode) const
{
    const char *p = fieldData(row, column);
    const bool asHex = mode == DisplayMode::Hex;
    switch (m_layout.at(size_t(column)).type) {
    case FieldType::U8:  return asHex ? hex(quint8(*p), 2) : QString::number(quint8(*p));
    case FieldType::I8:  return asHex ? hex(quint8(*p), 2) : QString::number(qint8(*p));
    case FieldType::U16: return asHex ? hex(load<quint16>(p), 4) : QString::number(load<quint16>(p));
    case FieldType::I16: return asHex ? hex(load<quint16>(p), 4) : QString::number(load<qint16>(p));
    case FieldType::U32: return asHex ? hex(load<quint32>(p), 8) : QString::number(load<quint32>(p));
    case FieldType::I32: return asHex ? hex(load<quint32>(p), 8) : QString::number(load<qint32>(p));
    case FieldType::U64: return asHex ? hex(load<quint64>(p), 16) : QString::number(load<quint64>(p));
    case FieldType::F32: {
        const quint32 raw = load<quint32>(p);
        return asHex ? hex(raw, 8) : QString::number(std::bit_cast<float>(raw), 'g', 7);
    }
    case FieldType::Auto32: {
        const quint32 raw = load<quint32>(p);
        if (asHex)
            return hex(raw, 8);
        switch (guessKind(raw)) {
        case NumberKind::Int:   return QString::number(static_cast<qint32>(raw));
        case NumberKind::Float: return QString::number(std::bit_cast<float>(raw), 'g', 7);
        case NumberKind::Hex:   return hex(raw, 8);
        }
        break;
    }
    case FieldType::Chars: {
        const int size = m_layout.at(size_t(column)).length();
        if (asHex)
            return QString::fromLatin1(QByteArray(p, size).toHex(' '));
        return QString::fromUtf8(p, qsizetype(qstrnlen(p, uint(size)))).replace(QLatin1Char('\n'), QStringLiteral(" ↵ "));
    }
    case FieldType::Text: {
        const quint64 pointer = load<quint64>(p);
        if (asHex)
            return hex(pointer, 8);
        if (pointer == 0)
            return {};
        if (std::optional<QString> s = m_source->textAt(pointer))
            return s->replace(QLatin1Char('\n'), QStringLiteral(" ↵ "));
        return hex(pointer, 8);
    }
    }
    return {};
}

QVariant RecordTable::sortKey(int row, int column) const
{
    const char *p = fieldData(row, column);
    switch (m_layout.at(size_t(column)).type) {
    case FieldType::U8:  return double(quint8(*p));
    case FieldType::I8:  return double(qint8(*p));
    case FieldType::U16: return double(load<quint16>(p));
    case FieldType::I16: return double(load<qint16>(p));
    case FieldType::U32: return double(load<quint32>(p));
    case FieldType::I32: return double(load<qint32>(p));
    case FieldType::U64: return double(load<quint64>(p));
    case FieldType::F32: return double(std::bit_cast<float>(load<quint32>(p)));
    case FieldType::Auto32: {
        const quint32 raw = load<quint32>(p);
        switch (guessKind(raw)) {
        case NumberKind::Int:   return double(static_cast<qint32>(raw));
        case NumberKind::Float: return double(std::bit_cast<float>(raw));
        case NumberKind::Hex:   return double(raw);
        }
        return double(raw);
    }
    case FieldType::Text:
    case FieldType::Chars:
        return text(row, column, DisplayMode::Auto);
    }
    return {};
}

QString RecordTable::toolTip(int row, int column) const
{
    const QByteArrayView record = m_source->record(row);
    const FieldDef &f = m_layout.at(size_t(column));
    const qsizetype available = record.size() - f.offset;
    const char *p = record.data() + f.offset;

    QString tip = QObject::tr("Offset %1 in the record").arg(hex(quint64(f.offset), 2));
    tip += QObject::tr("\nu8: %1   i8: %2").arg(quint8(*p)).arg(qint8(*p));
    if (available >= 2)
        tip += QObject::tr("\nu16: %1   i16: %2").arg(load<quint16>(p)).arg(load<qint16>(p));
    if (available >= 4) {
        const quint32 raw = load<quint32>(p);
        tip += QObject::tr("\nu32: %1   i32: %2\nf32: %3\nhex: %4")
                   .arg(raw)
                   .arg(static_cast<qint32>(raw))
                   .arg(double(std::bit_cast<float>(raw)), 0, 'g', 7)
                   .arg(hex(raw, 8));
    }
    if (available >= 8 && m_source->hasTextPointers()) {
        if (std::optional<QString> s = m_source->textAt(load<quint64>(p)))
            tip += QObject::tr("\ntext: %1").arg(s->left(80));
    }
    return tip;
}

bool RecordTable::isNumeric(int column) const
{
    const FieldType type = m_layout.at(size_t(column)).type;
    return type != FieldType::Text && type != FieldType::Chars;
}

QString RecordTable::description() const
{
    QString d = QObject::tr("%1 records × %2 bytes").arg(m_source->recordCount()).arg(m_source->recordSize());
    if (const QString extra = m_source->description(); !extra.isEmpty())
        d += ' ' + extra;
    return d;
}
