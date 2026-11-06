#pragma once

#include "scopes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <unordered_map>

class MainWindow;
class QAction;
class QTimer;
class TabView;
namespace KTextEditor
{
class Document;
class View;
}

namespace Lieve
{
class LspClient;
class LspManager;

// The scopes of each open document (see scopes.h), for the breadcrumbs and
// sticky scroll. Computed only when asked, once per document revision: from
// the language server's document symbols when one covers the file (asked
// again a moment after typing stops, meanwhile the last answer follows the
// edits), else found from the braces and the indentation.
class DocumentScopes : public QObject
{
    Q_OBJECT
public:
    DocumentScopes(LspManager *lsp, QObject *parent);
    ~DocumentScopes() override;

    std::shared_ptr<const ScopeTree> scopes(KTextEditor::Document *doc);
    // Found from the text alone (braces, indentation), now.
    std::shared_ptr<const ScopeTree> heuristic(KTextEditor::Document *doc) const;

Q_SIGNALS:
    // A newer tree is ready (the language server answered).
    void changed(KTextEditor::Document *doc);

private:
    struct Entry {
        QPointer<KTextEditor::Document> doc;
        std::shared_ptr<const ScopeTree> tree; // for `revision`
        qint64 revision = -1;
        std::shared_ptr<const ScopeTree> server; // the server's last answer, for `serverRevision` (locked)
        qint64 serverRevision = -1;
        int generation = 0; // reloads: older locked revisions are gone
        int request = -1; // a question to the server in flight, for `pendingRevision` (locked)
        qint64 pendingRevision = -1;
        int pendingGeneration = 0;
        int failures = 0;
        QPointer<LspClient> client;
        QTimer *timer = nullptr;
    };
    Entry &entry(KTextEditor::Document *doc);
    void request(Entry &e);
    void dropServer(Entry &e, bool unlock);
    std::shared_ptr<const ScopeTree> moved(const Entry &e) const;

    QPointer<LspManager> m_lsp;
    std::unordered_map<KTextEditor::Document *, Entry> m_entries;
};
}

class BreadcrumbBar;
class StickyScroll;

// Breadcrumbs above each editor group and sticky scroll in each editor
// ([editor] breadcrumbs / stickyScroll), made only while they are on.
class EditorOutline : public QObject
{
    Q_OBJECT
public:
    EditorOutline(MainWindow *window, Lieve::LspManager *lsp);

    void addGroup(TabView *group);
    void addView(KTextEditor::View *view);
    // Re-reads the settings: adds or removes the bars and overlays.
    void applySettings();

    Lieve::DocumentScopes *scopes() const
    {
        return m_scopes;
    }
    MainWindow *window() const
    {
        return m_window;
    }

    // Automated checks (LIEVE_TEST_OUTLINE): steps separated by '|'.
    void runTest(const QString &script);

private:
    BreadcrumbBar *newBar(TabView *group);
    void setBreadcrumbs(bool on);
    void setStickyScroll(bool on);
    void toggle(const QString &key, bool on);
    KTextEditor::View *currentView() const;

    MainWindow *m_window;
    Lieve::DocumentScopes *m_scopes;
    bool m_breadcrumbs = false;
    bool m_sticky = false;
    QList<QPointer<TabView>> m_groups;
    QList<QPointer<KTextEditor::View>> m_views;
    QHash<TabView *, QPointer<BreadcrumbBar>> m_bars;
    QHash<KTextEditor::View *, QPointer<StickyScroll>> m_overlays;
    QPointer<KTextEditor::View> m_current;
};
