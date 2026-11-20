#pragma once

#include "core/format.h"

class RageStringTableReader : public FormatReader
{
public:
    static constexpr quint32 kType = 1;
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE string table"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};

class RageLanguageTableReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE multi-language string table"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
