#pragma once

#include "core/bytes.h"

#include <QStringList>

namespace byml {

enum Type : quint8 {
    HashDict = 0x20,
    HashDictRemap = 0x21,
    String = 0xA0,
    Binary = 0xA1,
    FileData = 0xA2,
    Array = 0xC0,
    Dict = 0xC1,
    StringTable = 0xC2,
    PathTable = 0xC3,
    Bool = 0xD0,
    Int = 0xD1,
    Float = 0xD2,
    UInt = 0xD3,
    Int64 = 0xD4,
    UInt64 = 0xD5,
    Double = 0xD6,
    Null = 0xFF,
};

inline bool isContainer(quint8 type)
{
    return type == Array || type == Dict || type == HashDict || type == HashDictRemap;
}

class Bytes : public ByteReader
{
public:
    explicit Bytes(const QByteArray &data) : ByteReader(data, data.startsWith("YB")) {}

    quint16 version() const { return u16(2); }

    quint32 u24(qint64 at) const
    {
        need(at, 3);
        const auto *p = reinterpret_cast<const uchar *>(data().constData() + at);
        return little() ? quint32(p[0] | p[1] << 8 | p[2] << 16) : quint32(p[2] | p[1] << 8 | p[0] << 16);
    }

    QStringList stringTable(qint64 at) const
    {
        if (u8(at) != StringTable)
            parseFailure(QObject::tr("A string table has the wrong type."));
        const quint32 count = u24(at + 1);
        need(at + 4, qint64(count) * 4);
        QStringList out;
        out.reserve(count);
        for (quint32 i = 0; i < count; ++i)
            out << cstr(at + u32(at + 4 + qint64(i) * 4));
        return out;
    }
};

}
