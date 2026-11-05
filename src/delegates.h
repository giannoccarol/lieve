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
};

enum ItemKind {
    FileItem = 0,
    MatchItem = 1,
};
}

// Paints "name  dim/folder  [count]" rows and search-match rows with the
// matched text highlighted.
class FileItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
