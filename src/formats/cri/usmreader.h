#pragma once

#include "core/format.h"

class UsmReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{4} * 1024 * 1024 * 1024;

    QString name() const override { return QStringLiteral("CRI USM movie"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
    bool readsStored() const override { return true; }
    std::unique_ptr<Document> readStored(const StoredReader &read, qint64 size, const QString &fileName,
                                         QString *error) const override;
};
