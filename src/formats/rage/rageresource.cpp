#include "rage/rageresource.h"

#include <QObject>
#include <QtEndian>

bool isRageResource(QByteArrayView head)
{
    return head.size() >= 12
           && (head.startsWith(QByteArrayView("RSC\x05", 4)) || head.startsWith(QByteArrayView("RSC\x85", 4)));
}

std::optional<RageResource> readRageResource(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<RageResource> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!isRageResource(data))
        return fail(QObject::tr("Not a RAGE resource (no \"RSC\" header)."));
    const bool extended = uchar(data[3]) == 0x85;
    if (extended && data.size() < 16)
        return fail(QObject::tr("The RAGE resource header is cut off."));
    RageResource r;
    r.type = qFromLittleEndian<quint32>(data.constData() + 4);
    r.flags = qFromLittleEndian<quint32>(data.constData() + 8);
    if (extended && (r.flags & 0x80000000u)) {
        r.extendedFlags = qFromLittleEndian<quint32>(data.constData() + 12);
        r.virtualSize = qint64(r.extendedFlags & 0x3FFF) * 4096;
        r.physicalSize = qint64((r.extendedFlags >> 14) & 0x3FFF) * 4096;
        const qint64 startPage = qint64(4096) << ((r.extendedFlags >> 28) & 7);
        const int counts[4] = {int((r.flags >> 14) & 3), int((r.flags >> 8) & 63), int(r.flags & 0xFF), 1 << 30};
        qint64 left = r.virtualSize, page = 512 * 1024, at = 0;
        bool found = false;
        for (int group = 0; group < 4 && left > 0 && !found; ++group, page >>= 1)
            for (int k = 0; k < counts[group] && left > 0; ++k) {
                while (page > left)
                    page >>= 1;
                if (page == startPage) {
                    r.objectStart = at;
                    found = true;
                    break;
                }
                at += page;
                left -= page;
            }
    } else {
        r.virtualSize = qint64(r.flags & 0x7FF) << (((r.flags >> 11) & 0xF) + 8);
        r.physicalSize = qint64((r.flags >> 15) & 0x7FF) << (((r.flags >> 26) & 0xF) + 8);
    }
    r.body = data.mid(extended ? 16 : 12);
    if (r.body.size() < r.virtualSize + r.physicalSize)
        return fail(QObject::tr("The RAGE resource is shorter than its header says (it may still be compressed)."));
    return r;
}

std::optional<qint64> RageResource::offsetOf(quint32 pointer) const
{
    const qint64 offset = pointer & 0x0FFFFFFFu;
    if ((pointer >> 28) == 5 && offset < virtualSize)
        return offset;
    if ((pointer >> 28) == 6 && offset < physicalSize)
        return virtualSize + offset;
    return std::nullopt;
}

std::optional<quint32> RageResource::u32(qint64 offset) const
{
    if (offset < 0 || offset > body.size() - 4)
        return std::nullopt;
    return qFromLittleEndian<quint32>(body.constData() + offset);
}

std::optional<quint16> RageResource::u16(qint64 offset) const
{
    if (offset < 0 || offset > body.size() - 2)
        return std::nullopt;
    return qFromLittleEndian<quint16>(body.constData() + offset);
}

QString RageResource::cstring(quint32 pointer, int maxLength) const
{
    const std::optional<qint64> at = offsetOf(pointer);
    if (!at)
        return {};
    const qint64 end = std::min<qint64>(body.size(), *at + maxLength);
    qint64 stop = *at;
    while (stop < end && body[qsizetype(stop)] != '\0')
        ++stop;
    return QString::fromLatin1(body.constData() + *at, qsizetype(stop - *at));
}
