#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QTimer>

#include <memory>
#include <unordered_map>

namespace KTextEditor
{
class Document;
class View;
}

namespace Lieve
{
class DocumentScopes;
class LensNotes;
class LspClient;
class LspManager;
class SymbolActions;

// Reference counts after declarations, like Visual Studio's CodeLens and
// VS Code's TypeScript reference lenses: a small dim "3 references" at the
// end of the line of each class, interface, function, method and property
// (KTextEditor cannot add lines above a line); a click lists them.
//
// Only for the lines on screen of the editors that are shown, asked a
// moment after scrolling or typing stops, a few at a time. The counts are
// kept for each document revision and asked again after a file is saved;
// meanwhile the last ones stay. They come from the server's code lenses
// (textDocument/codeLens, codeLens/resolve) or, for a server without them,
// from its document symbols (DocumentScopes) and textDocument/references.
// Without a language server for the file nothing is shown. Made only
// while [lsp] codeLens is on.
class ReferenceLenses : public QObject
{
    Q_OBJECT
public:
    explicit ReferenceLenses(SymbolActions *actions);
    ~ReferenceLenses() override;

    // True if `line` of the document shows a count: other notes at the end
    // of the line (the inline blame) keep off it.
    static bool hasNote(const KTextEditor::Document *doc, int line);

    void addView(KTextEditor::View *view);
    void removeDocument(KTextEditor::Document *doc);
    // Counts change when files change: asked again for what is shown.
    void filesChanged();
    // Asks for the lines on screen of `view` after `delay` ms (or of every view shown).
    void schedule(KTextEditor::View *view, int delay);
    void scheduleAll(int delay);

    // For the checks (LIEVE_TEST_REFS)
    struct Shown {
        int line = 0;
        QList<int> counts;
    };
    QList<Shown> shown(KTextEditor::View *view) const;
    bool idle() const;
    // Where count `segment` of `line` is drawn, in view coordinates.
    QRect noteRect(KTextEditor::View *view, int line, int segment) const;
    // What clicking it does (also from the checks).
    void open(KTextEditor::View *view, int line, int segment);

private:
    friend class LensNotes;
    LensNotes *notesFor(KTextEditor::Document *doc, bool create);
    void refreshPending();
    void refresh(KTextEditor::View *view);
    void configure(LspClient *client);
    void requestList(LensNotes *notes, LspClient *client);
    void listFromSymbols(LensNotes *notes);
    void queueVisible(LensNotes *notes);
    void pump();
    void resolve(LensNotes *notes, int lensId, LspClient *client);
    void finished(LensNotes *notes, int lensId, qint64 revision, int generation, int count);
    void openLens(KTextEditor::View *view, LensNotes *notes, int lensId);
    QList<KTextEditor::View *> viewsOf(const KTextEditor::Document *doc) const;
    DocumentScopes *scopes();

    struct Job {
        QPointer<LensNotes> notes;
        int lens = 0;
    };

    SymbolActions *m_actions;
    LspManager *m_lsp;
    std::unordered_map<const KTextEditor::Document *, std::unique_ptr<LensNotes>> m_notes;
    QSet<const KTextEditor::Document *> m_watched; // connected to
    QList<QPointer<KTextEditor::View>> m_views;
    QList<QPointer<KTextEditor::View>> m_pending; // asked for when the timer fires
    QTimer m_timer;
    QElapsedTimer m_lastEdit; // in a document with counts
    QList<Job> m_queue;
    int m_inFlight = 0;
    int m_generation = 0; // files saved or written: older counts are asked again
    int m_nextLens = 1;
    QSet<LspClient *> m_configured; // servers told to make reference lenses
    QSet<LspClient *> m_seen;
    QPointer<DocumentScopes> m_scopes;
};
}
