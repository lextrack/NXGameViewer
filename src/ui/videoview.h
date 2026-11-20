#pragma once

#include <QWidget>

#include <memory>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QSlider;
class QTemporaryFile;
class QToolButton;
class QVideoWidget;

class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);
    ~VideoView() override;

    void playFile(const QString &path);
    void playData(const QByteArray &bytes, const QString &suffix);
    void clear();

private:
    void togglePlaying();
    void updateTime();
    void showError();

    QVideoWidget *m_video = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QToolButton *m_play = nullptr;
    QSlider *m_position = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_message = nullptr;
    std::unique_ptr<QTemporaryFile> m_temporary;
};
