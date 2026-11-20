#include "imageview.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

ImageView::ImageView(QWidget *parent) : QWidget(parent)
{
    m_checkerboard = QPixmap(16, 16);
    m_checkerboard.fill(QColor(0xCC, 0xCC, 0xCC));
    QPainter p(&m_checkerboard);
    p.fillRect(0, 0, 8, 8, QColor(0x99, 0x99, 0x99));
    p.fillRect(8, 8, 8, 8, QColor(0x99, 0x99, 0x99));
    setMinimumHeight(80);
}

void ImageView::setImage(const QImage &image)
{
    m_image = image;
    m_scaled = {};
    m_placeholder.clear();
    m_reduced = std::max(image.width(), image.height()) > kReducedSide
                  ? image.scaled(kReducedSide, kReducedSide, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                  : QImage();
    setToolTip(image.isNull() ? QString() : tr("%1 × %2 pixels").arg(image.width()).arg(image.height()));
    update();
}

void ImageView::setPlaceholder(const QString &text)
{
    m_image = {};
    m_reduced = {};
    m_scaled = {};
    m_placeholder = text;
    setToolTip({});
    update();
}

QRect ImageView::imageRect() const
{
    if (m_image.isNull())
        return {};
    const QRect area = rect().adjusted(8, 8, -8, -8);
    double scale = std::min(double(area.width()) / m_image.width(), double(area.height()) / m_image.height());
    if (scale >= 1)
        scale = std::min(std::floor(scale), 8.0);
    const QSize size(std::max(1, int(m_image.width() * scale)), std::max(1, int(m_image.height() * scale)));
    return QRect(area.topLeft() + QPoint((area.width() - size.width()) / 2, (area.height() - size.height()) / 2), size);
}

void ImageView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    if (m_image.isNull()) {
        if (!m_placeholder.isEmpty()) {
            p.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
            p.drawText(rect(), Qt::AlignCenter, m_placeholder);
        }
        return;
    }
    const QRect target = imageRect();
    p.drawTiledPixmap(target, m_checkerboard);

    const qreal ratio = devicePixelRatioF();
    const QSize pixels = (QSizeF(target.size()) * ratio).toSize();
    if (m_scaled.isNull() || m_scaled.size() != pixels) {
        const Qt::TransformationMode mode = pixels.width() < m_image.width() ? Qt::SmoothTransformation
                                                                              : Qt::FastTransformation;
        const bool useReduced = !m_reduced.isNull() && pixels.width() <= m_reduced.width()
                             && pixels.height() <= m_reduced.height();
        const QImage &source = useReduced ? m_reduced : m_image;
        m_scaled = QPixmap::fromImage(source.scaled(pixels, Qt::IgnoreAspectRatio, mode));
        m_scaled.setDevicePixelRatio(ratio);
    }
    p.drawPixmap(target.topLeft(), m_scaled);
}
