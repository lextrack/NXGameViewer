#pragma once

#include "archives/compress.h"
#include "archives/rpf6reader.h"

#include <QByteArray>
#include <QtEndian>

#include <cstring>
#include <vector>

#include <zlib.h>

namespace samplerpf6 {


inline QByteArray testKey()
{
    QByteArray key(32, '\0');
    for (int i = 0; i < 32; ++i)
        key[i] = char(0x11 * (i % 15) + i * 7 + 3);
    return key;
}

inline void putBe(QByteArray &b, qsizetype at, quint32 v)
{
    qToBigEndian(v, b.data() + at);
}

inline QByteArray rawDeflate(const QByteArray &data)
{
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&z, uLong(data.size()))), '\0');
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    z.avail_in = uInt(data.size());
    z.next_out = reinterpret_cast<Bytef *>(out.data());
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(qsizetype(z.total_out));
    deflateEnd(&z);
    return out;
}

inline QByteArray zlibStream(const QByteArray &data)
{
    return qCompress(data).mid(4);
}

struct Sample
{
    QByteArray file;
    QByteArray plain = "plain text file\n";
    QByteArray packed = QByteArray(3000, 'z') + "end";
    QByteArray zlibbed = "zlib data, zlib data, zlib data";
    QByteArray deflated = QByteArray("raw deflate ") .repeated(20);
    QByteArray resource = QByteArray("resource body ").repeated(30);
    QByteArray script = QByteArray("script code ").repeated(10);
};

inline Sample makeArchive(bool encrypted = true)
{
    Sample s;
    const QByteArray key = testKey();
    QString error;
    const QByteArray zstdPacked = *compressZstd(s.packed, 3, false, &error);
    const QByteArray zstdResource = *compressZstd(s.resource, 3, false, &error);
    const QByteArray zstdScript = *compressZstd(s.script, 3, false, &error);
    QByteArray encryptedScript = rpf6Encrypt(key, zstdScript);

    struct File { const char *name; QByteArray stored; quint32 c; qint64 at = 0; bool resource = false; };
    std::vector<File> rootFiles = {
        {"plain.txt", s.plain, quint32(s.plain.size())},
        {"packed.bin", zstdPacked, 0x40000000u | quint32(s.packed.size())},
    };
    std::vector<File> subFiles = {
        {"zlib.dat", zlibStream(s.zlibbed), 0x40000000u | quint32(s.zlibbed.size())},
        {"config.dat", rawDeflate(s.deflated), 0x40000000u | quint32(s.deflated.size())},
        {"thing.wtd", QByteArray("RSC\x05", 4), 0xC0000123u, 0, true},
        {"logic.wsc", encryptedScript, 0x40000000u | quint32(s.script.size())},
    };
    QByteArray rsc(12, '\0');
    std::memcpy(rsc.data(), "RSC\x05", 4);
    qToLittleEndian<quint32>(0x1e, rsc.data() + 4);
    qToLittleEndian<quint32>(0xC0000123u, rsc.data() + 8);
    subFiles[2].stored = rsc + zstdResource;

    const int count = 1 + int(rootFiles.size()) + 1 + int(subFiles.size());
    QByteArray data;
    qint64 at = 0x1000;
    auto place = [&](File &f) {
        at = f.resource ? (at + 2047) / 2048 * 2048 : (at + 7) / 8 * 8;
        f.at = at;
        if (data.size() < at - 0x1000)
            data.resize(at - 0x1000, '\0');
        data.resize(at - 0x1000);
        data += f.stored;
        at += f.stored.size();
    };
    for (File &f : rootFiles)
        place(f);
    for (File &f : subFiles)
        place(f);

    QByteArray table(count * 20, '\0');
    auto entry = [&](int i, quint32 hash, quint32 a, quint32 b, quint32 c) {
        putBe(table, i * 20, hash);
        putBe(table, i * 20 + 4, a);
        putBe(table, i * 20 + 8, b);
        putBe(table, i * 20 + 12, c);
    };
    entry(0, 0, 0, 0x80000001u, quint32(rootFiles.size() + 1));
    int index = 1;
    for (const File &f : rootFiles)
        entry(index++, joaat(f.name), quint32(f.stored.size()), quint32(f.at / 8), f.c);
    const int sub = index++;
    entry(sub, joaat("sub"), 0, 0x80000000u | quint32(index), quint32(subFiles.size()));
    for (const File &f : subFiles)
        entry(index++, joaat(f.name), quint32(f.stored.size()),
              f.resource ? (quint32(f.at / 8) & 0x7FFFFF00u) | 0x1e : quint32(f.at / 8), f.c);

    QByteArray names("\x01\x02\x03\x04", 4);
    for (const char *n : {"sub", "plain.txt", "packed.bin", "zlib.dat", "config.dat", "thing.wtd", "logic.wsc"})
        names += QByteArray("\x7f\x7f") + n + '\0';
    names.resize((names.size() + 15) / 16 * 16, '\0');

    QByteArray paddedTable = table;
    paddedTable.resize((table.size() + 15) / 16 * 16, '\0');
    s.file = QByteArray(16, '\0');
    std::memcpy(s.file.data(), "RPF6", 4);
    putBe(s.file, 4, quint32(count));
    putBe(s.file, 12, encrypted ? Rpf6Reader::kEncrypted : 0);
    s.file += encrypted ? rpf6Encrypt(key, paddedTable) : paddedTable;
    s.file.resize(0x1000, '\0');
    s.file += data;
    s.file.resize((s.file.size() + 7) / 8 * 8, '\0');
    putBe(s.file, 8, quint32(s.file.size() / 8));
    s.file += encrypted ? rpf6Encrypt(key, names) : names;
    return s;
}

inline QByteArray nso(const QByteArray &text, const QByteArray &ro, const QByteArray &data)
{
    QByteArray out(0x100, '\0');
    std::memcpy(out.data(), "NSO0", 4);
    const QByteArray segments[3] = {text, ro, data};
    quint32 memory = 0;
    for (int s = 0; s < 3; ++s) {
        qToLittleEndian<quint32>(quint32(out.size()), out.data() + 0x10 + s * 0x10);
        qToLittleEndian<quint32>(memory, out.data() + 0x14 + s * 0x10);
        qToLittleEndian<quint32>(quint32(segments[s].size()), out.data() + 0x18 + s * 0x10);
        qToLittleEndian<quint32>(quint32(segments[s].size()), out.data() + 0x60 + s * 4);
        out += segments[s];
        memory += quint32((segments[s].size() + 0xFFF) / 0x1000 * 0x1000);
    }
    return out;
}

inline QByteArray instructions(std::initializer_list<quint32> words)
{
    QByteArray out;
    for (quint32 w : words) {
        char b[4];
        qToLittleEndian(w, b);
        out.append(b, 4);
    }
    return out;
}

}
