#pragma once

#include "core/format.h"

class JsonReader : public FormatReader
{
public:
    QString name() const override { return QStringLiteral("JSON"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
