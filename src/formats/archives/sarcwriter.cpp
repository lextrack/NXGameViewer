#include "archives/sarcwriter.h"

#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <numeric>

namespace {

constexpr qsizetype kHeaderSize = 0x14;
constexpr qsizetype kSfatHeaderSize = 0x0C;
constexpr qsizetype kSfntHeaderSize = 0x08;
constexpr qsizetype kNodeSize = 16;
constexpr qint64 kMaxOffset = 0xFFFFFFFFll;

class Endian
{
public:
    explicit Endian(bool little) : m_little(little) {}

    quint32 u16(const char *p) const
    {
        return m_little ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    }
    quint32 u32(const char *p) const
    {
        return m_little ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    }
    void put16(char *p, quint32 v) const
    {
        m_little ? qToLittleEndian(quint16(v), p) : qToBigEndian(quint16(v), p);
    }
    void put32(char *p, quint32 v) const
    {
        m_little ? qToLittleEndian(v, p) : qToBigEndian(v, p);
    }

private:
    bool m_little;
};

qsizetype alignUp(qsizetype value, qsizetype alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

bool isPowerOfTwo(qsizetype v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

qsizetype inferredAlignment(qsizetype offset)
{
    if (offset <= 0)
        return kSarcMaxInferredAlignment;
    return std::min(offset & -offset, kSarcMaxInferredAlignment);
}

std::optional<SarcIndex> parse(const QByteArray &data, QString *error)
{
    std::optional<SarcIndex> index = parseSarcIndex(data, error);
    if (!index)
        return std::nullopt;
    for (size_t i = 0; i < index->nodes.size(); ++i) {
        const SarcNode &node = index->nodes[i];
        QString problem;
        if (!node.inside)
            problem = QObject::tr("SARC entry %1 points outside the file.").arg(i);
        else if (node.badName)
            problem = QObject::tr("The name of SARC entry %1 is outside the name table.").arg(i);
        if (!problem.isEmpty()) {
            if (error)
                *error = problem;
            return std::nullopt;
        }
    }
    return index;
}

}

QString sarcHashName(quint32 hash)
{
    return QStringLiteral("%1.bin").arg(hash, 8, 16, QLatin1Char('0'));
}

QString sarcNodeName(const SarcNode &node)
{
    return node.name.isEmpty() ? sarcHashName(node.hash) : node.name;
}

std::optional<SarcIndex> parseSarcIndex(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<SarcIndex> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const qsizetype size = data.size();
    if (size < kHeaderSize || !data.startsWith("SARC"))
        return fail(QObject::tr("Not a SARC archive (no \"SARC\" at the start)."));
    const char *raw = data.constData();
    const bool little = uchar(raw[6]) == 0xFF && uchar(raw[7]) == 0xFE;
    if (!little && !(uchar(raw[6]) == 0xFE && uchar(raw[7]) == 0xFF))
        return fail(QObject::tr("The SARC byte order mark is not valid."));
    const Endian e(little);

    SarcIndex p;
    p.layout.littleEndian = little;
    p.layout.version = quint16(e.u16(raw + 0x10));
    const qsizetype sfat = e.u16(raw + 4);
    p.dataOffset = e.u32(raw + 0x0C);
    if (sfat < kHeaderSize || sfat + kSfatHeaderSize > size || data.mid(sfat, 4) != "SFAT")
        return fail(QObject::tr("The SARC file table is missing."));
    const qsizetype count = e.u16(raw + sfat + 6);
    p.layout.hashMultiplier = e.u32(raw + sfat + 8);
    p.nodesOffset = sfat + e.u16(raw + sfat + 4);
    const qsizetype sfnt = p.nodesOffset + count * kNodeSize;
    if (p.nodesOffset < sfat + kSfatHeaderSize || sfnt + kSfntHeaderSize > size || data.mid(sfnt, 4) != "SFNT")
        return fail(QObject::tr("The SARC name table is missing."));
    const qsizetype names = sfnt + e.u16(raw + sfnt + 4);
    if (names < sfnt + kSfntHeaderSize || p.dataOffset < names || p.dataOffset > size)
        return fail(QObject::tr("The SARC data offset is not valid."));

    p.nodes.reserve(size_t(count));
    for (qsizetype i = 0; i < count; ++i) {
        const char *n = raw + p.nodesOffset + i * kNodeSize;
        SarcNode node;
        node.hash = e.u32(n);
        node.attributes = e.u32(n + 4);
        node.start = p.dataOffset + qsizetype(e.u32(n + 8));
        node.end = p.dataOffset + qsizetype(e.u32(n + 12));
        node.inside = node.end >= node.start && node.end <= size;
        if (node.attributes >> 24) {
            const qsizetype at = names + qsizetype(node.attributes & 0xFFFFFF) * 4;
            const qsizetype stop = at < p.dataOffset ? data.indexOf('\0', at) : -1;
            if (stop >= 0 && stop <= p.dataOffset)
                node.name = QString::fromUtf8(raw + at, stop - at);
            else
                node.badName = true;
        }
        p.nodes.push_back(std::move(node));
    }
    return p;
}

quint32 sarcNameHash(QByteArrayView name, quint32 multiplier)
{
    quint32 hash = 0;
    for (char c : name)
        hash = hash * multiplier + quint32(qint32(static_cast<signed char>(c)));
    return hash;
}

QString sarcEntryName(const SarcEntry &entry)
{
    return entry.name.isEmpty() ? sarcHashName(entry.hash) : entry.name;
}

std::optional<QByteArray> writeSarc(const std::vector<SarcEntry> &entries, const SarcLayout &layout, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<QByteArray> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (entries.size() > 0xFFFF)
        return fail(QObject::tr("A SARC archive can hold at most 65535 files."));

    struct Item
    {
        const SarcEntry *entry;
        QByteArray utf8;
        quint32 hash;
        qsizetype alignment;
    };
    std::vector<Item> items;
    items.reserve(entries.size());
    for (const SarcEntry &entry : entries) {
        const qsizetype alignment = entry.alignment > 0 ? entry.alignment : kSarcDefaultAlignment;
        if (!isPowerOfTwo(alignment))
            return fail(QObject::tr("The alignment of %1 is not a power of two.").arg(sarcEntryName(entry)));
        QByteArray utf8 = entry.name.toUtf8();
        if (utf8.contains('\0'))
            return fail(QObject::tr("The name %1 contains a zero byte.").arg(entry.name));
        const quint32 hash = utf8.isEmpty() ? entry.hash : sarcNameHash(utf8, layout.hashMultiplier);
        items.push_back({&entry, std::move(utf8), hash, alignment});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.hash < b.hash; });

    const Endian e(layout.littleEndian);
    QByteArray nameTable;
    std::vector<quint32> attributes(items.size(), 0);
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].utf8.isEmpty())
            continue;
        quint32 collision = 1;
        for (size_t k = i; k > 0 && items[k - 1].hash == items[i].hash; --k)
            if (!items[k - 1].utf8.isEmpty())
                ++collision;
        if (collision > 0xFF)
            return fail(QObject::tr("Too many file names share the hash %1.").arg(items[i].hash, 8, 16, QLatin1Char('0')));
        if (nameTable.size() / 4 > 0xFFFFFF)
            return fail(QObject::tr("The SARC name table is too large."));
        attributes[i] = (collision << 24) | quint32(nameTable.size() / 4);
        nameTable.append(items[i].utf8);
        nameTable.append('\0');
        nameTable.append(QByteArray(alignUp(nameTable.size(), 4) - nameTable.size(), '\0'));
    }

    const qsizetype nodes = kHeaderSize + kSfatHeaderSize;
    const qsizetype sfnt = nodes + qsizetype(items.size()) * kNodeSize;
    const qsizetype namesEnd = sfnt + kSfntHeaderSize + nameTable.size();
    const qsizetype maxAlignment =
        std::accumulate(items.begin(), items.end(), qsizetype(1), [](qsizetype m, const Item &it) { return std::max(m, it.alignment); });
    const qsizetype dataOffset = alignUp(namesEnd, maxAlignment);

    std::vector<std::pair<qsizetype, qsizetype>> ranges(items.size());
    qsizetype cursor = dataOffset;
    for (size_t i = 0; i < items.size(); ++i) {
        const qsizetype start = alignUp(cursor, items[i].alignment);
        cursor = start + items[i].entry->data.size();
        ranges[i] = {start - dataOffset, cursor - dataOffset};
        if (cursor > kMaxOffset)
            return fail(QObject::tr("The archive would be larger than 4 GB."));
    }

    try {
        QByteArray out(cursor, '\0');
        char *raw = out.data();
        std::memcpy(raw, "SARC", 4);
        e.put16(raw + 4, kHeaderSize);
        e.put16(raw + 6, 0xFEFF);
        e.put32(raw + 8, quint32(cursor));
        e.put32(raw + 0x0C, quint32(dataOffset));
        e.put16(raw + 0x10, layout.version);
        std::memcpy(raw + kHeaderSize, "SFAT", 4);
        e.put16(raw + kHeaderSize + 4, kSfatHeaderSize);
        e.put16(raw + kHeaderSize + 6, quint32(items.size()));
        e.put32(raw + kHeaderSize + 8, layout.hashMultiplier);
        for (size_t i = 0; i < items.size(); ++i) {
            char *n = raw + nodes + qsizetype(i) * kNodeSize;
            e.put32(n, items[i].hash);
            e.put32(n + 4, attributes[i]);
            e.put32(n + 8, quint32(ranges[i].first));
            e.put32(n + 12, quint32(ranges[i].second));
            const QByteArray &d = items[i].entry->data;
            if (!d.isEmpty())
                std::memcpy(raw + dataOffset + ranges[i].first, d.constData(), size_t(d.size()));
        }
        std::memcpy(raw + sfnt, "SFNT", 4);
        e.put16(raw + sfnt + 4, kSfntHeaderSize);
        if (!nameTable.isEmpty())
            std::memcpy(raw + sfnt + kSfntHeaderSize, nameTable.constData(), size_t(nameTable.size()));
        return out;
    } catch (const std::bad_alloc &) {
        return fail(QObject::tr("Not enough memory to build the archive."));
    }
}

std::optional<SarcContents> readSarc(const QByteArray &data, QString *error)
{
    const std::optional<SarcIndex> p = parse(data, error);
    if (!p)
        return std::nullopt;
    SarcContents contents;
    contents.layout = p->layout;
    contents.entries.reserve(p->nodes.size());
    for (const SarcNode &node : p->nodes)
        contents.entries.push_back({node.name, data.mid(node.start, node.end - node.start), inferredAlignment(node.start),
                                    node.hash});
    return contents;
}

std::optional<QByteArray> replaceSarcEntry(const QByteArray &sarc, const QString &name, const QByteArray &data,
                                           QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<QByteArray> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const std::optional<SarcIndex> p = parse(sarc, error);
    if (!p)
        return std::nullopt;

    qsizetype target = -1;
    for (size_t i = 0; i < p->nodes.size() && target < 0; ++i)
        if (sarcNodeName(p->nodes[i]) == name)
            target = qsizetype(i);
    if (target < 0)
        return fail(QObject::tr("%1 is not in this archive.").arg(name));
    const std::pair<qsizetype, qsizetype> replaced{p->nodes[size_t(target)].start, p->nodes[size_t(target)].end};

    std::map<std::pair<qsizetype, qsizetype>, std::pair<qsizetype, qsizetype>> moved;
    for (const SarcNode &node : p->nodes)
        moved.emplace(std::pair{node.start, node.end}, std::pair<qsizetype, qsizetype>{0, 0});
    qsizetype previousEnd = p->dataOffset;
    for (const auto &[range, unused] : moved) {
        if (range.first < previousEnd && range.first != range.second)
            return fail(QObject::tr("Files in this archive overlap, so it cannot be rebuilt."));
        previousEnd = std::max(previousEnd, range.second);
    }
    const qsizetype tailStart = previousEnd;

    qsizetype cursor = p->dataOffset;
    for (auto &[range, place] : moved) {
        const qsizetype length = range == replaced ? data.size() : range.second - range.first;
        const qsizetype start = alignUp(cursor, inferredAlignment(range.first));
        place = {start, start + length};
        cursor = start + length;
    }
    const qsizetype total = cursor + (sarc.size() - tailStart);
    if (total > kMaxOffset)
        return fail(QObject::tr("The archive would be larger than 4 GB."));

    try {
        QByteArray out(total, '\0');
        std::memcpy(out.data(), sarc.constData(), size_t(p->dataOffset));
        const Endian e(p->layout.littleEndian);
        e.put32(out.data() + 8, quint32(total));
        for (const auto &[range, place] : moved) {
            const char *source = range == replaced ? data.constData() : sarc.constData() + range.first;
            if (place.second > place.first)
                std::memcpy(out.data() + place.first, source, size_t(place.second - place.first));
        }
        if (sarc.size() > tailStart)
            std::memcpy(out.data() + cursor, sarc.constData() + tailStart, size_t(sarc.size() - tailStart));
        for (size_t i = 0; i < p->nodes.size(); ++i) {
            const auto &place = moved.at({p->nodes[i].start, p->nodes[i].end});
            char *n = out.data() + p->nodesOffset + qsizetype(i) * kNodeSize;
            e.put32(n + 8, quint32(place.first - p->dataOffset));
            e.put32(n + 12, quint32(place.second - p->dataOffset));
        }
        return out;
    } catch (const std::bad_alloc &) {
        return fail(QObject::tr("Not enough memory to rebuild the archive."));
    }
}
