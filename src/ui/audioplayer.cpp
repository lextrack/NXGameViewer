#include "audioplayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {}

const Audio *AudioPlayer::audioFor(const Table &table, int row)
{
    if (m_table != &table || m_row != row) {
        QString error;
        m_audio = row < 0 ? table.decodeAudio(&error) : table.decodeRowAudio(row, &error);
        m_table = &table;
        m_row = row;
        if (!m_audio)
            emit decodeFailed(error);
    }
    return m_audio ? &*m_audio : nullptr;
}

bool AudioPlayer::start(const Audio &audio)
{
    release();
    QAudioFormat format;
    format.setSampleRate(audio.sampleRate);
    format.setChannelCount(audio.channels);
    format.setSampleFormat(QAudioFormat::Int16);

    m_buffer = new QBuffer(this);
    m_buffer->setData(audio.pcm);
    m_buffer->open(QIODevice::ReadOnly);
    m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (!m_sink)
            return;
        const bool finished = state == QAudio::IdleState && m_buffer && m_buffer->atEnd();
        const bool failed = state == QAudio::StoppedState && m_sink->error() != QAudio::NoError;
        if (finished || failed)
            stop();
    });
    m_sink->start(m_buffer);
    if (m_sink->error() != QAudio::NoError) {
        stop();
        return false;
    }
    emit playingChanged(true);
    return true;
}

void AudioPlayer::stop()
{
    release();
    emit playingChanged(false);
}

void AudioPlayer::forget()
{
    stop();
    m_audio.reset();
    m_table = nullptr;
    m_row = -1;
}

void AudioPlayer::release()
{
    if (m_sink) {
        m_sink->disconnect(this);
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    if (m_buffer) {
        m_buffer->deleteLater();
        m_buffer = nullptr;
    }
}
