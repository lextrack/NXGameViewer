#pragma once

#include "core/format.h"

class CriUtfReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{1024} * 1024 * 1024;

    QString name() const override { return QStringLiteral("CRI @UTF table"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};

class Afs2Reader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{2} * 1024 * 1024 * 1024;

    QString name() const override { return QStringLiteral("CRI AFS2 archive"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
