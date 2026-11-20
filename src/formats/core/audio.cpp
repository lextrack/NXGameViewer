#include "core/format.h"

#include <QtEndian>

namespace {

void appendU32(QByteArray &b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

void appendU16(QByteArray &b, quint16 v)
{
    char raw[2];
    qToLittleEndian(v, raw);
    b.append(raw, 2);
}

}

QByteArray Audio::toWav() const
{
    const quint16 blockAlign = quint16(channels * 2);
    QByteArray wav;
    wav.reserve(44 + pcm.size());
    wav.append("RIFF");
    appendU32(wav, quint32(36 + pcm.size()));
    wav.append("WAVEfmt ");
    appendU32(wav, 16);
    appendU16(wav, 1);
    appendU16(wav, quint16(channels));
    appendU32(wav, quint32(sampleRate));
    appendU32(wav, quint32(sampleRate) * blockAlign);
    appendU16(wav, blockAlign);
    appendU16(wav, 16);
    wav.append("data");
    appendU32(wav, quint32(pcm.size()));
    wav.append(pcm);
    return wav;
}
