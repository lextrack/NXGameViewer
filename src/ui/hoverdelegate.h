#pragma once

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolTip>

class HoverRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setHoverRow(int row) { m_hoverRow = row; }
    int hoverRow() const { return m_hoverRow; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.row() == m_hoverRow && !(option.state & QStyle::State_Selected)) {
            QColor tint = option.palette.color(QPalette::Highlight);
            tint.setAlpha(45);
            painter->fillRect(option.rect, tint);
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override
    {
        if (!event || !view || event->type() != QEvent::ToolTip || !index.isValid())
            return QStyledItemDelegate::helpEvent(event, view, option, index);
        const QString own = index.data(Qt::ToolTipRole).toString();
        const QString full = isCutOff(option, index) ? index.data(Qt::DisplayRole).toString() : QString();
        QString tip;
        if (!full.isEmpty())
            tip = full.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"));
        if (!own.isEmpty())
            tip += (tip.isEmpty() ? QString() : QStringLiteral("<hr>")) + own.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"));
        if (tip.isEmpty()) {
            QToolTip::hideText();
            return true;
        }
        QToolTip::showText(event->globalPos(), QStringLiteral("<qt>%1</qt>").arg(tip), view->viewport(),
                           view->visualRect(index));
        return true;
    }

private:
    bool isCutOff(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        if (opt.text.contains(u'\n'))
            return true;
        const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, opt.widget) + 1;
        return opt.fontMetrics.horizontalAdvance(opt.text) > textRect.width() - 2 * margin;
    }

    int m_hoverRow = -1;
};
