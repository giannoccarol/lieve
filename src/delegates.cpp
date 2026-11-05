#include "delegates.h"

#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QPoint>

using namespace Lieve;

void FileItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget); // background, selection, icon

    const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -4, 0);
    const bool selected = opt.state & QStyle::State_Selected;
    const QPalette::ColorGroup group = (opt.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
    const QColor fg = opt.palette.color(group, selected && !Lieve::Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
    QColor dim = fg;
    dim.setAlphaF(0.55f);

    const QFontMetrics fm(opt.font);
    const int baseline = r.top() + (r.height() - fm.height()) / 2 + fm.ascent();

    p->save();
    p->setFont(opt.font);

    if (index.data(KindRole).toInt() == FileItem) {
        QRect textRect = r;
        const int count = index.data(CountRole).toInt();
        if (count > 0) {
            const QString badge = QString::number(count);
            const int bw = fm.horizontalAdvance(badge) + 12;
            const QRect badgeRect(r.right() - bw, r.top() + (r.height() - fm.height()) / 2, bw, fm.height());
            QColor bg = fg;
            bg.setAlphaF(0.14f);
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(badgeRect, badgeRect.height() / 2.0, badgeRect.height() / 2.0);
            p->setPen(fg);
            p->drawText(badgeRect, Qt::AlignCenter, badge);
            textRect.setRight(badgeRect.left() - 6);
        }
        p->setClipRect(textRect);
        p->setPen(fg);
        const QString name = fm.elidedText(text, Qt::ElideRight, textRect.width());
        p->drawText(textRect.left(), baseline, name);
        const int x = textRect.left() + fm.horizontalAdvance(name) + 8;
        const QString dir = index.data(DirRole).toString();
        if (!dir.isEmpty() && x < textRect.right()) {
            p->setPen(dim);
            p->drawText(x, baseline, fm.elidedText(dir, Qt::ElideMiddle, textRect.right() - x));
        }
    } else {
        p->setClipRect(r);
        QColor highlight(234, 92, 0, selected ? 140 : 90);
        int x = r.left();
        int pos = 0;
        const auto drawSegment = [&](const QString &segment, bool match) {
            if (segment.isEmpty()) {
                return;
            }
            const int w = fm.horizontalAdvance(segment);
            if (match) {
                p->fillRect(QRect(x, r.top() + 1, w, r.height() - 2), highlight);
            }
            p->setPen(fg);
            p->drawText(x, baseline, segment);
            x += w;
        };
        const QVariantList ranges = index.data(RangesRole).toList();
        for (const QVariant &v : ranges) {
            const QPoint range = v.toPoint();
            const int start = qBound(0, range.x(), int(text.size()));
            const int end = qBound(start, range.x() + range.y(), int(text.size()));
            if (start < pos) {
                continue;
            }
            drawSegment(text.mid(pos, start - pos), false);
            drawSegment(text.mid(start, end - start), true);
            pos = end;
            if (x > r.right()) {
                break;
            }
        }
        if (pos < text.size() && x <= r.right()) {
            drawSegment(text.mid(pos), false);
        }
    }
    p->restore();
}

QSize FileItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), option.fontMetrics.height() + 6));
    return size;
}
