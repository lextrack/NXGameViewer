#pragma once

#include "core/format.h"

#include <QHash>

class BaevReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{64} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Animation events (BAEV)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static quint32 fnv1a(QByteArrayView text);
    static std::unique_ptr<Document> readWithNames(const QByteArray &data, const QHash<quint32, QString> &names,
                                                   QString *error);
};
