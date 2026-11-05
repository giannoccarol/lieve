#include "delegates.h"

#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QPoint>

using namespace Lieve;

// Text area and count badge of a row (opt already initialised).
void FileItemDelegate::layout(const QStyleOptionViewItem &opt, const QModelIndex &index, QRect *text, QRect *badge) const
{
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    *text = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -4, 0);
    *badge = QRect();
    const int count = index.data(KindRole).toInt() == FileItem ? index.data(CountRole).toInt() : 0;
    if (count > 0) {
        const QFontMetrics fm(opt.font);
        const int bw = fm.horizontalAdvance(QString::number(count)) + 12;
        *badge = QRect(text->right() - bw, text->top() + (text->height() - fm.height()) / 2, bw, fm.height());
    }
}

QRect FileItemDelegate::buttonsRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int width = buttonsWidth(index);
    if (width <= 0) {
        return {};
    }
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();
    QRect text;
    QRect badge;
    layout(opt, index, &text, &badge);
    const int right = badge.isNull() ? text.right() : badge.left() - 6;
    return QRect(right - width + 1, option.rect.top(), width, option.rect.height());
}

void FileItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget); // background, selection, icon

    QRect r;
    QRect badgeRect;
    layout(opt, index, &r, &badgeRect);
    const int buttons = (opt.state & QStyle::State_MouseOver) ? buttonsWidth(index) : 0;
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
        if (!badgeRect.isNull()) {
            const QString badge = QString::number(index.data(CountRole).toInt());
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
        if (buttons > 0) {
            textRect.setRight(textRect.right() - buttons - 4);
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
        if (buttons > 0) {
            r.setRight(r.right() - buttons - 4);
        }
        p->setClipRect(r);
        const QColor highlight(234, 92, 0, selected ? 140 : 90);
        // A replace preview uses the hues of the diff tabs.
        const QColor removed(248, 81, 73, selected ? 130 : 90);
        const QColor inserted(46, 160, 67, selected ? 130 : 90);
        QFont struck = opt.font;
        struck.setStrikeOut(true);
        int x = r.left();
        int pos = 0;
        const auto drawSegment = [&](const QString &segment, const QColor &background, const QFont &font) {
            if (segment.isEmpty()) {
                return;
            }
            const int w = fm.horizontalAdvance(segment);
            if (background.isValid()) {
                p->fillRect(QRect(x, r.top() + 1, w, r.height() - 2), background);
            }
            p->setFont(font);
            p->setPen(fg);
            p->drawText(x, baseline, segment);
            x += w;
        };
        const QVariantList ranges = index.data(RangesRole).toList();
        const QStringList replace = replacements(index);
        const bool preview = !replace.isEmpty() && replace.size() == ranges.size();
        for (qsizetype i = 0; i < ranges.size(); ++i) {
            const QPoint range = ranges.at(i).toPoint();
            const int start = qBound(0, range.x(), int(text.size()));
            const int end = qBound(start, range.x() + range.y(), int(text.size()));
            if (start < pos) {
                continue;
            }
            drawSegment(text.mid(pos, start - pos), QColor(), opt.font);
            if (preview) {
                drawSegment(text.mid(start, end - start), removed, struck);
                drawSegment(replace.at(i), inserted, opt.font);
            } else {
                drawSegment(text.mid(start, end - start), highlight, opt.font);
            }
            pos = end;
            if (x > r.right()) {
                break;
            }
        }
        if (pos < text.size() && x <= r.right()) {
            drawSegment(text.mid(pos), QColor(), opt.font);
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
