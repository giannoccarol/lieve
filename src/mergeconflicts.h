#pragma once

#include "conflicts.h"

#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>

#include <memory>
#include <vector>

namespace KTextEditor
{
class MovingRange;
}
class ConflictNotes;

// Merge conflict helpers in the editor, like VS Code's: the blocks between
// git's conflict markers are tinted (current change green, incoming blue,
// common ancestor grey), the <<<<<<< line offers Accept Current Change,
// Accept Incoming Change and Accept Both Changes, and the "Merge Conflict"
// commands of the palette move between the conflicts or resolve them all.
// Each resolution is one undo step. Every view of the document shows them.
//
// While a document has no conflict nothing is shown or registered and
// nothing runs: edits are only checked for a marker line they touch, and a
// scan follows a moment after the last one.
class MergeConflicts : public QObject
{
    Q_OBJECT
public:
    // Follows the document and each of its views from now on (once per
    // document; it goes away with the document).
    static MergeConflicts *watch(KTextEditor::Document *document);
    // The helpers of a document, or null if it is not watched.
    static MergeConflicts *of(const KTextEditor::Document *document);
    // True if `line` of the document shows a note of ours (marker label,
    // actions): other inline notes keep off that line.
    static bool hasNote(const KTextEditor::Document *document, int line);

    ~MergeConflicts() override;

    KTextEditor::Document *document() const
    {
        return m_document;
    }
    // As of the last scan.
    const QList<Lieve::Conflict> &conflicts() const
    {
        return m_conflicts;
    }
    // Scans now if an edit is still waiting for its scan.
    void update();
    // Resolves conflict `index`, or all of them, keeping `side`: one undo step.
    void accept(int index, Lieve::ConflictSide side);
    void acceptAll(Lieve::ConflictSide side);
    // Puts the cursor of `view` on the next (previous) conflict, wrapping around.
    void goTo(KTextEditor::View *view, bool forward);
    // Where action `action` (0 current, 1 incoming, 2 both) of conflict
    // `index` is drawn in `view`, in view coordinates; empty if not shown.
    QRect actionRect(KTextEditor::View *view, int index, int action) const;
    // Test hook (LIEVE_TEST_CONFLICTS): steps separated by ';' on the
    // current view, a moment apart: list, dump, next, prev, undo,
    // accept:<n>:<current|incoming|both>, all:<side>, hover:<n>:<action>,
    // click:<n>:<action>, insert:<line>:<text> (\n for new lines).
    static void runTest(KTextEditor::View *view, const QString &steps);

private:
    friend class ConflictNotes;
    enum Note {
        NoNote,
        StartNote, // the <<<<<<< line: label and actions
        EndNote, // the >>>>>>> line: label
    };
    struct Block {
        std::unique_ptr<KTextEditor::MovingRange> start; // the <<<<<<< line
        std::unique_ptr<KTextEditor::MovingRange> end; // the >>>>>>> line
        std::vector<std::unique_ptr<KTextEditor::MovingRange>> sections; // tinted lines between
    };

    explicit MergeConflicts(KTextEditor::Document *document);
    void checkLines(int from, int to);
    void scan();
    bool intact(const QList<Lieve::Conflict> &conflicts) const;
    void rebuild(const QList<Lieve::Conflict> &conflicts);
    void clear();
    void updateViews();
    void attach(KTextEditor::View *view);
    void detach(KTextEditor::View *view);
    void addCommands(KTextEditor::View *view);
    std::unique_ptr<KTextEditor::MovingRange> tint(int from, int to, const KTextEditor::Attribute::Ptr &style);
    Note noteAt(int line, int *index = nullptr) const;
    int indexAt(int startLine) const;
    void replaceBlock(const Lieve::Conflict &conflict, Lieve::ConflictSide side);

    QPointer<KTextEditor::Document> m_document;
    QList<Lieve::Conflict> m_conflicts;
    std::vector<Block> m_blocks; // same order as m_conflicts
    QTimer m_timer;
    std::unique_ptr<ConflictNotes> m_notes; // made with the first conflict, kept until the document goes
    QList<QPointer<KTextEditor::View>> m_registered; // views showing the notes
    KTextEditor::Attribute::Ptr m_styles[6];
};
