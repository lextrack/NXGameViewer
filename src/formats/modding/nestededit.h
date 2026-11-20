#pragma once

#include "archives/compress.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

struct RebuiltLayer
{
    QString name;
    QStringList compression;
    qsizetype oldUnpacked = 0;
    qsizetype newUnpacked = 0;
    qsizetype oldStored = 0;
    qsizetype newStored = 0;
};

using LeafEdit = std::function<std::optional<QByteArray>(const QByteArray &leaf, QString *error)>;

std::optional<QByteArray> editNested(const QByteArray &file, const QString &fileName, const QStringList &entries,
                                     const LeafEdit &edit, std::vector<RebuiltLayer> *rebuilt, QString *error);
