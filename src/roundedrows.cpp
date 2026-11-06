#include "roundedrows.h"

#include "theme.h"

#include <QItemSelectionModel>
#include <QPainter>

namespace
{
constexpr int kInset = 4; // from the left and right edges of the tree
constexpr qreal kRadius = 6;
}

void RoundedRows::paintBackground(const QTreeView *view, QPainter *p, const QRect &row, const QModelIndex &index, const QModelIndex &hovered)
{
    if (!Lieve::Theme::styled()) {
        return;
    }
    const QItemSelectionModel *selection = view->selectionModel();
    const auto isSelected = [selection](const QModelIndex &i) {
        return selection && i.isValid() && selection->isSelected(i.siblingAtColumn(0));
    };
    const bool selected = isSelected(index);
    if (!selected && !(hovered.isValid() && hovered.row() == index.row() && hovered.parent() == index.parent())) {
        return;
    }

    const Lieve::Colors &c = Lieve::Theme::colors();
    const QColor fill = !selected ? c.hover : view->isActiveWindow() ? c.selection : c.selectionInactive;
    const QRectF r(kInset, row.top(), view->viewport()->width() - 2 * kInset, row.height());

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    p->setBrush(fill);
    p->drawRoundedRect(r, kRadius, kRadius);
    // Square off the corners that touch another selected row.
    if (selected && isSelected(view->indexAbove(index))) {
        p->fillRect(QRectF(r.left(), r.top(), r.width(), kRadius), fill);
    }
    if (selected && isSelected(view->indexBelow(index))) {
        p->fillRect(QRectF(r.left(), r.bottom() - kRadius, r.width(), kRadius), fill);
    }
    p->restore();
}
