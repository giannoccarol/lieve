#pragma once

#include <QStyledItemDelegate>

namespace Lieve
{
enum ItemRole {
    KindRole = Qt::UserRole + 1, // ItemKind
    PathRole, // absolute file path
    DirRole, // folder shown dimmed next to the name
    LineRole, // 0-based
    ColumnRole, // 0-based
    LengthRole,
    RangesRole, // QVariantList of QPoint(start, length) to highlight in the text
    CountRole, // badge on file rows (0 = none)
    ShortcutRole, // command rows: key sequence drawn as key caps on the right
    NoteRole, // command rows: dim note at the far right ("recently used")
    SeparatorRole, // command rows: a line above the row
};

enum ItemKind {
    FileItem = 0,
    MatchItem = 1,
    CommandItem = 2,
};
}

// Paints "name  dim/folder  [count]" rows, search-match rows with the
// matched text highlighted and command palette rows.
class FileItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

protected:
    // Text replacing each highlighted range of a match row, drawn as a change
    // (old text struck out, new text after it); empty: plain highlight.
    virtual QStringList replacements(const QModelIndex &index) const
    {
        return {};
    }
    // Room for buttons shown on hover, at the right of the row (left of the count badge).
    virtual int buttonsWidth(const QModelIndex &index) const
    {
        return 0;
    }
    QRect buttonsRect(const QStyleOptionViewItem &option, const QModelIndex &index) const;

private:
    void layout(const QStyleOptionViewItem &opt, const QModelIndex &index, QRect *text, QRect *badge) const;
};
