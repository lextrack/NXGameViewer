#pragma once

#include "core/format.h"

class FontFileReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;
    static constexpr int kPreviewGlyphs = 256;

    QString name() const override { return QStringLiteral("Font (TTF, OTF, BFTTF, BFOTF)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static Image renderPreview(const QByteArray &font, int glyphs);
};
