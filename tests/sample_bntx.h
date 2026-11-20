#pragma once

#include "textures/texturedecode.h"

#include <QByteArray>
#include <QtEndian>

#include <cstring>
#include <vector>

namespace samplebntx {

struct Texture
{
    QByteArray name;
    quint32 format = 0x0B01;
    int width = 8;
    int height = 8;
    quint32 channels = 0x05040302;
    QByteArray linear;
    int elementBytes = 4;
    int elementWidth = 1;
    int elementHeight = 1;
    int blockHeightLog2 = 0;
    int mips = 1;
    std::vector<QByteArray> mipData;
};

inline void put16(QByteArray &b, qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); }
inline void put32(QByteArray &b, qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); }
inline void put64(QByteArray &b, qsizetype at, quint64 v) { qToLittleEndian(v, b.data() + at); }

inline qsizetype alignTo(qsizetype v, qsizetype a) { return (v + a - 1) / a * a; }

inline QByteArray make(const std::vector<Texture> &textures)
{
    const qsizetype count = qsizetype(textures.size());
    const qsizetype pointers = 0x60;
    const qsizetype infos = alignTo(pointers + count * 8, 0x10);
    qsizetype strings = infos + count * 0x100;
    QByteArray b(strings, '\0');

    std::vector<qsizetype> nameAt, mipArrayAt;
    for (const Texture &t : textures) {
        nameAt.push_back(b.size());
        QByteArray s(2, '\0');
        qToLittleEndian(quint16(t.name.size()), s.data());
        s += t.name;
        s += '\0';
        b += s;
        b += QByteArray(alignTo(b.size(), 8) - b.size(), '\0');
    }
    const qsizetype fileNameAt = b.size() + 2;
    {
        QByteArray s(2, '\0');
        qToLittleEndian(quint16(6), s.data());
        b += s + QByteArray("sample") + '\0';
        b += QByteArray(alignTo(b.size(), 8) - b.size(), '\0');
    }
    for (const Texture &t : textures) {
        mipArrayAt.push_back(b.size());
        b += QByteArray(t.mips * 8, '\0');
    }

    const qsizetype brtd = alignTo(b.size(), 0x1000) - 0x10;
    b += QByteArray(brtd - b.size(), '\0');
    b += QByteArray(0x10, '\0');
    std::memcpy(b.data() + brtd, "BRTD", 4);

    std::vector<qsizetype> dataAt, dataSize;
    std::vector<std::vector<qsizetype>> mipAts;
    for (const Texture &t : textures) {
        b += QByteArray(alignTo(b.size(), 0x200) - b.size(), '\0');
        const int columns = (t.width + t.elementWidth - 1) / t.elementWidth;
        const int rows = (t.height + t.elementHeight - 1) / t.elementHeight;
        const QByteArray swizzled =
            *swizzleBlockLinear(t.linear, columns, rows, t.elementBytes, 1 << t.blockHeightLog2);
        dataAt.push_back(b.size());
        mipAts.push_back({b.size()});
        b += swizzled;
        for (int m = 1; m < t.mips; ++m) {
            b += QByteArray(alignTo(b.size(), 0x200) - b.size(), '\0');
            mipAts.back().push_back(b.size());
            b += size_t(m) <= t.mipData.size() ? t.mipData[size_t(m) - 1] : QByteArray(64, char(m));
        }
        dataSize.push_back(b.size() - dataAt.back());
    }
    put64(b, brtd + 8, quint64(b.size() - brtd));
    b += QByteArray(alignTo(b.size(), 0x10) - b.size(), '\0');

    std::memcpy(b.data(), "BNTX", 4);
    put32(b, 8, 0x00040100);
    put16(b, 0x0C, 0xFEFF);
    b[0x0E] = 0x0C;
    b[0x0F] = 0x40;
    put32(b, 0x10, quint32(fileNameAt));
    put32(b, 0x1C, quint32(b.size()));
    std::memcpy(b.data() + 0x20, "NX  ", 4);
    put32(b, 0x24, quint32(count));
    put64(b, 0x28, quint64(pointers));
    put64(b, 0x30, quint64(brtd));

    for (qsizetype i = 0; i < count; ++i) {
        const Texture &t = textures[size_t(i)];
        const qsizetype at = infos + i * 0x100;
        put64(b, pointers + i * 8, quint64(at));
        std::memcpy(b.data() + at, "BRTI", 4);
        put32(b, at + 4, 0xA0);
        put32(b, at + 8, 0xA0);
        b[at + 0x10] = 0x01;
        b[at + 0x11] = 0x02;
        put16(b, at + 0x16, quint16(t.mips));
        put32(b, at + 0x18, 1);
        put32(b, at + 0x1C, t.format);
        put32(b, at + 0x24, quint32(t.width));
        put32(b, at + 0x28, quint32(t.height));
        put32(b, at + 0x2C, 1);
        put32(b, at + 0x30, 1);
        put32(b, at + 0x34, quint32(t.blockHeightLog2));
        put32(b, at + 0x38, 0x10007);
        put32(b, at + 0x50, quint32(dataSize[size_t(i)]));
        put32(b, at + 0x54, 0x200);
        put32(b, at + 0x58, t.channels);
        put32(b, at + 0x5C, 1);
        put64(b, at + 0x60, quint64(nameAt[size_t(i)]));
        put64(b, at + 0x68, 0x20);
        put64(b, at + 0x70, quint64(mipArrayAt[size_t(i)]));
        for (int m = 0; m < t.mips; ++m)
            put64(b, mipArrayAt[size_t(i)] + m * 8, quint64(mipAts[size_t(i)][size_t(m)]));
    }
    return b;
}

inline QByteArray embedIn(const char *magic, const QByteArray &bntx, qsizetype at = 0x1000)
{
    QByteArray b(at, '\0');
    std::memcpy(b.data(), magic, 4);
    for (qsizetype i = 8; i < at; ++i)
        b[i] = char(i * 13);
    b += bntx;
    b += QByteArray(300, '\x55');
    return b;
}

}
