#pragma once

#include "core/format.h"

#include <optional>

class TextReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{64} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Text"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override
    {
        return fromBytes(data, fileName, error);
    }

    static std::unique_ptr<Document> fromBytes(const QByteArray &data, const QString &fileName, QString *error,
                                               std::optional<TextEncoding> encoding = std::nullopt,
                                               bool requireText = true);
};
