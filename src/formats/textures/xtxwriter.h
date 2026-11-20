#pragma once

#include "core/format.h"

#include <QByteArray>
#include <QString>

#include <optional>

struct XtxReplacement
{
    QByteArray file;
    QString formatName;
    qsizetype reusedBlocks = 0;
    qsizetype encodedBlocks = 0;
    int mipLevels = 1;
};

std::optional<XtxReplacement> replaceXtxImage(const QByteArray &xtx, const Image &image, QString *error);
