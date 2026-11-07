#pragma once

#include "linediff.h"

#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QPoint;
class QWidget;
namespace KTextEditor
{
class Document;
class View;
}
namespace Lieve
{
class GitRepo;
}

// "Stage Selected Ranges", "Unstage Selected Ranges" and "Revert Selected
// Ranges", like VS Code's: part of a file's changes put into or taken out of
// the index, or taken back in the editor. Staging writes the index text with
// only the chosen changes applied (git hash-object + update-index, through
// GitRepo's queue); unstaging does the same between HEAD and the index;
// reverting edits the document (one undo step, not saved). Which changes a
// selection picks: see LineSelection (linediff.h). The working text is the
// document's, unsaved edits included.
class GitStaging : public QObject
{
    Q_OBJECT
public:
    enum Command {
        Stage,
        Unstage,
        Revert,
    };

    explicit GitStaging(Lieve::GitRepo *repo, QObject *parent = nullptr);

    Lieve::GitRepo *repo() const
    {
        return m_repo;
    }
    // A document's file relative to the repository root ("" outside it).
    QString relativePath(const KTextEditor::Document *doc) const;
    // The file has changes that are not staged (or is new).
    bool hasChanges(const QString &path) const;

    // "Stage Selected Ranges", or "Stage Change" for the change at the cursor.
    static QString commandText(Command command, bool selection = true);
    static QIcon commandIcon(Command command);
    // The lines of a view's selection; without one, the cursor line and the
    // whole changes it touches.
    static Lieve::LineSelection selectionOf(KTextEditor::View *view, Lieve::DiffSide side);

    // The changes `selection` picks between the index and `doc` (the file at
    // `path`, relative to the root).
    void stage(KTextEditor::Document *doc, const QString &path, const Lieve::LineSelection &selection);
    void revert(KTextEditor::Document *doc, const QString &path, const Lieve::LineSelection &selection);
    // The staged changes it picks between HEAD (the file was `headPath`
    // there) and the index; `messages` shows "nothing to unstage".
    void unstage(const QString &path, const QString &headPath, const Lieve::LineSelection &selection, KTextEditor::Document *messages);

    // A command on an editor's selection (palette, shortcut, context menu).
    void run(Command command, KTextEditor::View *view);
    // Stage and Revert in the context menu of an editor whose file has changes.
    void addView(KTextEditor::View *view);
    // The menu of a change mark next to the line numbers: open, stage, revert.
    void showChangeMenu(QWidget *parent, KTextEditor::Document *doc, int line, const QPoint &globalPos, const std::function<void()> &openChange);

    // Replaces the text of `doc` with `text`, touching only the part that
    // differs (scroll position, marks and folds elsewhere stay): one undo step.
    static void replaceText(KTextEditor::Document *doc, const QString &text);
    static void showMessage(KTextEditor::Document *doc, const QString &text);
    // The document's lines and line breaks, as the hunk maths wants them.
    static QStringList documentLines(const KTextEditor::Document *doc);
    static bool documentCrlf(KTextEditor::Document *doc);

private:
    Lieve::GitRepo *m_repo;
    QPointer<KTextEditor::View> m_menuView; // the view whose context menu is open
};
