#pragma once

#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <stdexcept>

inline quint16 le16(const uchar *p)
{
    return qFromLittleEndian<quint16>(p);
}

inline quint32 le32(const uchar *p)
{
    return qFromLittleEndian<quint32>(p);
}

inline quint16 le16(QByteArrayView data, qsizetype at)
{
    return qFromLittleEndian<quint16>(data.data() + at);
}

inline quint32 le32(QByteArrayView data, qsizetype at)
{
    return qFromLittleEndian<quint32>(data.data() + at);
}

inline quint64 le64(QByteArrayView data, qsizetype at)
{
    return qFromLittleEndian<quint64>(data.data() + at);
}

inline quint16 be16(QByteArrayView data, qsizetype at)
{
    return qFromBigEndian<quint16>(data.data() + at);
}

inline quint32 be32(QByteArrayView data, qsizetype at)
{
    return qFromBigEndian<quint32>(data.data() + at);
}

struct ParseFailure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void parseFailure(const QString &message)
{
    throw ParseFailure(message.toStdString());
}

class ByteReader
{
public:
    explicit ByteReader(const QByteArray &data, bool little = true) : m_data(data), m_little(little) {}

    const QByteArray &data() const { return m_data; }
    qint64 size() const { return m_data.size(); }
    bool little() const { return m_little; }

    void need(qint64 at, qint64 size) const
    {
        if (at < 0 || size < 0 || at > m_data.size() - size)
            parseFailure(QObject::tr("An offset points outside the file."));
    }
    quint8 u8(qint64 at) const
    {
        need(at, 1);
        return quint8(m_data[at]);
    }
    quint16 u16(qint64 at) const
    {
        need(at, 2);
        return m_little ? qFromLittleEndian<quint16>(m_data.constData() + at) : qFromBigEndian<quint16>(m_data.constData() + at);
    }
    qint16 s16(qint64 at) const { return qint16(u16(at)); }
    quint32 u32(qint64 at) const
    {
        need(at, 4);
        return m_little ? qFromLittleEndian<quint32>(m_data.constData() + at) : qFromBigEndian<quint32>(m_data.constData() + at);
    }
    qint32 s32(qint64 at) const { return qint32(u32(at)); }
    quint64 u64(qint64 at) const
    {
        need(at, 8);
        return m_little ? qFromLittleEndian<quint64>(m_data.constData() + at) : qFromBigEndian<quint64>(m_data.constData() + at);
    }
    float f32(qint64 at) const
    {
        const quint32 bits = u32(at);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    qint64 ptr(qint64 at) const
    {
        const quint64 v = u64(at);
        if (v > quint64(m_data.size()))
            parseFailure(QObject::tr("A pointer points outside the file."));
        return qint64(v);
    }
    int count(quint32 n, int max) const
    {
        if (n > quint32(max))
            parseFailure(QObject::tr("The file lists an impossible number of items."));
        return int(n);
    }
    QString cstr(qint64 at) const
    {
        need(at, 1);
        const char *p = m_data.constData() + at;
        return QString::fromUtf8(p, qsizetype(qstrnlen(p, uint(std::min<qint64>(m_data.size() - at, 0xFFFFFFFF)))));
    }
    void setPool(qint64 at) { m_pool = at; }
    QString str(quint32 offset) const { return cstr(m_pool + offset); }
    QString guid(qint64 at) const
    {
        need(at, 16);
        return QStringLiteral("%1-%2-%3-%4-%5")
            .arg(u32(at), 8, 16, QLatin1Char('0'))
            .arg(u16(at + 4), 4, 16, QLatin1Char('0'))
            .arg(u16(at + 6), 4, 16, QLatin1Char('0'))
            .arg(u16(at + 8), 4, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(m_data.mid(at + 10, 6).toHex()));
    }

private:
    const QByteArray &m_data;
    bool m_little;
    qint64 m_pool = 0;
};

class ByteCursor
{
public:
    explicit ByteCursor(QByteArrayView data, bool bigEndian = false) : m_data(data), m_big(bigEndian) {}

    qsizetype pos() const { return m_pos; }
    qsizetype size() const { return m_data.size(); }
    void setBigEndian(bool big) { m_big = big; }
    void seek(qsizetype pos)
    {
        if (pos < 0 || pos > m_data.size())
            throw ParseFailure("seek");
        m_pos = pos;
    }
    void align(qsizetype to = 4) { seek(std::min<qsizetype>((m_pos + to - 1) / to * to, m_data.size())); }
    QByteArrayView take(qsizetype n)
    {
        if (n < 0 || n > m_data.size() - m_pos)
            throw ParseFailure("short");
        const QByteArrayView out = m_data.sliced(m_pos, n);
        m_pos += n;
        return out;
    }
    template <typename T> T read()
    {
        const char *p = take(sizeof(T)).data();
        return m_big ? qFromBigEndian<T>(p) : qFromLittleEndian<T>(p);
    }
    template <typename T> T be() { return qFromBigEndian<T>(take(sizeof(T)).data()); }
    float readFloat()
    {
        const quint32 bits = read<quint32>();
        float out;
        std::memcpy(&out, &bits, 4);
        return out;
    }
    double readDouble()
    {
        const quint64 bits = read<quint64>();
        double out;
        std::memcpy(&out, &bits, 8);
        return out;
    }
    QString cstring()
    {
        const qsizetype end = m_data.indexOf('\0', m_pos);
        if (end < 0)
            throw ParseFailure("string");
        const QString out = QString::fromUtf8(m_data.sliced(m_pos, end - m_pos));
        m_pos = end + 1;
        return out;
    }

private:
    QByteArrayView m_data;
    bool m_big;
    qsizetype m_pos = 0;
};
