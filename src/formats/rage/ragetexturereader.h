#pragma once

#include "core/format.h"

class RageTextureDictionaryReader : public FormatReader
{
public:
    static constexpr quint32 kType = 10;
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE texture dictionary"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
