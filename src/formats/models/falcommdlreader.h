#pragma once

#include "core/format.h"

#include <QStringList>

#include <optional>

class FalcomMdlReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Falcom model (MDL)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override
    {
        return fromBytes(data, error, fileName);
    }

    static std::unique_ptr<Document> fromBytes(const QByteArray &data, QString *error, const QString &path = {});

    static QStringList animatedNodeNames(const QByteArray &data);
    static std::optional<ModelAnimation> readAnimation(const QByteArray &data, const Model3D &model,
                                                       const QString &name, QString *error);
};
