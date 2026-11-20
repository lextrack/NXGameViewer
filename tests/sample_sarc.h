#pragma once

#include <QByteArray>
#include <QtEndian>

#include <utility>
#include <vector>

namespace samplesarc {

struct File
{
    QByteArray name;
    QByteArray data;
};

inline QByteArray make(const std::vector<File> &files, bool littleEndian = true)
{
    auto u16 = [littleEndian](QByteArray &b, quint16 v) {
        char raw[2];
        littleEndian ? qToLittleEndian(v, raw) : qToBigEndian(v, raw);
        b.append(raw, 2);
    };
    auto u32 = [littleEndian](QByteArray &b, quint32 v) {
        char raw[4];
        littleEndian ? qToLittleEndian(v, raw) : qToBigEndian(v, raw);
        b.append(raw, 4);
    };

    QByteArray names;
    std::vector<quint32> nameOffsets;
    for (const File &f : files) {
        nameOffsets.push_back(quint32(names.size() / 4));
        if (!f.name.isEmpty()) {
            names += f.name;
            names += '\0';
            while (names.size() % 4)
                names += '\0';
        }
    }
    QByteArray data;
    std::vector<std::pair<quint32, quint32>> ranges;
    for (const File &f : files) {
        while (data.size() % 8)
            data += '\0';
        ranges.emplace_back(quint32(data.size()), quint32(data.size() + f.data.size()));
        data += f.data;
    }

    QByteArray sfat("SFAT");
    u16(sfat, 0x0C);
    u16(sfat, quint16(files.size()));
    u32(sfat, 0x65);
    for (size_t i = 0; i < files.size(); ++i) {
        u32(sfat, 0x1000 + quint32(i));
        u32(sfat, files[i].name.isEmpty() ? 0 : (0x01000000u | nameOffsets[i]));
        u32(sfat, ranges[i].first);
        u32(sfat, ranges[i].second);
    }
    QByteArray sfnt("SFNT");
    u16(sfnt, 0x08);
    u16(sfnt, 0);
    sfnt += names;

    QByteArray head("SARC");
    const quint32 dataOffset = quint32(0x14 + sfat.size() + sfnt.size());
    u16(head, 0x14);
    head.append(littleEndian ? "\xFF\xFE" : "\xFE\xFF", 2);
    u32(head, quint32(dataOffset + data.size()));
    u32(head, dataOffset);
    u16(head, 0x0100);
    u16(head, 0);
    return head + sfat + sfnt + data;
}

}
