#pragma once

// Builds a tiny hand-made "#TBL" file for tests (no game data).

#include <QByteArray>
#include <QtEndian>

#include <bit>
#include <cstring>

namespace sample {


inline void putU32(QByteArray &b, qsizetype pos, quint32 v) { qToLittleEndian(v, b.data() + pos); }
inline void putU64(QByteArray &b, qsizetype pos, quint64 v) { qToLittleEndian(v, b.data() + pos); }

inline void putSection(QByteArray &b, int i, const char *name, quint32 offset, quint32 size, quint32 count)
{
    const qsizetype h = 8 + i * 80;
    std::memcpy(b.data() + h, name, std::strlen(name));
    putU32(b, h + 64, 0x1234);
    putU32(b, h + 68, offset);
    putU32(b, h + 72, size);
    putU32(b, h + 76, count);
}

// A small hand-made file in the same layout as the game's tables (no game data):
//   header   0..8      "#TBL", 2 sections
//   headers  8..168
//   ItemData 168..200  2 entries × 16 bytes: text pointer, int, float
//   Other    200..212  3 entries × 4 bytes
//   strings  212..     "Sword\0Shield\0"
inline QByteArray makeSample()
{
    QByteArray b(212, '\0');
    std::memcpy(b.data(), "#TBL", 4);
    putU32(b, 4, 2);
    putSection(b, 0, "ItemData", 168, 16, 2);
    putSection(b, 1, "Other", 200, 4, 3);
    b.append("Sword", 6);   // includes '\0'
    b.append("Shield", 7);

    putU64(b, 168, 212);                          // "Sword"
    putU32(b, 176, 42);
    putU32(b, 180, std::bit_cast<quint32>(1.5f));
    putU64(b, 184, 218);                          // "Shield"
    putU32(b, 192, quint32(-1));
    putU32(b, 196, std::bit_cast<quint32>(0.25f));

    putU32(b, 200, 7);
    putU32(b, 204, 8);
    putU32(b, 208, 0xDEADBEEF);
    return b;
}

}
