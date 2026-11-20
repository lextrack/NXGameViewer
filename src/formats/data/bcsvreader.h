#pragma once

#include "core/format.h"

#include <QStringList>

#include <optional>

struct HashedName
{
    QString name;
    QString type;
};

class BcsvReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("BCSV table"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static quint32 crc32(QByteArrayView data);
    static void setHashNames(const QStringList &lines);
    static std::optional<HashedName> nameForHash(quint32 hash);
};
