#include "core/zipwriter.h"

#include <QDateTime>
#include <QIODevice>
#include <QObject>
#include <QtEndian>

#include <zlib.h>

#include <vector>

namespace {

constexpr qint64 kMaxZip32 = 0xFFFFFFFFll;
constexpr int kMaxEntries = 0xFFFF;
constexpr qint64 kChunk = qint64{1} << 20;
constexpr quint16 kUtf8Flag = 0x0800;
constexpr quint16 kDeflate = 8;
constexpr quint16 kVersion = 20;

void put16(QByteArray &b, quint16 v)
{
    char raw[2];
    qToLittleEndian(v, raw);
    b.append(raw, 2);
}

void put32(QByteArray &b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

struct DosTime
{
    quint16 time = 0;
    quint16 date = 0;
};

DosTime dosTime(const QDateTime &when)
{
    const QDate d = when.date();
    const QTime t = when.time();
    if (d.year() < 1980)
        return {0, quint16((1 << 5) | 1)};
    return {quint16((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2)),
            quint16(((d.year() - 1980) << 9) | (d.month() << 5) | d.day())};
}

struct Entry
{
    QByteArray name;
    quint32 crc = 0;
    qint64 compressed = 0;
    qint64 size = 0;
    qint64 offset = 0;
    DosTime when;
};

}

struct ZipWriter::State
{
    QIODevice *out = nullptr;
    std::vector<Entry> entries;
    QString error;
    bool failed = false;
    bool finished = false;
    DosTime now = dosTime(QDateTime::currentDateTime());

    bool fail(const QString &message)
    {
        if (!failed)
            error = message;
        failed = true;
        return false;
    }

    bool write(const QByteArray &bytes)
    {
        if (out->write(bytes) != bytes.size())
            return fail(QObject::tr("Cannot write the zip file: %1").arg(out->errorString()));
        return true;
    }

    QByteArray localHeader(const Entry &e) const
    {
        QByteArray h;
        put32(h, 0x04034b50);
        put16(h, kVersion);
        put16(h, kUtf8Flag);
        put16(h, kDeflate);
        put16(h, e.when.time);
        put16(h, e.when.date);
        put32(h, e.crc);
        put32(h, quint32(e.compressed));
        put32(h, quint32(e.size));
        put16(h, quint16(e.name.size()));
        put16(h, 0);
        return h + e.name;
    }
};

ZipWriter::ZipWriter(QIODevice *out) : m(std::make_unique<State>())
{
    m->out = out;
}

ZipWriter::~ZipWriter() = default;

QString ZipWriter::error() const
{
    return m->error;
}

bool ZipWriter::addFile(const QString &name, QIODevice *source, const std::function<bool()> &keepGoing)
{
    if (m->failed || m->finished)
        return false;
    if (int(m->entries.size()) >= kMaxEntries)
        return m->fail(QObject::tr("A zip file can hold at most %1 files.").arg(kMaxEntries));
    if (!m->out->isWritable() || m->out->isSequential())
        return m->fail(QObject::tr("The zip file cannot be written."));
    Entry e;
    e.name = name.toUtf8();
    e.when = m->now;
    e.offset = m->out->pos();
    if (e.name.isEmpty() || e.name.size() > 0xFFFF)
        return m->fail(QObject::tr("\"%1\" is not a valid name for a file in a zip.").arg(name));
    if (e.offset > kMaxZip32)
        return m->fail(QObject::tr("The zip file would be larger than 4 GB."));
    if (!m->write(m->localHeader(e)))
        return false;

    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return m->fail(QObject::tr("Not enough memory to compress the zip file."));
    struct Guard
    {
        z_stream *z;
        ~Guard() { deflateEnd(z); }
    } guard{&z};
    uLong crc = crc32(0, nullptr, 0);
    QByteArray outBuffer(qsizetype(kChunk), Qt::Uninitialized);
    for (bool done = false; !done;) {
        if (keepGoing && !keepGoing())
            return m->fail(QObject::tr("Canceled."));
        QByteArray input = source->read(kChunk);
        if (input.isEmpty() && !source->atEnd())
            return m->fail(QObject::tr("Cannot read %1.").arg(name));
        done = source->atEnd();
        e.size += input.size();
        if (e.size > kMaxZip32)
            return m->fail(QObject::tr("%1 is larger than 4 GB, which a zip file cannot hold.").arg(name));
        crc = crc32(crc, reinterpret_cast<const Bytef *>(input.constData()), uInt(input.size()));
        z.next_in = reinterpret_cast<Bytef *>(input.data());
        z.avail_in = uInt(input.size());
        int result = Z_OK;
        do {
            z.next_out = reinterpret_cast<Bytef *>(outBuffer.data());
            z.avail_out = uInt(outBuffer.size());
            result = deflate(&z, done ? Z_FINISH : Z_NO_FLUSH);
            if (result == Z_STREAM_ERROR)
                return m->fail(QObject::tr("Cannot compress %1.").arg(name));
            const qsizetype produced = outBuffer.size() - qsizetype(z.avail_out);
            e.compressed += produced;
            if (!m->write(outBuffer.left(produced)))
                return false;
        } while (z.avail_out == 0 || (done && result != Z_STREAM_END));
    }
    e.crc = quint32(crc);
    if (e.compressed > kMaxZip32 || m->out->pos() > kMaxZip32)
        return m->fail(QObject::tr("The zip file would be larger than 4 GB."));
    const qint64 end = m->out->pos();
    if (!m->out->seek(e.offset) || !m->write(m->localHeader(e)) || !m->out->seek(end))
        return m->fail(QObject::tr("Cannot write the zip file: %1").arg(m->out->errorString()));
    m->entries.push_back(std::move(e));
    return true;
}

bool ZipWriter::finish()
{
    if (m->failed)
        return false;
    if (m->finished)
        return true;
    const qint64 directoryAt = m->out->pos();
    QByteArray directory;
    for (const Entry &e : m->entries) {
        put32(directory, 0x02014b50);
        put16(directory, quint16((3 << 8) | kVersion));
        put16(directory, kVersion);
        put16(directory, kUtf8Flag);
        put16(directory, kDeflate);
        put16(directory, e.when.time);
        put16(directory, e.when.date);
        put32(directory, e.crc);
        put32(directory, quint32(e.compressed));
        put32(directory, quint32(e.size));
        put16(directory, quint16(e.name.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, quint32(0100644) << 16);
        put32(directory, quint32(e.offset));
        directory += e.name;
    }
    if (directoryAt + directory.size() > kMaxZip32)
        return m->fail(QObject::tr("The zip file would be larger than 4 GB."));
    QByteArray end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, quint16(m->entries.size()));
    put16(end, quint16(m->entries.size()));
    put32(end, quint32(directory.size()));
    put32(end, quint32(directoryAt));
    put16(end, 0);
    if (!m->write(directory) || !m->write(end))
        return false;
    m->finished = true;
    return true;
}
