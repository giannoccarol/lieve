#include "delegates.h"

#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QPoint>

#include <algorithm>

using namespace Lieve;

namespace
{
// Draws `text` from x, the ranges in `match` colour and bold (fuzzy
// matches). Returns the end x.
int drawMatched(QPainter *p, const QString &text, const QVariantList &ranges, int x, int baseline, const QColor &fg, const QColor &match)
{
    const QFont normal = p->font();
    QFont bold = normal;
    bold.setBold(true);
    const auto segment = [&](qsizetype from, qsizetype to, bool matched) {
        if (to <= from) {
            return;
        }
        const QString part = text.mid(from, to - from);
        p->setFont(matched ? bold : normal);
        p->setPen(matched ? match : fg);
        p->drawText(x, baseline, part);
        x += QFontMetrics(p->font()).horizontalAdvance(part);
    };
    qsizetype pos = 0;
    for (const QVariant &v : ranges) {
        const QPoint range = v.toPoint();
        const qsizetype start = qBound<qsizetype>(pos, range.x(), text.size());
        const qsizetype end = qBound<qsizetype>(start, range.x() + range.y(), text.size());
        segment(pos, start, false);
        segment(start, end, true);
        pos = end;
    }
    segment(pos, text.size(), false);
    p->setFont(normal);
    return x;
}

// Key caps like VS Code's, right-aligned at `right`: [Ctrl]+[Shift]+[P].
// Returns their left edge.
int drawKeys(QPainter *p, const QString &keys, int right, const QRect &row, const QColor &fg)
{
    QFont font = p->font();
    font.setPointSizeF(font.pointSizeF() * 0.88);
    const QFontMetrics fm(font);
    struct Piece {
        QString text;
        bool cap;
        int width;
    };
    QList<Piece> pieces;
    const QStringList chords = keys.split(QStringLiteral(", "));
    for (const QString &chord : chords) {
        if (!pieces.isEmpty()) {
            pieces.append({QString(), false, 6});
        }
        QStringList parts = chord.split(QLatin1Char('+'));
        if (chord.endsWith(QLatin1String("++"))) { // the plus key itself
            parts = chord.chopped(2).split(QLatin1Char('+'));
            parts << QStringLiteral("+");
        }
        for (const QString &part : std::as_const(parts)) {
            if (part.isEmpty()) {
                continue;
            }
            if (!pieces.isEmpty() && pieces.last().cap) {
                pieces.append({QStringLiteral("+"), false, fm.horizontalAdvance(QLatin1Char('+')) + 4});
            }
            pieces.append({part, true, fm.horizontalAdvance(part) + 10});
        }
    }
    int width = 0;
    for (const Piece &piece : std::as_const(pieces)) {
        width += piece.width;
    }
    const int left = right - width;
    const int height = std::min(fm.height() + 4, row.height() - 4);
    const int top = row.top() + (row.height() - height) / 2;
    QColor capBackground = fg;
    capBackground.setAlphaF(0.09f);
    QColor capBorder = fg;
    capBorder.setAlphaF(0.20f);
    QColor text = fg;
    text.setAlphaF(0.80f);
    p->save();
    p->setFont(font);
    p->setRenderHint(QPainter::Antialiasing);
    int x = left;
    for (const Piece &piece : std::as_const(pieces)) {
        if (piece.cap) {
            p->setPen(capBorder);
            p->setBrush(capBackground);
            p->drawRoundedRect(QRectF(x + 0.5, top + 0.5, piece.width - 1, height - 1), 3, 3);
        }
        p->setPen(text);
        p->drawText(QRect(x, top, piece.width, height), Qt::AlignCenter, piece.text);
        x += piece.width;
    }
    p->restore();
    return left;
}
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

    // Fuzzy-matched letters (Ctrl+P, command palette): accent colour, bold.
    QColor match = Lieve::Theme::styled() ? Lieve::Theme::colors().accent : opt.palette.color(group, QPalette::Link);
    if (selected) {
        match = Lieve::Theme::styled() ? match.lighter(Lieve::Theme::colors().dark ? 140 : 100) : fg;
    }

    const int kind = index.data(KindRole).toInt();
    if (kind == CommandItem) {
        // "Category: Title  detail ........ [Ctrl]+[P]  note"
        if (index.data(SeparatorRole).toBool()) {
            QColor line = fg;
            line.setAlphaF(0.14f);
            p->setPen(line);
            p->drawLine(opt.rect.left() + 4, opt.rect.top(), opt.rect.right() - 4, opt.rect.top());
        }
        int right = r.right();
        const QString note = index.data(NoteRole).toString();
        if (!note.isEmpty()) {
            const int w = fm.horizontalAdvance(note);
            p->setPen(dim);
            p->drawText(right - w, baseline, note);
            right -= w + 14;
        }
        const QString keys = index.data(ShortcutRole).toString();
        if (!keys.isEmpty()) {
            right = drawKeys(p, keys, right, r, fg) - 12;
        }
        p->setClipRect(QRect(r.left(), r.top(), std::max(0, right - r.left()), r.height()));
        const int x = drawMatched(p, text, index.data(RangesRole).toList(), r.left(), baseline, fg, match) + 8;
        const QString detail = index.data(DirRole).toString();
        if (!detail.isEmpty() && x < right) {
            p->setPen(dim);
            p->drawText(x, baseline, fm.elidedText(detail, Qt::ElideRight, right - x));
        }
    } else if (kind == FileItem) {
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
        const QVariantList ranges = index.data(RangesRole).toList();
        int x = 0;
        if (ranges.isEmpty()) {
            const QString name = fm.elidedText(text, Qt::ElideRight, textRect.width());
            p->drawText(textRect.left(), baseline, name);
            x = textRect.left() + fm.horizontalAdvance(name) + 8;
        } else {
            x = drawMatched(p, text, ranges, textRect.left(), baseline, fg, match) + 8;
        }
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
