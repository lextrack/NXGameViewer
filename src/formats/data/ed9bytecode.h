#pragma once

#include <QByteArray>
#include <QString>

namespace ed9 {

int instructionSize(const QByteArray &data, qsizetype pos);

struct Instruction
{
    QString name;
    QString arguments;
};

Instruction decode(const QByteArray &data, qsizetype pos);
QString valueText(const QByteArray &data, quint32 value);

}
