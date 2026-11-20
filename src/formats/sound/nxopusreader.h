#pragma once

#include "core/format.h"

class NxOpusReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Falcom sound (nxopus)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override
    {
        Q_UNUSED(fileName);
        return fromBytes(data, error);
    }

    static std::unique_ptr<Document> fromBytes(const QByteArray &data, QString *error);
};

class NintendoOpusReader : public FormatReader
{
public:
    QString name() const override { return QStringLiteral("Nintendo Opus sound"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override
    {
        Q_UNUSED(fileName);
        return fromBytes(data, error);
    }

    static std::unique_ptr<Document> fromBytes(const QByteArray &data, QString *error);
};
