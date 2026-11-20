#include "contenticons.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>

#include <map>

namespace {

void drawModel(QPainter &p, qreal s)
{
    const QColor color(0x7e, 0x57, 0xc2);
    const QPointF top(s * 0.5, s * 0.08), right(s * 0.9, s * 0.3), bottomRight(s * 0.9, s * 0.72),
        bottom(s * 0.5, s * 0.94), bottomLeft(s * 0.1, s * 0.72), left(s * 0.1, s * 0.3), center(s * 0.5, s * 0.52);
    QPainterPath topFace;
    topFace.moveTo(top);
    topFace.lineTo(right);
    topFace.lineTo(center);
    topFace.lineTo(left);
    topFace.closeSubpath();
    QPainterPath sideLeft;
    sideLeft.moveTo(left);
    sideLeft.lineTo(center);
    sideLeft.lineTo(bottom);
    sideLeft.lineTo(bottomLeft);
    sideLeft.closeSubpath();
    QPainterPath sideRight;
    sideRight.moveTo(center);
    sideRight.lineTo(right);
    sideRight.lineTo(bottomRight);
    sideRight.lineTo(bottom);
    sideRight.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color.lighter(150));
    p.drawPath(topFace);
    p.setBrush(color);
    p.drawPath(sideLeft);
    p.setBrush(color.darker(130));
    p.drawPath(sideRight);
}

void drawPicture(QPainter &p, qreal s, const QColor &color)
{
    const QRectF frame(s * 0.08, s * 0.16, s * 0.84, s * 0.68);
    p.setPen(QPen(color.darker(120), s * 0.07));
    p.setBrush(QColor(0xe8, 0xf5, 0xe9));
    p.drawRoundedRect(frame, s * 0.08, s * 0.08);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xff, 0xb3, 0x00));
    p.drawEllipse(QPointF(s * 0.68, s * 0.36), s * 0.08, s * 0.08);
    QPainterPath hills;
    hills.moveTo(s * 0.14, s * 0.78);
    hills.lineTo(s * 0.38, s * 0.42);
    hills.lineTo(s * 0.56, s * 0.66);
    hills.lineTo(s * 0.68, s * 0.54);
    hills.lineTo(s * 0.86, s * 0.78);
    hills.closeSubpath();
    p.setBrush(color);
    p.drawPath(hills);
}

void drawSound(QPainter &p, qreal s)
{
    const QColor color(0xfb, 0x8c, 0x00);
    QPainterPath speaker;
    speaker.moveTo(s * 0.1, s * 0.38);
    speaker.lineTo(s * 0.28, s * 0.38);
    speaker.lineTo(s * 0.5, s * 0.16);
    speaker.lineTo(s * 0.5, s * 0.84);
    speaker.lineTo(s * 0.28, s * 0.62);
    speaker.lineTo(s * 0.1, s * 0.62);
    speaker.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(speaker);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, s * 0.08, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(s * 0.38, s * 0.3, s * 0.36, s * 0.4), -50 * 16, 100 * 16);
    p.drawArc(QRectF(s * 0.34, s * 0.14, s * 0.56, s * 0.72), -50 * 16, 100 * 16);
}

void drawDocument(QPainter &p, qreal s, const QColor &color, bool grid)
{
    const QRectF page(s * 0.16, s * 0.06, s * 0.68, s * 0.88);
    p.setPen(QPen(color.darker(120), s * 0.06));
    p.setBrush(QColor(0xff, 0xff, 0xff));
    p.drawRoundedRect(page, s * 0.06, s * 0.06);
    p.setPen(QPen(color, s * 0.07, Qt::SolidLine, Qt::RoundCap));
    if (grid) {
        for (int i = 1; i <= 3; ++i)
            p.drawLine(QPointF(page.left() + s * 0.1, page.top() + page.height() * i / 4),
                       QPointF(page.right() - s * 0.1, page.top() + page.height() * i / 4));
        p.drawLine(QPointF(page.center().x(), page.top() + s * 0.12), QPointF(page.center().x(), page.bottom() - s * 0.12));
    } else {
        for (int i = 1; i <= 4; ++i) {
            const qreal y = page.top() + page.height() * i / 5;
            p.drawLine(QPointF(page.left() + s * 0.12, y), QPointF(page.right() - (i == 4 ? s * 0.26 : s * 0.12), y));
        }
    }
}

QPixmap draw(ContentCategory category, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    switch (category) {
    case ContentCategory::Model: drawModel(p, s); break;
    case ContentCategory::Texture: drawPicture(p, s, QColor(0x43, 0xa0, 0x47)); break;
    case ContentCategory::Image: drawPicture(p, s, QColor(0x00, 0x89, 0x7b)); break;
    case ContentCategory::Sound: drawSound(p, s); break;
    case ContentCategory::Text: drawDocument(p, s, QColor(0x1e, 0x88, 0xe5), false); break;
    case ContentCategory::Data: drawDocument(p, s, QColor(0x00, 0x83, 0x8f), true); break;
    default: break;
    }
    return pixmap;
}

}

QIcon contentIcon(ContentCategory category)
{
    static std::map<ContentCategory, QIcon> cache;
    auto it = cache.find(category);
    if (it != cache.end())
        return it->second;
    QIcon icon;
    switch (category) {
    case ContentCategory::Archive:
    case ContentCategory::Compressed:
        icon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
        break;
    case ContentCategory::Other:
        icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
        break;
    default:
        for (int size : {16, 24, 32, 48})
            icon.addPixmap(draw(category, size));
        break;
    }
    return cache.emplace(category, icon).first->second;
}
