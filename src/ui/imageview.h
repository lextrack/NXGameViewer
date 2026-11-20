#pragma once

#include <QImage>
#include <QWidget>

class ImageView : public QWidget
{
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    const QImage &image() const { return m_image; }

    void setPlaceholder(const QString &text);

    QRect imageRect() const;

    QSize sizeHint() const override { return {400, 300}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    QString m_placeholder;
    QPixmap m_checkerboard;
    QPixmap m_scaled;
    static constexpr int kReducedSide = 2048;
    QImage m_reduced;
};
