#pragma once

#include "core/format.h"

#include <optional>

struct VideoInfo
{
    QString container;
    double seconds = -1;
    QString videoCodec;
    int width = 0;
    int height = 0;
    double framesPerSecond = 0;
    QString audioCodec;
    double sampleRate = 0;
    int channels = 0;
};

std::optional<VideoInfo> parseMatroska(QByteArrayView head);

class VideoFileReader : public FormatReader
{
public:
    static constexpr qint64 kHeadBytes = qint64{4} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Video (WebM, MKV, MP4)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
