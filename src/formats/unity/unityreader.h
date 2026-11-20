#pragma once

#include "core/format.h"

class UnityBundleReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{2} * 1024 * 1024 * 1024;

    QString name() const override { return QStringLiteral("Unity asset bundle"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
    bool readsStored() const override { return true; }
    std::unique_ptr<Document> readStored(const StoredReader &read, qint64 size, const QString &fileName,
                                         QString *error) const override;
};

class UnityAssetsReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{2} * 1024 * 1024 * 1024;

    QString name() const override { return QStringLiteral("Unity serialized file"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};

class UnityObjectReader : public FormatReader
{
public:
    static constexpr QByteArrayView kMagic = "NXGVUnityObject";

    QString name() const override { return QStringLiteral("Unity object"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
