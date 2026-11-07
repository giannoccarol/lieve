#pragma once

#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QTimer>

class QProcess;
class QWidget;
namespace KTextEditor
{
class Document;
class View;
}

// Git change marks next to the line numbers, like VS Code's quick diff: a bar
// on lines added (green) or modified (blue) and a small red wedge where lines
// were deleted, comparing the editor's text (unsaved edits included) with the
// file in the git index. git runs once per file, and again only when its
// index entry changes; edits are compared in memory a moment after typing.
//
// The marks take the place of KTextEditor's own line modification markers,
// in the same thin column between the line numbers and the folding arrows.
class GitGutter : public QObject
{
    Q_OBJECT
public:
    enum Kind {
        Added,
        Modified,
        Deleted,
    };
    struct Change {
        int line; // first line; Deleted: the line the removed ones stood before
        int count; // lines added or modified; Deleted: lines removed
        Kind kind;
        bool operator==(const Change &) const = default;
    };

    GitGutter(KTextEditor::Document *document, QObject *parent);
    ~GitGutter() override;

    void addView(KTextEditor::View *view);
    // The file in the repository at `root` and its GitRepo::indexState():
    // git runs again only when they change. Empty state = no marks.
    void setSource(const QString &root, const QString &relative, const QString &state);
    // Compares the text with the index version now (normally done a moment
    // after the last edit).
    void compare();

    const QList<Change> &changes() const
    {
        return m_changes;
    }
    // Line to jump to for the next (or previous) change after `line`,
    // wrapping around; -1 when there are none.
    int nextChange(int line, bool forward) const;
    // The hunk of a unified diff (git diff) that shows `line` of the new file:
    // the line of its "@@" header in `diff`, or -1.
    static int diffLineFor(const QString &diff, int line);

Q_SIGNALS:
    void clicked(int line);
    // A right click on the mark of `line` (Stage Change, Revert Change...).
    void menuRequested(int line, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Border {
        QPointer<QWidget> widget; // KTextEditor's KateIconBorder
        QPointer<KTextEditor::View> view;
    };
    struct Geometry {
        int x = -1; // left of the marker column
        int rowHeight = 0;
    };

    void fetch();
    void setBase(const QByteArray &data);
    void clear();
    void repaint();
    void shiftLines(int from, int delta);
    bool isShown() const;
    const Change *changeOn(int line) const;
    Geometry geometry(const Border &border) const;
    void paint(const Border &border);
    int lineAt(const Border &border, QPoint pos) const;

    QPointer<KTextEditor::Document> m_document;
    QList<Border> m_borders;
    QString m_root;
    QString m_relative;
    QString m_key; // relative path + index state; "" = nothing to compare with
    bool m_stale = false; // the source changed: read it from git when shown
    QProcess *m_process = nullptr;
    bool m_hasBase = false;
    QList<size_t> m_base; // hash of each line of the index version
    QList<Change> m_changes; // sorted by line
    QTimer m_timer;
    int m_pressedLine = -1;
    bool m_menuPressed = false; // a right click on a mark: its release is ours too
};
