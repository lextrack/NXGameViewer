#pragma once

#include "core/format.h"

class AudioFileReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;
    static constexpr qsizetype kMaxPcmBytes = qsizetype{1024} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Audio (WAV, MP3, OGG, FLAC, …)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;

    static std::optional<Audio> decodeFile(const QString &path, QString *error);
};
