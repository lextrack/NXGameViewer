#include "data/ed9bytecode.h"

#include <QtEndian>

#include <cstring>

namespace ed9 {

namespace {

constexpr int kOperandBytes[0x29] = {
    -1, 1, 4, 4, 4, 4, 4, 4, 4, 1, 1, 4, 2, 0, 4, 4,  // 0x00-0x0F
    0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x1F: operators
    0,  0, 9, 9, 3, 4, 2, 1, 4,                       // 0x20-0x28
};

constexpr const char *kNames[0x29] = {
    "PUSH", "POP", "RETRIEVEELEMENTATINDEX", "RETRIEVEELEMENTATINDEX2", "PUSHCONVERTINTEGER",
    "PUTBACKATINDEX", "PUTBACK", "LOAD32", "STORE32", "LOADRESULT", "SAVERESULT", "JUMP", "CALL", "EXIT",
    "JUMPIFTRUE", "JUMPIFFALSE", "ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "MODULO", "EQUAL", "NONEQUAL",
    "GREATERTHAN", "GREATEROREQ", "LOWERTHAN", "LOWEROREQ", "AND", "OR1", "OR2", "OR3", "NEGATIVE",
    "ISFALSE", "XOR1", "CALLFROMANOTHERSCRIPT", "CALLFROMANOTHERSCRIPT2", "RUNCMD",
    "PUSHRETURNADDRESSFROMANOTHERSCRIPT", "ADDLINEMARKER", "POP2", "DEBUG",
};

QString commandGroup(int group)
{
    static const char *const names[] = {
        "system", "characters", "camera", "event", nullptr, "text", "sound", nullptr, "movies", nullptr,
        nullptr, "map", "party", "btl", nullptr, "menu", nullptr, nullptr, "portraits", nullptr,
        "activevoice", nullptr, "mapjump", "ui", nullptr, "battlestatus", "shop", nullptr, "achievements",
    };
    if (group >= 0 && group < int(std::size(names)) && names[group])
        return QString::fromLatin1(names[group]);
    return QStringLiteral("group%1").arg(group);
}

quint32 u32(const QByteArray &d, qsizetype pos) { return qFromLittleEndian<quint32>(d.constData() + pos); }
qint32 i32(const QByteArray &d, qsizetype pos) { return qint32(u32(d, pos)); }

QString hex(quint32 v) { return QStringLiteral("0x") + QString::number(v, 16).toUpper(); }

}

int instructionSize(const QByteArray &data, qsizetype pos)
{
    if (pos < 0 || pos >= data.size())
        return -1;
    const int op = uchar(data[pos]);
    int size;
    if (op == 0) {
        if (pos + 1 >= data.size())
            return -1;
        size = 2 + uchar(data[pos + 1]);
    } else if (op < int(std::size(kOperandBytes))) {
        size = 1 + kOperandBytes[op];
    } else {
        return -1;
    }
    return size <= data.size() - pos ? size : -1;
}

QString valueText(const QByteArray &data, quint32 value)
{
    const qint32 payload = qint32(value << 2) >> 2;
    switch (value >> 30) {
    case 1:
        return QString::number(payload);
    case 2: {
        const quint32 bits = quint32(payload) << 2;
        float f;
        std::memcpy(&f, &bits, 4);
        return QString::number(double(f), 'g', 7);
    }
    case 3: {
        const quint32 offset = value & 0x3FFFFFFF;
        if (offset >= quint32(data.size()))
            return QStringLiteral("string(%1)").arg(hex(offset));
        QString text = QString::fromUtf8(data.constData() + offset);
        text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        return QLatin1Char('"') + text + QLatin1Char('"');
    }
    default:
        return QStringLiteral("undefined(%1)").arg(hex(value));
    }
}

Instruction decode(const QByteArray &data, qsizetype pos)
{
    const int op = uchar(data[pos]);
    const qsizetype a = pos + 1;
    Instruction in{QString::fromLatin1(kNames[op]), {}};
    switch (op) {
    case 0x00: {
        const int size = uchar(data[a]);
        if (size == 4) {
            in.arguments = valueText(data, u32(data, a + 1));
        } else {
            quint64 v = 0;
            for (int i = 0; i < size && i < 8; ++i)
                v |= quint64(uchar(data[a + 1 + i])) << (8 * i);
            in.arguments = QStringLiteral("%1 (%2 bytes)").arg(v).arg(size);
        }
        break;
    }
    case 0x01: case 0x09: case 0x0A: case 0x27:
        in.arguments = QString::number(uchar(data[a]));
        break;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
        in.arguments = QString::number(i32(data, a));
        break;
    case 0x0B: case 0x0E: case 0x0F:
        in.arguments = QStringLiteral("→ %1").arg(hex(u32(data, a)));
        break;
    case 0x0C:
        in.arguments = QStringLiteral("function %1").arg(qFromLittleEndian<quint16>(data.constData() + a));
        break;
    case 0x22: case 0x23:
        in.arguments = QStringLiteral("%1 . %2, %3 args")
                           .arg(valueText(data, u32(data, a)), valueText(data, u32(data, a + 4)))
                           .arg(uchar(data[a + 8]));
        break;
    case 0x24:
        in.arguments = QStringLiteral("%1_%2, %3 args")
                           .arg(commandGroup(uchar(data[a])))
                           .arg(uchar(data[a + 1]), 2, 16, QLatin1Char('0'))
                           .arg(uchar(data[a + 2]));
        break;
    case 0x25: case 0x28:
        in.arguments = hex(u32(data, a));
        break;
    case 0x26:
        in.arguments = QString::number(qFromLittleEndian<quint16>(data.constData() + a));
        break;
    default:
        break;
    }
    return in;
}

}
