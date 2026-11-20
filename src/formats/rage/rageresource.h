#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstring>
#include <optional>
#include <stdexcept>

#include <QtEndian>

struct RageResource
{
    quint32 type = 0;
    quint32 flags = 0;
    quint32 extendedFlags = 0;
    qint64 virtualSize = 0;
    qint64 physicalSize = 0;
    QByteArray body;
    qint64 objectStart = 0;

    std::optional<qint64> offsetOf(quint32 pointer) const;
    std::optional<quint32> u32(qint64 offset) const;
    std::optional<quint16> u16(qint64 offset) const;
    QString cstring(quint32 pointer, int maxLength = 256) const;
};

bool isRageResource(QByteArrayView head);
std::optional<RageResource> readRageResource(const QByteArray &data, QString *error);

struct RageDamaged : std::runtime_error
{
    RageDamaged() : std::runtime_error("damaged") {}
};

class RageView
{
public:
    explicit RageView(const RageResource &r) : m_r(r) {}

    qint64 at(quint32 pointer) const
    {
        const std::optional<qint64> o = m_r.offsetOf(pointer);
        if (!o)
            throw RageDamaged();
        return *o;
    }
    std::optional<qint64> maybe(quint32 pointer) const { return pointer ? m_r.offsetOf(pointer) : std::nullopt; }
    void need(qint64 offset, qint64 size) const
    {
        if (offset < 0 || size < 0 || offset > m_r.body.size() - size)
            throw RageDamaged();
    }
    const char *bytes(qint64 offset, qint64 size) const
    {
        need(offset, size);
        return m_r.body.constData() + offset;
    }
    quint8 u8(qint64 o) const { return quint8(*bytes(o, 1)); }
    quint16 u16(qint64 o) const { return qFromLittleEndian<quint16>(bytes(o, 2)); }
    quint32 u32(qint64 o) const { return qFromLittleEndian<quint32>(bytes(o, 4)); }
    float f32(qint64 o) const
    {
        const quint32 bits = u32(o);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    QString cstring(quint32 pointer) const { return m_r.cstring(pointer); }
    qint64 objectStart() const { return m_r.objectStart; }
    const RageResource &resource() const { return m_r; }

private:
    const RageResource &m_r;
};
