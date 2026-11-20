#pragma once

#include "core/format.h"

class ImageFileReader : public FormatReader
{
public:
    static constexpr int kMaxFrames = 64;

    QString name() const override { return QStringLiteral("Image (PNG, JPEG, …)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
};
