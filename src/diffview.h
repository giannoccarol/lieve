#pragma once

#include "git.h"
#include "gitstaging.h"
#include "linediff.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QMenu;
class QSplitter;
class TabView;
class DiffStrip;
namespace KTextEditor
{
class Document;
class MovingRange;
class View;
}

// A side-by-side diff tab, like VS Code's diff editor: on the left the
// original (the index for unstaged changes, HEAD for staged ones), read-only;
// on the right the new version: for unstaged changes a view of the file's
// own document (the one of its tab), so edits there are normal edits and the
// diff follows them a moment after typing; for staged ones the index text,
// read-only. Removed lines are tinted red on the left, added ones green on
// the right, the words that changed inside them stronger.
//
// KTextEditor cannot add filler lines, so the two sides scroll together
// line for line through unchanged text and proportionally through a change
// (the line in the middle of the view is the one kept aligned, as in Meld),
// and the strip between them draws each change as a band from its lines on
// the left to its lines on the right. The strip carries the buttons of the
// change under the mouse or at the cursor: Stage Change and Revert Change,
// or Unstage Change; it can be dragged to resize the sides.
//
// The working file's document stays the window's: when the file has no tab,
// the page opens it itself and hands it over (a tab of its own) at the first
// edit, or when the file is opened. The page closes with that document.
class DiffView : public QWidget
{
    Q_OBJECT
public:
    // What a diff shows, read from git before the page is made.
    struct Texts {
        bool ok = false; // false: binary, too large or unreadable (the unified diff instead)
        QByteArray original; // left: the index (unstaged changes) or HEAD (staged ones)
        QByteArray modified; // right, for staged changes: the index
    };
    // Changes that can be shown side by side (not deletions, conflicts, links).
    static bool supports(const Lieve::GitEntry &entry);
    // Reads the texts of `entry` (in GitRepo's queue).
    static void load(Lieve::GitRepo *repo, const Lieve::GitEntry &entry, const std::function<void(const Texts &)> &done);
    // The key of the diff tab of an entry ("<group>:<path>", also the unified
    // diff tab's), and the entry of such a key among the repository's.
    static QString keyOf(const Lieve::GitEntry &entry);
    static std::optional<Lieve::GitEntry> entryOf(const Lieve::GitRepo *repo, const QString &key);

    // `working`: the file's document when it has a tab (unstaged changes);
    // `configure` sets up an editor view like the window's (shortcuts).
    DiffView(GitStaging *staging,
             const Lieve::GitEntry &entry,
             const Texts &texts,
             KTextEditor::Document *working,
             const std::function<void(KTextEditor::View *)> &configure,
             QWidget *parent = nullptr);
    ~DiffView() override;

    const Lieve::GitEntry &entry() const
    {
        return m_entry;
    }
    bool staged() const;
    QString key() const;
    // "app.ts (Working Tree)" or "app.ts (Index)", with ● while the file has unsaved changes.
    QString title() const;
    QString path() const; // absolute
    KTextEditor::Document *document() const; // the right side's
    // The working file's document is this page's own (the file has no tab).
    bool ownsDocument() const;
    // Hands that document over to the window (null if it is not its own).
    KTextEditor::Document *releaseDocument();
    KTextEditor::View *leftView() const;
    KTextEditor::View *rightView() const;
    KTextEditor::View *focusedView() const; // the side last focused (the right one at first)
    const QList<Lieve::LineHunk> &changes() const
    {
        return m_hunks;
    }

    // Alt+F5 / Shift+Alt+F5: the cursor on the next (previous) change, wrapping around.
    void goToChange(bool forward);
    // Shows the change at (or after) `line` of the right side (a change mark's
    // click); -1 = the first change.
    void revealLine(int line);
    // Stage, unstage or revert the selection of the focused side.
    void run(GitStaging::Command command);
    // The button of change `index` in the strip.
    void runOnChange(int index, GitStaging::Command command);

    // VS Code's buttons for diffs at the right of a group's tab bar (Open File,
    // Previous / Next Change, Inline / Side by Side), shown on diff tabs.
    static QWidget *createCorner(TabView *group,
                                 Lieve::GitRepo *repo,
                                 const std::function<void(QWidget *page)> &toggle,
                                 const std::function<void(QWidget *page)> &openFile);
    // The next (previous) "@@" of a unified diff tab.
    static void goToHunk(KTextEditor::View *view, bool forward);
    // Test hook (LIEVE_TEST_DIFF): steps separated by ';', see the definition.
    static void runTest(QWidget *window, const QString &steps);

Q_SIGNALS:
    void titleChanged();
    void focused();
    // The page's own working document was edited: the window takes it over.
    void documentEdited();
    // The working document went away (its last tab was closed).
    void documentClosed();

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    friend class DiffStrip;

    void setupView(KTextEditor::View *view);
    void fillMenu(KTextEditor::View *view, QMenu *menu);
    void refresh(); // the index (or HEAD) changed
    void setTexts(const Texts &texts);
    QString decode(const QByteArray &data) const;
    void compare();
    void rebuildRanges(const QStringList &left, const QStringList &right);
    void updateTints();
    void shiftRight(int from, int delta);
    void syncFrom(KTextEditor::View *view);
    void syncHorizontal(KTextEditor::View *view, int value);
    double mapLine(double line, bool fromLeft) const;
    void doReveal();
    void showChange(int index, KTextEditor::View *view);
    int changeAt(int line, bool left) const;
    int currentChange() const;
    int rowHeight(KTextEditor::View *view) const;
    int visibleRows(KTextEditor::View *view) const;
    QString headPath() const;

    GitStaging *m_staging;
    Lieve::GitEntry m_entry;
    std::function<void(KTextEditor::View *)> m_configure;
    QSplitter *m_split = nullptr;
    QPointer<DiffStrip> m_strip;
    QPointer<KTextEditor::Document> m_leftDoc; // always this page's
    QPointer<KTextEditor::Document> m_rightDoc;
    bool m_ownsRight = false;
    QPointer<KTextEditor::View> m_left;
    QPointer<KTextEditor::View> m_right;
    QPointer<KTextEditor::View> m_focused;
    QByteArray m_encoding;
    QList<Lieve::LineHunk> m_hunks; // left (a) to right (b)
    std::vector<std::unique_ptr<KTextEditor::MovingRange>> m_leftRanges;
    std::vector<std::unique_ptr<KTextEditor::MovingRange>> m_rightRanges;
    QTimer m_compareTimer;
    QString m_state; // the index (and HEAD) the texts were read from
    quint64 m_readSerial = 0;
    std::optional<int> m_reveal; // a line to show once laid out
    bool m_shown = false;
    bool m_stale = false; // changed while hidden
    bool m_syncing = false;
    bool m_closing = false;
};
