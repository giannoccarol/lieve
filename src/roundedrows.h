#pragma once

#include <QEvent>
#include <QHoverEvent>
#include <QPersistentModelIndex>
#include <QTreeView>

// Side bar trees with rounded rows: the selection and the hover are drawn as
// one rounded shape over the whole row (indentation and chevron too), and
// selected rows next to each other join up. With [ui] theme=system the
// platform style draws the rows as usual.
namespace RoundedRows
{
// Paints the pill behind `index`'s row, if it is selected or is `hovered`'s row.
void paintBackground(const QTreeView *view, QPainter *painter, const QRect &row, const QModelIndex &index, const QModelIndex &hovered);
}

template<typename Base>
class RoundedTree : public Base
{
public:
    explicit RoundedTree(QWidget *parent = nullptr)
        : Base(parent)
    {
        // The stylesheet leaves the backgrounds of these rows to drawRow().
        this->setProperty("roundedRows", true);
        this->viewport()->setAttribute(Qt::WA_Hover);
    }

protected:
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        RoundedRows::paintBackground(this, painter, option.rect, index, m_hover);
        Base::drawRow(painter, option, index);
    }
    bool viewportEvent(QEvent *event) override
    {
        // The view repaints the rows the mouse leaves and enters.
        switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
            m_hover = this->indexAt(static_cast<QHoverEvent *>(event)->position().toPoint());
            break;
        case QEvent::HoverLeave:
        case QEvent::Leave:
            m_hover = QPersistentModelIndex();
            break;
        default:
            break;
        }
        return Base::viewportEvent(event);
    }
    // The selection is tinted with the accent while the window is active.
    void changeEvent(QEvent *event) override
    {
        Base::changeEvent(event);
        if (event->type() == QEvent::ActivationChange) {
            this->viewport()->update();
        }
    }

private:
    QPersistentModelIndex m_hover;
};
