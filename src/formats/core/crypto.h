#pragma once

#include <QByteArray>
#include <QByteArrayView>

namespace crypto {

QByteArray ecbDecrypt(QByteArrayView key, QByteArrayView data);
QByteArray ecbEncrypt(QByteArrayView key, QByteArrayView data);
bool ecbApplyInPlace(QByteArrayView key, char *data, qint64 size, int rounds, bool encrypt);
QByteArray xtsDecrypt(QByteArrayView key, QByteArrayView data, quint64 firstSector, int sectorSize);
QByteArray xtsEncrypt(QByteArrayView key, QByteArrayView data, quint64 firstSector, int sectorSize);
bool ctrApply(QByteArrayView key, QByteArrayView counter, char *data, qint64 size);
QByteArray ctrCounter(QByteArrayView upper, quint64 offset);

}
