#pragma once

#include "core/format.h"

class HexReader : public FormatReader
{
public:
    static constexpr qint64 kMaxBytes = qint64{16} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Hex"); }
    int score(const QString &, QByteArrayView) const override { return 1; }
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
    bool readsStored() const override { return true; }
    std::unique_ptr<Document> readStored(const StoredReader &read, qint64 size, const QString &fileName,
                                         QString *error) const override;
};
