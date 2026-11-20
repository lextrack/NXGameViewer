#include "videoview.h"

#include <QAudioOutput>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QSlider>
#include <QStyle>
#include <QTemporaryFile>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace {

QString clock(qint64 ms)
{
    return QStringLiteral("%1:%2").arg(ms / 60000).arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'));
}

}

VideoView::VideoView(QWidget *parent) : QWidget(parent)
{
    m_video = new QVideoWidget;
    m_video->setMinimumHeight(120);
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);

    m_play = new QToolButton;
    m_play->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_play->setToolTip(tr("Play or pause the video"));
    m_position = new QSlider(Qt::Horizontal);
    m_position->setToolTip(tr("Drag to move through the video"));
    m_time = new QLabel(clock(0) + QStringLiteral(" / ") + clock(0));
    m_message = new QLabel;
    m_message->setWordWrap(true);
    m_message->hide();

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_play);
    controls->addWidget(m_position, 1);
    controls->addWidget(m_time);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_video, 1);
    layout->addWidget(m_message);
    layout->addLayout(controls);

    connect(m_play, &QToolButton::clicked, this, &VideoView::togglePlaying);
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
        m_position->setRange(0, int(ms));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (!m_position->isSliderDown())
            m_position->setValue(int(ms));
        updateTime();
    });
    connect(m_position, &QSlider::sliderMoved, m_player, [this](int ms) { m_player->setPosition(ms); });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        m_play->setIcon(style()->standardIcon(state == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause
                                                                                  : QStyle::SP_MediaPlay));
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, &VideoView::showError);
}

VideoView::~VideoView()
{
    m_player->stop();
}

void VideoView::playFile(const QString &path)
{
    clear();
    m_player->setSource(QUrl::fromLocalFile(path));
}

void VideoView::playData(const QByteArray &bytes, const QString &suffix)
{
    clear();
    m_temporary = std::make_unique<QTemporaryFile>(QDir::temp().filePath(QStringLiteral("nxgv-XXXXXX.") + suffix));
    if (!m_temporary->open() || m_temporary->write(bytes) != bytes.size()) {
        m_message->setText(tr("The video could not be prepared for playing: %1").arg(m_temporary->errorString()));
        m_message->show();
        return;
    }
    m_temporary->flush();
    m_player->setSource(QUrl::fromLocalFile(m_temporary->fileName()));
}

void VideoView::clear()
{
    m_player->stop();
    m_player->setSource({});
    m_temporary.reset();
    m_message->hide();
    m_position->setRange(0, 0);
    updateTime();
}

void VideoView::togglePlaying()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void VideoView::updateTime()
{
    m_time->setText(clock(m_player->position()) + QStringLiteral(" / ") + clock(m_player->duration()));
}

void VideoView::showError()
{
    m_message->setText(tr("This video cannot be played here: %1. Extract it and open it in a video player.")
                           .arg(m_player->errorString()));
    m_message->show();
}
