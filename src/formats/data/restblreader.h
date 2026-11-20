#pragma once

#include "core/format.h"

#include <QHash>
#include <QStringList>

#include <optional>
#include <utility>
#include <vector>

struct Restbl
{
    quint32 version = 1;
    quint32 nameSize = 160;
    std::vector<std::pair<quint32, quint32>> hashed;
    std::vector<std::pair<QString, quint32>> named;
};

std::optional<Restbl> parseRestbl(const QByteArray &data, QString *error);
QByteArray writeRestbl(const Restbl &table);
quint32 restblPathHash(const QString &romfsPath);
QHash<quint32, QString> restblNamesUnder(const QString &romfsRoot);

class RestblReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{64} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Resource size table (RESTBL)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
