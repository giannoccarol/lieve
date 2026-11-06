#pragma once

#include <QIcon>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

class BreadcrumbPicker;
class TabView;
namespace KTextEditor
{
class View;
}
namespace Lieve
{
class DocumentScopes;
struct ScopeTree;
}

// VS Code's breadcrumbs: a thin bar between an editor group's tabs and its
// editor with the path of the current file and the symbols around the
// cursor (src › app › file.ts › Class › method). A folder opens a list of
// its files, the file and each symbol a list of the symbols at that level.
class BreadcrumbBar : public QWidget
{
    Q_OBJECT
public:
    // `root`: the project's folder (paths are shown from there); `open` opens a file.
    BreadcrumbBar(TabView *group,
                  Lieve::DocumentScopes *scopes,
                  const std::function<QString()> &root,
                  const std::function<void(const QString &)> &open);
    ~BreadcrumbBar() override;

    // For the automated checks: the parts shown, their lists.
    QStringList texts() const;
    bool openList(int index);
    QStringList listRows() const;
    bool activateListRow(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Segment {
        enum Type {
            Folder,
            File,
            Symbol,
            Ellipsis, // folders left out for lack of room
        };
        Segment(Type t, const QString &label, const QIcon &image, const QString &where)
            : type(t)
            , text(label)
            , icon(image)
            , path(where)
        {
        }
        Type type = File;
        QString text;
        QIcon icon;
        QString path; // Folder: the folder; File: the file
        int scope = -1; // Symbol: index in m_tree
        QRect rect;
        bool shown = true;
    };

    void setView(KTextEditor::View *view);
    void schedule(int ms);
    void rebuild();
    void layoutSegments();
    int segmentAt(const QPoint &pos) const;
    void showList(int index);
    void closeList();

    TabView *m_group;
    Lieve::DocumentScopes *m_scopes;
    std::function<QString()> m_root;
    std::function<void(const QString &)> m_openFile;
    QPointer<KTextEditor::View> m_view;
    QList<QMetaObject::Connection> m_connections;
    QList<Segment> m_segments;
    std::shared_ptr<const Lieve::ScopeTree> m_tree;
    qint64 m_revision = -1; // of the document when the parts were made
    QTimer m_timer;
    int m_hovered = -1;
    int m_open = -1; // the segment whose list is shown
    bool m_dirty = true;
    QPointer<BreadcrumbPicker> m_picker;
};
