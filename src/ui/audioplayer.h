#pragma once

#include "formats/core/format.h"

#include <QObject>

#include <optional>

class QAudioSink;
class QBuffer;

class AudioPlayer : public QObject
{
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);

    const Audio *audioFor(const Table &table, int row = -1);
    bool start(const Audio &audio);
    void stop();
    void forget();

signals:
    void playingChanged(bool playing);
    void decodeFailed(const QString &error);

private:
    void release();

    std::optional<Audio> m_audio;
    const Table *m_table = nullptr;
    int m_row = -1;
    QAudioSink *m_sink = nullptr;
    QBuffer *m_buffer = nullptr;
};
