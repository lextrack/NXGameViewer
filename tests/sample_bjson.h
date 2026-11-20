#pragma once

// Builds a tiny hand-made Falcom binary JSON file for tests (no game data). Equivalent to:
//   {"name": "Hero", "hp": 12.5, "tags": ["a", 1], "alive": true}

#include <QByteArray>
#include <QtEndian>

#include <bit>
#include <cstring>

namespace samplejson {

struct Offsets
{
    qsizetype root = 0, aliveNode = 0, tagsNode = 0;
};

inline void appendU32(QByteArray &b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

inline QByteArray make(Offsets *offsets = nullptr, const char *heroName = "Hero")
{
    QByteArray b("JSON", 4);
    appendU32(b, 0);
    appendU32(b, 0);  // root offset, filled in at the end
    appendU32(b, 0);

    auto key = [&](quint32 hash, const char *name) {
        const qsizetype at = b.size();
        appendU32(b, hash);
        b.append(name, qsizetype(std::strlen(name)) + 1);
        return at;
    };
    const qsizetype kRoot = key(0xFFFFFFFF, "");
    const qsizetype kName = key(1, "name");
    const qsizetype kHp = key(2, "hp");
    const qsizetype kTags = key(3, "tags");
    const qsizetype kAlive = key(4, "alive");

    auto start = [&](quint8 tag, qsizetype keyOffset) {
        const qsizetype at = b.size();
        b.append(char(tag));
        if (!(tag & 0x10))
            appendU32(b, quint32(keyOffset));
        return at;
    };
    auto appendDouble = [&](double v) {
        char raw[8];
        qToLittleEndian(std::bit_cast<quint64>(v), raw);
        b.append(raw, 8);
    };

    const qsizetype name = start(0x02, kName);
    b.append(heroName, qsizetype(std::strlen(heroName)) + 1);
    const qsizetype hp = start(0x03, kHp);
    appendDouble(12.5);
    const qsizetype t0 = start(0x12, 0);  // keyless string
    b.append("a", 2);
    const qsizetype t1 = start(0x13, 0);  // keyless number
    appendDouble(1.0);
    const qsizetype tags = start(0x05, kTags);
    appendU32(b, 2);
    appendU32(b, quint32(t0));
    appendU32(b, quint32(t1));
    const qsizetype alive = start(0x06, kAlive);
    b.append(char(1));
    const qsizetype root = start(0x04, kRoot);
    appendU32(b, 4);
    for (qsizetype child : {name, hp, tags, alive})
        appendU32(b, quint32(child));

    qToLittleEndian(quint32(root), b.data() + 8);
    if (offsets)
        *offsets = {root, alive, tags};
    return b;
}

// A list of `count` items that all point back at the list itself: tiny on disk,
// enormous if a parser follows it without limits.
inline QByteArray makeSelfReferencingList(quint32 count)
{
    QByteArray b("JSON", 4);
    appendU32(b, 0);
    appendU32(b, 0x15);  // root node offset
    appendU32(b, 0);
    appendU32(b, 0xFFFFFFFF);  // key table: the root's empty key at 0x10
    b.append('\0');
    b.append(char(0x05));      // array node at 0x15
    appendU32(b, 0x10);
    appendU32(b, count);
    for (quint32 i = 0; i < count; ++i)
        appendU32(b, 0x15);
    return b;
}

}
