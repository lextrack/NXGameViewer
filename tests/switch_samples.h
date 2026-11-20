#pragma once

#include "core/crypto.h"
#include "switchfs/switchkeys.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <zstd.h>

#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace switchsamples {

inline const QByteArray kHeaderKey = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f" "f0e0d0c0b0a090807060504030201000");
inline const QByteArray kAreaKey = QByteArray::fromHex("11223344556677881122334455667788");
inline const QByteArray kTitlekek = QByteArray::fromHex("99aabbccddeeff0099aabbccddeeff00");
inline const QByteArray kTitleKey = QByteArray::fromHex("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
inline const QByteArray kRightsId = QByteArray::fromHex("0100aaaa00000000000000000000000a");
inline const QByteArray kProgramKey = QByteArray::fromHex("cafebabecafebabecafebabecafebabe");

inline void put16(QByteArray &b, qsizetype at, quint16 v) { qToLittleEndian(v, b.data() + at); }
inline void put32(QByteArray &b, qsizetype at, quint32 v) { qToLittleEndian(v, b.data() + at); }
inline void put64(QByteArray &b, qsizetype at, quint64 v) { qToLittleEndian(v, b.data() + at); }

inline QByteArray keysText(int generation = 0)
{
    const QByteArray g = QByteArray::number(generation, 16).rightJustified(2, '0');
    return "; test keys\n"
           "header_key = " + kHeaderKey.toHex() + "\n"
           "key_area_key_application_" + g + " = " + kAreaKey.toHex() + "\n"
           "titlekek_" + g + " = " + kTitlekek.toHex() + "\n";
}

inline SwitchKeys testKeys()
{
    QString error;
    return *SwitchKeys::fromText(keysText(), {}, &error);
}

inline QByteArray partition(const std::vector<std::pair<QByteArray, QByteArray>> &files, bool hfs = false)
{
    QByteArray strings;
    std::vector<quint32> nameAt;
    for (const auto &f : files) {
        nameAt.push_back(quint32(strings.size()));
        strings += f.first + '\0';
    }
    while (strings.size() % 0x20)
        strings += '\0';
    const int entrySize = hfs ? 0x40 : 0x18;
    QByteArray header(16 + entrySize * int(files.size()), '\0');
    std::memcpy(header.data(), hfs ? "HFS0" : "PFS0", 4);
    put32(header, 4, quint32(files.size()));
    put32(header, 8, quint32(strings.size()));
    QByteArray data;
    for (size_t i = 0; i < files.size(); ++i) {
        const int at = 16 + entrySize * int(i);
        put64(header, at, quint64(data.size()));
        put64(header, at + 8, quint64(files[i].second.size()));
        put32(header, at + 16, nameAt[i]);
        data += files[i].second;
    }
    return header + strings + data;
}

inline QByteArray romfsImage(const QByteArray &a, const QByteArray &b, const QByteArray &badName = {})
{
    auto entryName = [](QByteArray name) {
        while (name.size() % 4)
            name += '\0';
        return name;
    };
    QByteArray dirs(0x18 * 2 + 4, '\0');
    const quint32 none = 0xFFFFFFFFu;
    put32(dirs, 0, 0);
    put32(dirs, 4, none);
    put32(dirs, 8, 0x18);
    put32(dirs, 12, 0);
    put32(dirs, 16, none);
    put32(dirs, 20, 0);
    put32(dirs, 0x18 + 0, 0);
    put32(dirs, 0x18 + 4, none);
    put32(dirs, 0x18 + 8, none);
    put32(dirs, 0x18 + 16, none);
    put32(dirs, 0x18 + 20, 3);
    dirs.truncate(0x18 * 2);
    dirs += entryName("sub");
    const quint32 secondFile = 0x20 + quint32(entryName(badName.isEmpty() ? QByteArray("a.txt") : badName).size());
    put32(dirs, 0x18 + 12, secondFile);
    QByteArray files;
    auto file = [&](quint32 parent, quint32 sibling, quint64 offset, quint64 size, const QByteArray &name) {
        QByteArray e(0x20, '\0');
        put32(e, 0, parent);
        put32(e, 4, sibling);
        put64(e, 8, offset);
        put64(e, 16, size);
        put32(e, 24, none);
        put32(e, 28, quint32(name.size()));
        files += e + entryName(name);
    };
    file(0, none, 0, quint64(a.size()), badName.isEmpty() ? QByteArray("a.txt") : badName);
    file(0x18, none, quint64((a.size() + 15) / 16 * 16), quint64(b.size()), "b.bin");
    QByteArray data = a;
    while (data.size() % 16)
        data += '\0';
    data += b;
    QByteArray header(0x50, '\0');
    QByteArray hashes(4, char(0xFF));
    put64(header, 0, 0x50);
    const quint64 dirHash = 0x50, dirMeta = dirHash + 4, fileHash = dirMeta + quint64(dirs.size());
    const quint64 fileMeta = fileHash + 4, dataAt = (fileMeta + quint64(files.size()) + 15) / 16 * 16;
    put64(header, 0x08, dirHash);
    put64(header, 0x10, 4);
    put64(header, 0x18, dirMeta);
    put64(header, 0x20, quint64(dirs.size()));
    put64(header, 0x28, fileHash);
    put64(header, 0x30, 4);
    put64(header, 0x38, fileMeta);
    put64(header, 0x40, quint64(files.size()));
    put64(header, 0x48, dataAt);
    QByteArray image = header + hashes + dirs + hashes + files;
    image += QByteArray(qsizetype(dataAt) - image.size(), '\0');
    return image + data;
}

struct Section
{
    bool romfs = false;
    QByteArray data;
    QByteArray fsHeader;
};

inline Section pfsSection(const QByteArray &pfs)
{
    Section s;
    s.data = pfs;
    s.fsHeader = QByteArray(0x200, '\0');
    s.fsHeader[2] = 1;
    s.fsHeader[3] = 2;
    put64(s.fsHeader, 8 + 0x38, 0);
    put64(s.fsHeader, 8 + 0x40, quint64(pfs.size()));
    return s;
}

inline Section romfsSection(const QByteArray &image)
{
    constexpr int shift = 9;
    constexpr qint64 block = qint64(1) << shift;
    QByteArray padded = image;
    while (padded.size() % block)
        padded += '\0';
    QByteArray hashes;
    for (qint64 at = 0; at < padded.size(); at += block)
        hashes += QCryptographicHash::hash(padded.mid(at, block), QCryptographicHash::Sha256);
    QByteArray hashArea = hashes;
    while (hashArea.size() % block)
        hashArea += '\0';
    Section s;
    s.romfs = true;
    s.data = hashArea + image;
    s.fsHeader = QByteArray(0x200, '\0');
    s.fsHeader[2] = 0;
    s.fsHeader[3] = 3;
    std::memcpy(s.fsHeader.data() + 8, "IVFC", 4);
    const int level4 = 8 + 0x10 + 4 * 0x18, level5 = 8 + 0x10 + 5 * 0x18;
    put64(s.fsHeader, level4, 0);
    put64(s.fsHeader, level4 + 8, quint64(hashes.size()));
    put32(s.fsHeader, level4 + 16, shift);
    put64(s.fsHeader, level5, quint64(hashArea.size()));
    put64(s.fsHeader, level5 + 8, quint64(image.size()));
    put32(s.fsHeader, level5 + 16, shift);
    return s;
}

inline QByteArray lz4Literals(const QByteArray &data)
{
    QByteArray out(1, char(std::min<qsizetype>(data.size(), 15) << 4));
    if (data.size() >= 15) {
        qsizetype rest = data.size() - 15;
        for (; rest >= 255; rest -= 255)
            out += char(255);
        out += char(rest);
    }
    return out + data;
}

inline Section compressedRomfsSection(const QByteArray &image)
{
    constexpr qsizetype chunk = 0x200;
    QByteArray physical, entries;
    quint32 count = 0;
    for (qsizetype at = 0; at < image.size(); at += chunk, ++count) {
        const QByteArray piece = image.mid(at, chunk);
        QByteArray e(0x18, '\0');
        put64(e, 0, quint64(at));
        put64(e, 8, quint64(physical.size()));
        if (piece == QByteArray(piece.size(), '\0')) {
            e[16] = 1;
        } else if (count % 2) {
            const QByteArray packed = lz4Literals(piece);
            e[16] = 3;
            put32(e, 20, quint32(packed.size()));
            physical += packed;
        } else {
            put32(e, 20, quint32(piece.size()));
            physical += piece;
        }
        entries += e;
    }
    while (physical.size() % 0x10)
        physical += '\0';
    QByteArray l1(0x4000, '\0'), node(0x4000, '\0');
    put32(l1, 4, 1);
    put64(l1, 8, quint64(image.size()));
    put32(node, 4, count);
    put64(node, 8, quint64(image.size()));
    node.replace(0x10, entries.size(), entries);
    const qint64 tableAt = physical.size();
    Section s = romfsSection(physical + l1 + node);
    put64(s.fsHeader, 0x178, quint64(tableAt));
    put64(s.fsHeader, 0x180, quint64(l1.size() + node.size()));
    std::memcpy(s.fsHeader.data() + 0x188, "BKTR", 4);
    put32(s.fsHeader, 0x18C, 1);
    put32(s.fsHeader, 0x190, count);
    return s;
}

inline QByteArray nca(int contentType, quint64 titleId, std::vector<Section> sections, bool titleKeyCrypto,
                      int generation = 0)
{
    QByteArray header(0xC00, '\0');
    std::memcpy(header.data() + 0x200, "NCA3", 4);
    header[0x205] = char(contentType);
    header[0x220] = char(generation > 0 ? generation + 1 : 0);
    put64(header, 0x210, titleId);
    const QByteArray key = titleKeyCrypto ? kTitleKey : kProgramKey;
    if (titleKeyCrypto) {
        header.replace(0x230, 16, kRightsId);
    } else {
        QByteArray area(0x40, '\0');
        area.replace(0x20, 16, kProgramKey);
        header.replace(0x300, 0x40, crypto::ecbEncrypt(kAreaKey, area));
    }
    QByteArray body;
    for (size_t i = 0; i < sections.size(); ++i) {
        Section &s = sections[i];
        while (s.data.size() % 0x200)
            s.data += '\0';
        const qint64 start = 0xC00 + body.size();
        s.fsHeader[4] = 3;
        for (int j = 0; j < 8; ++j)
            s.fsHeader[0x140 + j] = char(0x10 * int(i) + j);
        QByteArray upper(8, '\0');
        for (int j = 0; j < 8; ++j)
            upper[j] = s.fsHeader[0x140 + 7 - j];
        QByteArray encrypted = s.data;
        crypto::ctrApply(key, crypto::ctrCounter(upper, quint64(start)), encrypted.data(), encrypted.size());
        put32(header, 0x240 + 0x10 * int(i), quint32(start / 0x200));
        put32(header, 0x244 + 0x10 * int(i), quint32((start + s.data.size()) / 0x200));
        header.replace(0x400 + 0x200 * int(i), 0x200, s.fsHeader);
        body += encrypted;
    }
    put64(header, 0x208, quint64(0xC00 + body.size()));
    return crypto::xtsEncrypt(kHeaderKey, header, 0, 0x200) + body;
}

inline QByteArray ticket()
{
    QByteArray t(0x2C0, '\0');
    put32(t, 0, 0x10004);
    t.replace(0x180, 16, crypto::ecbEncrypt(kTitlekek, kTitleKey));
    t.replace(0x2A0, 16, kRightsId);
    return t;
}

inline QByteArray cnmt(quint64 titleId, quint8 type, const std::vector<std::pair<QByteArray, int>> &contents)
{
    QByteArray c(0x30 + 0x38 * int(contents.size()), '\0');
    put64(c, 0, titleId);
    put32(c, 8, 65536);
    c[0x0C] = char(type);
    put16(c, 0x0E, 0x10);
    put16(c, 0x10, quint16(contents.size()));
    for (size_t i = 0; i < contents.size(); ++i) {
        const int at = 0x30 + 0x38 * int(i);
        c.replace(at + 0x20, 16, contents[i].first);
        c[at + 0x36] = char(contents[i].second);
    }
    return c;
}

inline QByteArray nacp()
{
    QByteArray n(0x4000, '\0');
    std::memcpy(n.data(), "Test Game", 9);
    std::memcpy(n.data() + 0x200, "Tester", 6);
    std::memcpy(n.data() + 0x3060, "1.2.3", 5);
    return n;
}

inline const quint64 kTitleId = 0x0100AAAA00000000ull;
inline const QByteArray kProgramId = QByteArray(16, char(0x01));
inline const QByteArray kControlId = QByteArray(16, char(0x02));
inline const QByteArray kMetaId = QByteArray(16, char(0x03));

inline QByteArray singleFileRomfs(const QByteArray &name, const QByteArray &data)
{
    QByteArray dirs(0x18, '\0');
    const quint32 none = 0xFFFFFFFFu;
    put32(dirs, 4, none);
    put32(dirs, 8, none);
    put32(dirs, 12, 0);
    put32(dirs, 16, none);
    QByteArray padded = name;
    while (padded.size() % 4)
        padded += '\0';
    QByteArray files(0x20, '\0');
    put32(files, 4, none);
    put64(files, 16, quint64(data.size()));
    put32(files, 24, none);
    put32(files, 28, quint32(name.size()));
    files += padded;
    QByteArray header(0x50, '\0');
    put64(header, 0, 0x50);
    const quint64 dirMeta = 0x54, fileMeta = dirMeta + 0x18 + 4, dataAt = (fileMeta + quint64(files.size()) + 15) / 16 * 16;
    put64(header, 0x08, 0x50);
    put64(header, 0x10, 4);
    put64(header, 0x18, dirMeta);
    put64(header, 0x20, 0x18);
    put64(header, 0x28, dirMeta + 0x18);
    put64(header, 0x30, 4);
    put64(header, 0x38, fileMeta);
    put64(header, 0x40, quint64(files.size()));
    put64(header, 0x48, dataAt);
    QByteArray image = header + QByteArray(4, char(0xFF)) + dirs + QByteArray(4, char(0xFF)) + files;
    image += QByteArray(qsizetype(dataAt) - image.size(), '\0');
    return image + data;
}

inline std::vector<std::pair<QByteArray, QByteArray>> packageFiles(quint8 metaType = 0x80, const QByteArray &badRomfsName = {},
                                                            bool withTicket = true, int generation = 0,
                                                            const QByteArray &bigFile = {}, bool compressed = false)
{
    const QByteArray exefs = partition({{"main", "MAIN-CODE"}, {"main.npdm", "NPDM"}});
    const QByteArray bFile = bigFile.isEmpty() ? QByteArray(3000, 'B') : bigFile;
    const QByteArray image = romfsImage("hello", bFile, badRomfsName);
    const Section programRomfs = compressed ? compressedRomfsSection(image) : romfsSection(image);
    const QByteArray program = nca(0, kTitleId, {pfsSection(exefs), programRomfs}, true, generation);
    const QByteArray control = nca(2, kTitleId, {romfsSection(singleFileRomfs("control.nacp", nacp()))}, false, generation);
    const QByteArray meta = nca(1, kTitleId, {pfsSection(partition({{"Application_0100aaaa00000000.cnmt",
                                                                     cnmt(kTitleId, metaType, {{kProgramId, metaType == 0x82 ? 2 : 1},
                                                                                               {kControlId, 3}})}}))},
                                false, generation);
    std::vector<std::pair<QByteArray, QByteArray>> files{{kProgramId.toHex() + ".nca", program},
                                                         {kControlId.toHex() + ".nca", control},
                                                         {kMetaId.toHex() + ".cnmt.nca", meta}};
    if (withTicket)
        files.push_back({kRightsId.toHex() + ".tik", ticket()});
    return files;
}

inline QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        return {};
    return path;
}

inline QByteArray xci(const std::vector<std::pair<QByteArray, QByteArray>> &files)
{
    const QByteArray secure = partition(files, true);
    const QByteArray root = partition({{"update", partition({}, true)}, {"secure", secure}}, true);
    QByteArray head(0x200, '\0');
    std::memcpy(head.data() + 0x100, "HEAD", 4);
    put64(head, 0x130, 0x200);
    return head + root;
}

inline QByteArray decryptedNca(const QByteArray &file)
{
    const QByteArray header = crypto::xtsDecrypt(kHeaderKey, file.left(0xC00), 0, 0x200);
    const QByteArray key = header.mid(0x230, 16) != QByteArray(16, '\0') ? kTitleKey : kProgramKey;
    QByteArray plain = file;
    for (int i = 0; i < 4; ++i) {
        const qint64 start = qint64(qFromLittleEndian<quint32>(header.constData() + 0x240 + 0x10 * i)) * 0x200;
        const qint64 end = qint64(qFromLittleEndian<quint32>(header.constData() + 0x244 + 0x10 * i)) * 0x200;
        if (end <= start)
            continue;
        QByteArray upper(8, '\0');
        for (int j = 0; j < 8; ++j)
            upper[j] = header[0x400 + 0x200 * i + 0x140 + 7 - j];
        crypto::ctrApply(key, crypto::ctrCounter(upper, quint64(start)), plain.data() + start, end - start);
    }
    return plain;
}

inline QByteArray zstdFrame(const QByteArray &data)
{
    QByteArray out(qsizetype(ZSTD_compressBound(size_t(data.size()))), Qt::Uninitialized);
    out.truncate(qsizetype(ZSTD_compress(out.data(), size_t(out.size()), data.constData(), size_t(data.size()), 3)));
    return out;
}

struct NupFileSpec
{
    QByteArray path;
    QByteArray data;
    bool zstd = false;
};

struct NupPackageSpec
{
    quint64 titleId = 0;
    quint32 version = 0;
    quint8 kind = 0;
    std::vector<NupFileSpec> files;
    std::vector<std::pair<quint16, QByteArray>> meta;
};

inline QByteArray nupFile(const std::vector<NupPackageSpec> &packages)
{
    struct Dir
    {
        QByteArray name;
        std::map<QByteArray, int> dirs;
        std::map<QByteArray, int> files;
    };
    struct File
    {
        QByteArray name;
        const NupFileSpec *spec;
    };
    std::vector<Dir> dirs;
    std::vector<File> files;
    std::vector<std::array<int, 3>> roots;
    for (const NupPackageSpec &package : packages) {
        const int root = int(dirs.size());
        dirs.push_back({});
        for (const NupFileSpec &f : package.files) {
            const QList<QByteArray> parts = f.path.split('/');
            int dir = root;
            for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
                auto it = dirs[size_t(dir)].dirs.find(parts[i]);
                if (it == dirs[size_t(dir)].dirs.end()) {
                    const int made = int(dirs.size());
                    dirs.push_back({parts[i], {}, {}});
                    it = dirs[size_t(dir)].dirs.emplace(parts[i], made).first;
                }
                dir = it->second;
            }
            dirs[size_t(dir)].files.emplace(parts.back(), int(files.size()));
            files.push_back({parts.back(), &f});
        }
        auto child = [&](const char *name) {
            const auto it = dirs[size_t(root)].dirs.find(name);
            return it == dirs[size_t(root)].dirs.end() ? -1 : it->second;
        };
        roots.push_back({root, child("exefs"), child("romfs")});
    }
    QByteArray pool;
    auto intern = [&pool](const QByteArray &s) {
        const quint32 at = quint32(pool.size());
        pool += s;
        return at;
    };
    const quint32 dirCount = quint32(dirs.size());
    QByteArray children, dirRecords;
    for (const Dir &d : dirs) {
        std::map<QByteArray, quint32> sorted;
        for (const auto &[name, index] : d.dirs)
            sorted.emplace(name, quint32(index));
        for (const auto &[name, index] : d.files)
            sorted.emplace(name, dirCount + quint32(index));
        QByteArray r(0x18, '\0');
        put32(r, 0, intern(d.name));
        put32(r, 4, quint32(d.name.size()));
        put32(r, 8, quint32(children.size() / 4));
        put32(r, 12, quint32(sorted.size()));
        dirRecords += r;
        for (const auto &[name, node] : sorted) {
            QByteArray slot(4, '\0');
            put32(slot, 0, node);
            children += slot;
        }
    }
    const qint64 tocSize = 0x30 + 0x40 * qint64(packages.size()) + 0x18 * qint64(dirs.size()) + 0x50 * qint64(files.size())
                         + children.size();
    QByteArray fileRecords;
    std::vector<QByteArray> stored;
    for (const File &f : files) {
        QByteArray r(0x50, '\0');
        put32(r, 0, intern(f.name));
        put32(r, 4, quint32(f.name.size()));
        put64(r, 8, quint64(f.spec->data.size()));
        r.replace(0x20, 32, QCryptographicHash::hash(f.spec->data, QCryptographicHash::Sha256));
        const bool packed = f.spec->zstd && !f.spec->data.isEmpty();
        r[0x40] = char(packed ? 1 : 0);
        stored.push_back(packed ? zstdFrame(f.spec->data) : f.spec->data);
        fileRecords += r;
    }
    const qint64 fullToc = tocSize + pool.size();
    const qint64 blobAt = (0x80 + fullToc + 15) / 16 * 16;
    QByteArray blob;
    for (size_t i = 0; i < files.size(); ++i) {
        while ((blobAt + blob.size()) % 16)
            blob += '\0';
        if (!stored[i].isEmpty()) {
            put64(fileRecords, qsizetype(i) * 0x50 + 0x10, quint64(blobAt + blob.size()));
            put64(fileRecords, qsizetype(i) * 0x50 + 0x18, quint64(stored[i].size()));
        }
        blob += stored[i];
    }
    QByteArray packageRecords;
    for (size_t i = 0; i < packages.size(); ++i) {
        const NupPackageSpec &package = packages[i];
        QByteArray r(0x40, '\0');
        put64(r, 0, package.titleId);
        put32(r, 8, quint32(roots[i][0]));
        put32(r, 0x0C, roots[i][1] < 0 ? 0xFFFFFFFFu : quint32(roots[i][1]));
        put32(r, 0x10, roots[i][2] < 0 ? 0xFFFFFFFFu : quint32(roots[i][2]));
        put32(r, 0x14, package.version);
        r[0x18] = char(package.kind);
        if (!package.meta.empty()) {
            QByteArray meta;
            for (const auto &[type, payload] : package.meta) {
                QByteArray h(8, '\0');
                put16(h, 0, type);
                put32(h, 4, quint32(payload.size()));
                meta += h + payload;
            }
            while ((blobAt + blob.size()) % 16)
                blob += '\0';
            put64(r, 0x20, quint64(blobAt + blob.size()));
            put32(r, 0x28, quint32(meta.size()));
            blob += meta;
        }
        packageRecords += r;
    }
    QByteArray toc(0x30, '\0');
    std::memcpy(toc.data(), "TOC\x01", 4);
    put32(toc, 4, quint32(packages.size()));
    put64(toc, 8, dirCount);
    put64(toc, 0x10, quint64(files.size()));
    put64(toc, 0x18, quint64(tocSize));
    put64(toc, 0x20, quint64(pool.size()));
    toc += packageRecords + dirRecords + fileRecords + children + pool;
    QByteArray sb(0x80, '\0');
    std::memcpy(sb.data(), "NUP\x01", 4);
    put16(sb, 4, 0x80);
    put16(sb, 6, 1);
    put64(sb, 8, 0x80);
    put64(sb, 0x10, quint64(toc.size()));
    sb.replace(0x18, 32, QCryptographicHash::hash(toc, QCryptographicHash::Sha256));
    put64(sb, 0x38, quint64(blobAt));
    put32(sb, 0x48, quint32(packages.size()));
    QByteArray out = sb + toc;
    out += QByteArray(qsizetype(blobAt) - out.size(), '\0');
    out += blob;
    put64(out, 0x40, quint64(out.size()));
    return out;
}

inline QByteArray ncz(const QByteArray &file, int blockExponent)
{
    const QByteArray header = crypto::xtsDecrypt(kHeaderKey, file.left(0xC00), 0, 0x200);
    const QByteArray key = header.mid(0x230, 16) != QByteArray(16, '\0') ? kTitleKey : kProgramKey;
    const QByteArray plain = decryptedNca(file);
    QByteArray sections;
    qint64 count = 0;
    for (int i = 0; i < 4; ++i) {
        const qint64 start = qint64(qFromLittleEndian<quint32>(header.constData() + 0x240 + 0x10 * i)) * 0x200;
        const qint64 end = qint64(qFromLittleEndian<quint32>(header.constData() + 0x244 + 0x10 * i)) * 0x200;
        if (end <= start)
            continue;
        QByteArray entry(0x40, '\0');
        put64(entry, 0, quint64(start));
        put64(entry, 8, quint64(end - start));
        put64(entry, 16, 3);
        entry.replace(32, 16, key);
        for (int j = 0; j < 8; ++j)
            entry[48 + j] = header[0x400 + 0x200 * i + 0x140 + 7 - j];
        sections += entry;
        ++count;
    }
    QByteArray head(16, '\0');
    std::memcpy(head.data(), "NCZSECTN", 8);
    put64(head, 8, quint64(count));
    QByteArray out = file.left(0x4000) + head + sections;
    const QByteArray body = plain.mid(0x4000);
    if (blockExponent == 0)
        return out + zstdFrame(body);
    const qint64 block = qint64(1) << blockExponent;
    const qint64 blocks = (body.size() + block - 1) / block;
    QByteArray blockHeader(24, '\0');
    std::memcpy(blockHeader.data(), "NCZBLOCK", 8);
    blockHeader[8] = 2;
    blockHeader[9] = 1;
    blockHeader[11] = char(blockExponent);
    put32(blockHeader, 12, quint32(blocks));
    put64(blockHeader, 16, quint64(body.size()));
    QByteArray sizes, data;
    for (qint64 b = 0; b < blocks; ++b) {
        const QByteArray raw = body.mid(b * block, block);
        const QByteArray packed = zstdFrame(raw);
        const QByteArray &stored = packed.size() < raw.size() ? packed : raw;
        QByteArray size(4, '\0');
        put32(size, 0, quint32(stored.size()));
        sizes += size;
        data += stored;
    }
    return out + blockHeader + sizes + data;
}

inline std::vector<std::pair<QByteArray, QByteArray>> nszFiles(const std::vector<std::pair<QByteArray, QByteArray>> &files,
                                                               int blockExponent)
{
    std::vector<std::pair<QByteArray, QByteArray>> out;
    for (const auto &[name, data] : files) {
        if (name.endsWith(".nca") && !name.endsWith(".cnmt.nca") && data.size() > 0x4000)
            out.push_back({name.chopped(1) + 'z', ncz(data, blockExponent)});
        else
            out.push_back({name, data});
    }
    return out;
}

}

