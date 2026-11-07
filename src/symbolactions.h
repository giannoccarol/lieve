#pragma once

#include "lspedits.h"

#include <KTextEditor/Cursor>
#include <KTextEditor/Range>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class MainWindow;
class QAction;
class QWidget;
namespace KTextEditor
{
class Document;
class View;
}

namespace Lieve
{
class LspClient;
class LspManager;
class ReferenceLenses;
class ReferencesPopup;
class RenameBox;

// Go to References (Shift+F12) and Rename Symbol (F2) from the language
// server, like VS Code's, and the reference counts after declarations
// (ReferenceLenses, while [lsp] codeLens is on).
//
// The references are listed in a card under the symbol, like Visual
// Studio's CodeLens list (VS Code's peek needs an editor inside the
// editor): by file, each line with the symbol highlighted; one reference
// is jumped to at once. Rename asks for the new name in a small box under
// the symbol and changes every file: open ones in their editors (one undo
// step each, not saved), the others on disk. Nothing is made until used.
class SymbolActions : public QObject
{
    Q_OBJECT
public:
    SymbolActions(LspManager *lsp, MainWindow *window, QWidget *overlayParent);
    ~SymbolActions() override;

    // For the editor's context menu, after Go to Definition.
    QList<QAction *> menuActions() const;
    // From LspManager: views (F2, the counts), documents, focus, servers, settings.
    void addView(KTextEditor::View *view);
    void addDocument(KTextEditor::Document *doc);
    void removeDocument(KTextEditor::Document *doc);
    void setCurrentView(KTextEditor::View *view);
    void serverReady();
    void applySettings();

    // The references of the symbol at `position`. With its declaration
    // (Shift+F12) one result is jumped to; without (a count clicked) the
    // list always shows.
    void findReferences(KTextEditor::View *view, KTextEditor::Cursor position, bool includeDeclaration = true);
    void renameSymbol(KTextEditor::View *view, KTextEditor::Cursor position);

    // A message at the bottom of the editor, like Go to Definition's.
    void showMessage(KTextEditor::View *view, const QString &text, int ms = 4000);

    LspManager *lsp() const
    {
        return m_lsp;
    }
    MainWindow *window() const
    {
        return m_window;
    }

    // Automated checks (LIEVE_TEST_REFS): steps separated by '|', see the end of symbolactions.cpp.
    void runTest(const QString &script);

private:
    struct RenameOutcome {
        int occurrences = 0;
        int files = 0;
        QStringList problems;
        QStringList notApplied; // files to create, rename or delete
        bool wroteFiles = false;
    };
    // The server for `doc`, or a message saying why there is none.
    LspClient *serverOrMessage(KTextEditor::View *view, const QString &capability, const QString &needs);
    void showReferences(KTextEditor::View *view, KTextEditor::Cursor position, const QString &word, const QList<ReferenceLocation> &locations, bool jumpToOne);
    void showRenameBox(KTextEditor::View *view, KTextEditor::Range range, const QString &placeholder);
    void rename(KTextEditor::View *view, KTextEditor::Cursor position, const QString &newName);
    RenameOutcome apply(const WorkspaceEditPlan &plan, const QList<QPair<QPointer<KTextEditor::Document>, qint64>> &locked);
    QString summary(const RenameOutcome &outcome) const;
    void setLenses(bool on);
    void toggleLenses();

    LspManager *m_lsp;
    MainWindow *m_window;
    QWidget *m_overlayParent;
    QAction *m_referencesMenu = nullptr; // context menu entries (the keys are the window's and the editor's)
    QAction *m_renameMenu = nullptr;
    QPointer<ReferencesPopup> m_popup; // made when first shown
    QMetaObject::Connection m_popupScroll; // its editor scrolling closes it
    QPointer<RenameBox> m_renameBox;
    ReferenceLenses *m_lenses = nullptr; // [lsp] codeLens on and a server ready
    bool m_lensesOn = false;
    QList<QPointer<KTextEditor::View>> m_views;
    QPointer<KTextEditor::View> m_current;
    int m_referencesRequest = -1;
    QPointer<LspClient> m_referencesClient;
    int m_prepareRequest = -1;
    QPointer<LspClient> m_prepareClient;
    QString m_lastMessage; // for the checks
    int m_messages = 0;
    int m_testMark = 0;
};
}
