#pragma once

#include "core/format.h"

class RageModelReader : public FormatReader
{
public:
    static constexpr quint32 kFragmentType = 138;
    static constexpr quint32 kFragmentDrawableType = 1;
    static constexpr quint32 kVisualDictionaryType = 133;
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE model"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
