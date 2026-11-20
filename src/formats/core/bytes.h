#pragma once

#include <QByteArrayView>
#include <QtEndian>

inline quint32 le32(QByteArrayView data, qsizetype at)
{
    return qFromLittleEndian<quint32>(data.data() + at);
}

inline quint32 be32(QByteArrayView data, qsizetype at)
{
    return qFromBigEndian<quint32>(data.data() + at);
}
