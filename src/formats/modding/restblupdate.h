#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

struct UnpackedSize
{
    QString path;
    qint64 size = 0;
};

struct RestblChange
{
    QString path;
    quint32 oldValue = 0;
    quint32 newValue = 0;
};

struct RestblUpdate
{
    QByteArray file;
    std::vector<RestblChange> raised;
    QStringList unlisted;
};

QString restblKey(const QString &romfsPath);
bool isRestblPath(const QString &romfsPath);
std::optional<std::vector<UnpackedSize>> unpackedSizes(const QByteArray &file, const QString &gamePath,
                                                       const QStringList &entries, QString *error);
std::optional<RestblUpdate> raiseRestbl(const QByteArray &gameTable, const QByteArray &baseTable,
                                        const std::vector<UnpackedSize> &before, const std::vector<UnpackedSize> &after,
                                        QString *error);
QString newestRestblName(const QStringList &fileNames);
