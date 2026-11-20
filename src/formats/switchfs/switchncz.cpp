#include "switchfs/switchncz.h"

#include "core/crypto.h"

#include <QMutex>
#include <QObject>
#include <QtEndian>

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <memory>
#include <vector>

namespace {

constexpr qint64 kSectionEntrySize = 0x40;
constexpr qint64 kMaxSections = 0x100;
constexpr qint64 kMaxBlocks = qint64{1} << 24;
constexpr qint64 kMaxBodySize = qint64{1} << 40;
constexpr qint64 kStreamWindow = qint64{16} * 1024 * 1024;
constexpr int kCachedBlocks = 4;

struct NczSection
{
    qint64 offset = 0;
    qint64 size = 0;
    qint64 cryptoType = 0;
    QByteArray key;
    QByteArray counter;
};

qint64 i64(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<qint64>(b.constData() + at);
}

qint32 i32(const QByteArray &b, qsizetype at)
{
    return qFromLittleEndian<qint32>(b.constData() + at);
}

using DStream = std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)>;

class ZstdFrameStorage : public SwitchStorage
{
public:
    ZstdFrameStorage(StoragePtr packed, qint64 size) : m_packed(std::move(packed)), m_size(size) {}

    qint64 size() const override { return m_size; }
    bool seeksFast() const override { return false; }

    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > m_size - length)
            return false;
        if (length == 0)
            return true;
        const QMutexLocker lock(&m_mutex);
        return readStream(offset, out, length);
    }

private:
    bool restartStream() const
    {
        if (!m_stream)
            m_stream = DStream(ZSTD_createDStream(), &ZSTD_freeDStream);
        if (!m_stream || ZSTD_isError(ZSTD_DCtx_reset(m_stream.get(), ZSTD_reset_session_only)))
            return false;
        m_inputAt = 0;
        m_input.clear();
        m_inputPos = 0;
        m_outputAt = 0;
        m_window.clear();
        m_windowStart = 0;
        return true;
    }

    static void copyOverlap(qint64 chunkStart, const char *chunk, qint64 chunkLength, qint64 offset, char *out,
                     qint64 length)
    {
        const qint64 from = std::max(chunkStart, offset), to = std::min(chunkStart + chunkLength, offset + length);
        if (from < to)
            std::memcpy(out + (from - offset), chunk + (from - chunkStart), size_t(to - from));
    }

    bool readStream(qint64 offset, char *out, qint64 length) const
    {
        const qint64 end = offset + length;
        if (offset < m_windowStart || !m_stream) {
            if (!restartStream())
                return false;
        }
        copyOverlap(m_windowStart, m_window.constData(), m_window.size(), offset, out, length);
        if (m_outputAt >= end)
            return true;
        const qint64 packedEnd = m_packed->size();
        QByteArray chunk(qsizetype(ZSTD_DStreamOutSize()), Qt::Uninitialized);
        const qint64 inputStep = qint64(ZSTD_DStreamInSize());
        while (m_outputAt < end) {
            if (m_inputPos >= m_input.size()) {
                const qint64 step = std::min(inputStep, packedEnd - m_inputAt);
                if (step <= 0)
                    return false;
                m_input = m_packed->read(m_inputAt, step);
                if (m_input.size() != step)
                    return false;
                m_inputAt += step;
                m_inputPos = 0;
            }
            ZSTD_inBuffer in{m_input.constData(), size_t(m_input.size()), size_t(m_inputPos)};
            ZSTD_outBuffer o{chunk.data(), size_t(chunk.size()), 0};
            const size_t result = ZSTD_decompressStream(m_stream.get(), &o, &in);
            if (ZSTD_isError(result))
                return false;
            m_inputPos = qsizetype(in.pos);
            const qint64 produced = qint64(o.pos);
            if (produced == 0)
                continue;
            if (m_outputAt + produced > m_size)
                return false;
            copyOverlap(m_outputAt, chunk.constData(), produced, offset, out, length);
            m_window.append(chunk.constData(), qsizetype(produced));
            m_outputAt += produced;
            if (m_window.size() > 2 * kStreamWindow) {
                const qsizetype drop = qsizetype(m_window.size() - kStreamWindow);
                m_window.remove(0, drop);
                m_windowStart += drop;
            }
        }
        return true;
    }

    StoragePtr m_packed;
    qint64 m_size;
    mutable QMutex m_mutex;
    mutable DStream m_stream{nullptr, &ZSTD_freeDStream};
    mutable QByteArray m_input;
    mutable qsizetype m_inputPos = 0;
    mutable qint64 m_inputAt = 0;
    mutable qint64 m_outputAt = 0;
    mutable QByteArray m_window;
    mutable qint64 m_windowStart = 0;
};

}

StoragePtr zstdFrameStorage(const StoragePtr &packed, qint64 size)
{
    if (!packed || size < 0)
        return nullptr;
    return std::make_shared<ZstdFrameStorage>(packed, size);
}

namespace {

class NczStorage : public SwitchStorage
{
public:
    NczStorage(StoragePtr file, std::vector<NczSection> sections, qint64 dataStart, qint64 bodySize)
        : m_file(std::move(file)), m_sections(std::move(sections)), m_dataStart(dataStart), m_bodySize(bodySize)
    {
        m_stream = zstdFrameStorage(subStorage(m_file, m_dataStart, m_file->size() - m_dataStart), m_bodySize);
    }

    void useBlocks(int exponent, std::vector<qint64> compressedSizes)
    {
        m_blockSize = qint64(1) << exponent;
        m_blockSizes = std::move(compressedSizes);
        m_blockOffsets.reserve(m_blockSizes.size());
        qint64 at = m_dataStart;
        for (qint64 size : m_blockSizes) {
            m_blockOffsets.push_back(at);
            at += size;
        }
    }

    qint64 size() const override { return kNczHeaderSize + m_bodySize; }
    bool decryptedSections() const override { return true; }
    bool seeksFast() const override { return !m_blockSizes.empty(); }

    bool read(qint64 offset, char *out, qint64 length) const override
    {
        if (offset < 0 || length < 0 || offset > size() - length)
            return false;
        if (offset < kNczHeaderSize) {
            const qint64 head = std::min(length, kNczHeaderSize - offset);
            if (!readHeaderArea(offset, out, head))
                return false;
            offset += head;
            out += head;
            length -= head;
        }
        if (length == 0)
            return true;
        if (m_blockSizes.empty())
            return m_stream->read(offset - kNczHeaderSize, out, length);
        const QMutexLocker lock(&m_mutex);
        return readBlocks(offset - kNczHeaderSize, out, length);
    }

private:
    bool readHeaderArea(qint64 offset, char *out, qint64 length) const
    {
        if (!m_file->read(offset, out, length))
            return false;
        for (const NczSection &s : m_sections) {
            if (s.cryptoType != 3 && s.cryptoType != 4)
                continue;
            const qint64 from = std::max(offset, s.offset), to = std::min(offset + length, s.offset + s.size);
            if (from >= to)
                continue;
            const qint64 aligned = from & ~qint64(15);
            const qint64 span = ((to + 15) & ~qint64(15)) - aligned;
            if (aligned + span > kNczHeaderSize)
                return false;
            QByteArray buffer(span, Qt::Uninitialized);
            if (!m_file->read(aligned, buffer.data(), span)
                || !crypto::ctrApply(s.key, crypto::ctrCounter(s.counter.left(8), quint64(aligned)), buffer.data(), span))
                return false;
            std::memcpy(out + (from - offset), buffer.constData() + (from - aligned), size_t(to - from));
        }
        return true;
    }

    qint64 blockLength(size_t index) const
    {
        const qint64 start = qint64(index) * m_blockSize;
        return std::min(m_blockSize, m_bodySize - start);
    }

    const QByteArray *block(size_t index) const
    {
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
            if (it->first == index) {
                m_cache.splice(m_cache.begin(), m_cache, it);
                return &m_cache.front().second;
            }
        const qint64 expected = blockLength(index);
        const qint64 stored = m_blockSizes[index];
        QByteArray data;
        if (stored >= expected) {
            data = m_file->read(m_blockOffsets[index], expected);
            if (data.size() != expected)
                return nullptr;
        } else {
            const QByteArray packed = m_file->read(m_blockOffsets[index], stored);
            if (packed.size() != stored)
                return nullptr;
            data = QByteArray(expected, Qt::Uninitialized);
            const size_t written = ZSTD_decompress(data.data(), size_t(expected), packed.constData(), size_t(stored));
            if (ZSTD_isError(written) || qint64(written) != expected)
                return nullptr;
        }
        m_cache.emplace_front(index, std::move(data));
        if (m_cache.size() > size_t(kCachedBlocks))
            m_cache.pop_back();
        return &m_cache.front().second;
    }

    bool readBlocks(qint64 offset, char *out, qint64 length) const
    {
        while (length > 0) {
            const size_t index = size_t(offset / m_blockSize);
            if (index >= m_blockSizes.size())
                return false;
            const QByteArray *data = block(index);
            if (!data)
                return false;
            const qint64 inside = offset - qint64(index) * m_blockSize;
            const qint64 step = std::min(length, qint64(data->size()) - inside);
            if (step <= 0)
                return false;
            std::memcpy(out, data->constData() + inside, size_t(step));
            out += step;
            offset += step;
            length -= step;
        }
        return true;
    }

    StoragePtr m_file;
    std::vector<NczSection> m_sections;
    qint64 m_dataStart;
    qint64 m_bodySize;
    qint64 m_blockSize = 0;
    std::vector<qint64> m_blockSizes;
    std::vector<qint64> m_blockOffsets;

    mutable QMutex m_mutex;
    mutable std::list<std::pair<size_t, QByteArray>> m_cache;
    StoragePtr m_stream;
};

}

StoragePtr nczStorage(const StoragePtr &file, QString *error)
{
    auto fail = [error](const QString &message) -> StoragePtr {
        if (error)
            *error = message;
        return nullptr;
    };
    if (!file || file->size() < kNczHeaderSize + 16)
        return fail(QObject::tr("The NCZ file is cut off."));
    const QByteArray head = file->read(kNczHeaderSize, 16);
    if (head.size() != 16 || !head.startsWith("NCZSECTN"))
        return fail(QObject::tr("The NCZ file has no section table."));
    const qint64 count = i64(head, 8);
    if (count <= 0 || count > kMaxSections)
        return fail(QObject::tr("The NCZ section table is damaged."));
    const qint64 tableAt = kNczHeaderSize + 16;
    const QByteArray table = file->read(tableAt, count * kSectionEntrySize);
    if (table.size() != count * kSectionEntrySize)
        return fail(QObject::tr("The NCZ section table is cut off."));
    std::vector<NczSection> sections;
    qint64 sectionsEnd = kNczHeaderSize;
    for (qint64 i = 0; i < count; ++i) {
        const qsizetype at = qsizetype(i * kSectionEntrySize);
        NczSection s;
        s.offset = i64(table, at);
        s.size = i64(table, at + 8);
        s.cryptoType = i64(table, at + 16);
        s.key = table.mid(at + 32, 16);
        s.counter = table.mid(at + 48, 16);
        if (s.offset < 0 || s.size < 0 || s.size > kMaxBodySize || s.offset > kMaxBodySize)
            return fail(QObject::tr("The NCZ section table is damaged."));
        sectionsEnd = std::max(sectionsEnd, s.offset + s.size);
        sections.push_back(std::move(s));
    }

    qint64 dataStart = tableAt + count * kSectionEntrySize;
    const QByteArray blockHead = file->read(dataStart, 24);
    if (blockHead.size() == 24 && blockHead.startsWith("NCZBLOCK")) {
        const int exponent = uchar(blockHead[11]);
        const qint64 blocks = i32(blockHead, 12);
        const qint64 bodySize = i64(blockHead, 16);
        if (exponent < 14 || exponent > 32 || blocks <= 0 || blocks > kMaxBlocks || bodySize <= 0
            || bodySize > kMaxBodySize || (bodySize + (qint64(1) << exponent) - 1) >> exponent != blocks)
            return fail(QObject::tr("The NCZ block table is damaged."));
        const QByteArray sizes = file->read(dataStart + 24, blocks * 4);
        if (sizes.size() != blocks * 4)
            return fail(QObject::tr("The NCZ block table is cut off."));
        dataStart += 24 + blocks * 4;
        std::vector<qint64> compressed;
        compressed.reserve(size_t(blocks));
        qint64 total = 0;
        for (qint64 i = 0; i < blocks; ++i) {
            const qint64 size = i32(sizes, qsizetype(i * 4));
            if (size <= 0)
                return fail(QObject::tr("The NCZ block table is damaged."));
            compressed.push_back(size);
            total += size;
        }
        if (total > file->size() - dataStart)
            return fail(QObject::tr("The NCZ file is cut off."));
        auto storage = std::make_shared<NczStorage>(file, std::move(sections), dataStart, bodySize);
        storage->useBlocks(exponent, std::move(compressed));
        return storage;
    }

    const QByteArray frame = file->read(dataStart, std::min<qint64>(file->size() - dataStart, 32));
    const unsigned long long declared = ZSTD_getFrameContentSize(frame.constData(), size_t(frame.size()));
    qint64 bodySize = sectionsEnd - kNczHeaderSize;
    if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != ZSTD_CONTENTSIZE_ERROR && declared <= quint64(kMaxBodySize))
        bodySize = std::max(bodySize, qint64(declared));
    if (bodySize <= 0)
        return fail(QObject::tr("The NCZ file is empty."));
    return std::make_shared<NczStorage>(file, std::move(sections), dataStart, bodySize);
}
