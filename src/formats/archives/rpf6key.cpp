#include "archives/rpf6reader.h"

#include "core/bytes.h"
#include "unity/lz4block.h"

#include <QObject>
#include <QtEndian>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <bitset>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct Segment
{
    qint64 start = 0;
    qint64 size = 0;
};

struct Executable
{
    QByteArray image;
    Segment text;
    std::vector<Segment> searchable;
};

std::optional<Executable> loadExecutable(const QByteArray &file, QString *error)
{
    Executable out;
    if (!file.startsWith("NSO0")) {
        out.image = file;
        out.searchable.push_back({0, file.size()});
        return out;
    }
    if (file.size() < 0x100) {
        if (error)
            *error = QObject::tr("The executable is cut off.");
        return std::nullopt;
    }
    const quint32 flags = le32(file, 0x0C);
    struct Raw { quint32 fileOffset, memoryOffset, size, stored; bool compressed; };
    std::array<Raw, 3> segments{};
    qint64 imageSize = 0;
    for (int s = 0; s < 3; ++s) {
        segments[size_t(s)] = {le32(file, 0x10 + s * 0x10), le32(file, 0x14 + s * 0x10), le32(file, 0x18 + s * 0x10),
                               le32(file, 0x60 + s * 4), bool(flags & (1u << s))};
        imageSize = std::max<qint64>(imageSize, qint64(segments[size_t(s)].memoryOffset) + segments[size_t(s)].size);
    }
    if (imageSize > qint64{1024} * 1024 * 1024) {
        if (error)
            *error = QObject::tr("The executable is damaged.");
        return std::nullopt;
    }
    out.image = QByteArray(qsizetype(imageSize), '\0');
    for (int s = 0; s < 3; ++s) {
        const Raw &r = segments[size_t(s)];
        const qint64 stored = r.compressed ? r.stored : r.size;
        if (qint64(r.fileOffset) + stored > file.size()) {
            if (error)
                *error = QObject::tr("The executable is cut off.");
            return std::nullopt;
        }
        const QByteArrayView packed = QByteArrayView(file).sliced(r.fileOffset, stored);
        QByteArray bytes;
        if (r.compressed) {
            std::optional<QByteArray> unpacked = lz4BlockDecompress(packed, r.size);
            if (!unpacked) {
                if (error)
                    *error = QObject::tr("A segment of the executable could not be unpacked.");
                return std::nullopt;
            }
            bytes = std::move(*unpacked);
        } else {
            bytes = packed.toByteArray();
        }
        std::memcpy(out.image.data() + r.memoryOffset, bytes.constData(), size_t(bytes.size()));
        const Segment segment{r.memoryOffset, r.size};
        if (s == 0)
            out.text = segment;
        else
            out.searchable.push_back(segment);
    }
    return out;
}

qint64 signExtend(qint64 value, int bits)
{
    const qint64 sign = qint64(1) << (bits - 1);
    return (value ^ sign) - sign;
}

std::vector<QByteArray> keysFromSetup(const Executable &exe)
{
    static const quint32 kSetup[5] = {0x7100003f, 0x12800048, 0x1a810108, 0x31000d1f, 0xb9000008};
    QByteArray pattern(20, '\0');
    for (int i = 0; i < 5; ++i)
        qToLittleEndian(kSetup[i], pattern.data() + i * 4);
    const QByteArrayView img(exe.image);
    const qint64 ts = exe.text.start, te = exe.text.start + exe.text.size;
    std::vector<QByteArray> keys;
    for (qsizetype at = img.indexOf(pattern, ts); at >= 0 && at + 20 <= te; at = img.indexOf(pattern, at + 4)) {
        std::map<int, qint64> page, ctx{{0, 0}};
        std::map<int, QByteArray> q;
        std::map<qint64, QByteArray> mem;
        for (qint64 a = at + 24; a + 4 <= std::min<qint64>(at + 0x80, te); a += 4) {
            const quint32 w = le32(img, a);
            const int rd = int(w & 31), rn = int((w >> 5) & 31);
            if ((w & 0x9f000000u) == 0x90000000u) {
                const qint64 imm = qint64((w >> 29) & 3) | (qint64((w >> 5) & 0x7ffff) << 2);
                page[rd] = (a & ~qint64(0xfff)) + (signExtend(imm, 21) << 12);
                ctx.erase(rd);
            } else if ((w & 0xffe0ffe0u) == 0xaa0003e0u && ctx.count(int((w >> 16) & 31))) {
                ctx[rd] = ctx[int((w >> 16) & 31)];
            } else if ((w & 0xffc00000u) == 0x91000000u && ctx.count(rn)) {
                ctx[rd] = ctx[rn] + qint64((w >> 10) & 0xfff);
            } else if ((w & 0xffc00000u) == 0x3dc00000u && page.count(rn)) {
                const qint64 o = page[rn] + qint64((w >> 10) & 0xfff) * 16;
                if (o >= 0 && o + 16 <= img.size())
                    q[rd] = img.sliced(o, 16).toByteArray();
            } else if ((w & 0xffe00c00u) == 0x3c800c00u && ctx.count(rn) && q.count(rd)) {
                ctx[rn] += signExtend((w >> 12) & 0x1ff, 9);
                mem[ctx[rn]] = q[rd];
            } else if ((w & 0xffc00000u) == 0x3d800000u && ctx.count(rn) && q.count(rd)) {
                mem[ctx[rn] + qint64((w >> 10) & 0xfff) * 16] = q[rd];
            } else if ((w & 0xffc00000u) == 0xad000000u && ctx.count(rn) && q.count(rd) && q.count(int((w >> 10) & 31))) {
                const qint64 o = ctx[rn] + signExtend((w >> 15) & 0x7f, 7) * 16;
                mem[o] = q[rd];
                mem[o + 16] = q[int((w >> 10) & 31)];
            } else if ((w & 0xfc000000u) == 0x14000000u || (w & 0xfc000000u) == 0x94000000u
                       || (w & 0xff000010u) == 0x54000000u) {
                break;
            }
        }
        QByteArray words;
        for (qint64 o = 8; o < 0x48; o += 16) {
            const auto it = mem.find(o);
            if (it == mem.end())
                break;
            words += it->second;
        }
        if (words.size() != 64)
            continue;
        QByteArray key(32, '\0');
        bool valid = true;
        for (int i = 0; i < 8 && valid; ++i) {
            const quint64 v = qFromLittleEndian<quint64>(words.constData() + i * 8);
            valid = (v >> 32) == 0;
            qToBigEndian(quint32(v), key.data() + i * 4);
        }
        if (valid)
            keys.push_back(key);
    }
    return keys;
}

}

std::optional<QByteArray> findRpf6Key(const QByteArray &executable, QByteArrayView firstEntry, QString *error,
                                      const std::function<bool(int percent)> &progress)
{
    if (firstEntry.size() < 16) {
        if (error)
            *error = QObject::tr("The archive is too short to check a key against.");
        return std::nullopt;
    }
    const std::optional<Executable> exe = loadExecutable(executable, error);
    if (!exe)
        return std::nullopt;
    const QByteArrayView block = firstEntry.first(16);
    for (const QByteArray &key : keysFromSetup(*exe))
        if (rpf6IsRootEntry(rpf6Decrypt(key, block)))
            return key;

    std::vector<std::pair<qint64, qint64>> ranges;
    qint64 total = 0;
    for (const Segment &s : exe->searchable) {
        const qint64 windows = s.size >= 32 ? (s.size - 32) / 4 + 1 : 0;
        ranges.push_back({total, s.start});
        total += windows;
    }
    auto addressOf = [&ranges](qint64 index) {
        size_t r = ranges.size() - 1;
        while (r > 0 && ranges[r].first > index)
            --r;
        return ranges[r].second + (index - ranges[r].first) * 4;
    };
    std::atomic<qint64> next{0}, done{0};
    std::atomic<bool> stop{false};
    std::mutex resultMutex;
    std::optional<QByteArray> found;
    const QByteArray &img = exe->image;
    auto work = [&] {
        constexpr qint64 kChunk = 4096;
        while (!stop.load()) {
            const qint64 from = next.fetch_add(kChunk);
            if (from >= total)
                return;
            const qint64 to = std::min(total, from + kChunk);
            for (qint64 i = from; i < to && !stop.load(); ++i) {
                const char *w = img.constData() + addressOf(i);
                std::bitset<256> distinct;
                for (int k = 0; k < 32; ++k)
                    distinct.set(uchar(w[k]));
                if (distinct.count() < 24)
                    continue;
                const QByteArray key(w, 32);
                if (rpf6IsRootEntry(rpf6Decrypt(key, block))) {
                    const std::lock_guard lock(resultMutex);
                    if (!found)
                        found = key;
                    stop = true;
                }
            }
            done.fetch_add(to - from);
        }
    };
    {
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::jthread> pool;
        pool.reserve(threads);
        try {
            for (unsigned t = 0; t < threads; ++t)
                pool.emplace_back(work);
        } catch (const std::system_error &) {
            if (pool.empty())
                work();
        }
        while (!stop.load() && done.load() < total) {
            if (progress && !progress(total ? int(done.load() * 100 / total) : 100))
                stop = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!found && error)
        *error = QObject::tr("No key in this executable opens the archive. Is it the \"main\" file of the same game?");
    return found;
}
