#pragma once

#include <KTextEditor/Cursor>
#include <KTextEditor/Range>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <vector>

class MainWindow;
class StatusBar;
class SymbolPicker;
class QAction;
class QIcon;
class QWidget;
namespace KTextEditor
{
class Document;
class MovingRange;
class View;
}

namespace Lieve
{
class LspClient;
class LspCompletionModel;
class LspHints;

// Icons for LSP completion item kinds and symbol kinds.
QIcon completionKindIcon(int kind);
QIcon symbolKindIcon(int kind);

// Language intelligence for one window, from language servers (LSP):
// go to definition, problems, hover, completion and the symbols of a file.
// A server starts when the first file of its language is opened (if it is
// installed) and stops some minutes after the last one is closed, or with
// the window. Without a server everything stays as it was.
class LspManager : public QObject
{
    Q_OBJECT
public:
    struct Shortcut {
        QString text;
        QList<QKeySequence> keys;
        std::function<void()> slot;
    };

    LspManager(MainWindow *window, StatusBar *status, QWidget *overlayParent);
    ~LspManager() override;

    // The window's shortcuts (F12, Alt+Left, Ctrl+Shift+O, F8...).
    QList<Shortcut> shortcuts();

    void setRoot(const QString &root);
    void addDocument(KTextEditor::Document *doc);
    // Before the document is deleted: its underlines go first.
    void removeDocument(KTextEditor::Document *doc);
    void addView(KTextEditor::View *view);
    void setCurrentView(KTextEditor::View *view);
    void statusClicked();

    void goToDefinition(KTextEditor::View *view, KTextEditor::Cursor position);
    void navigate(int direction);
    void showDocumentSymbols(KTextEditor::View *view);
    void nextProblem(KTextEditor::View *view, bool forward);

    // For the hint provider and the completion model
    QString textHint(KTextEditor::View *view, KTextEditor::Cursor position);
    LspClient *readyClient(KTextEditor::Document *doc) const;
    void flush(KTextEditor::Document *doc);
    QJsonObject positionParams(KTextEditor::Document *doc, KTextEditor::Cursor position) const;
    // Edits from the server (imports added by a completion), made against
    // the text as it was at `revision`.
    void applyEdits(KTextEditor::Document *doc, const QJsonArray &edits, qint64 revision);

    // Automated checks (LIEVE_TEST_LSP): steps separated by '|'.
    void runTest(const QString &script);

private:
    struct Diagnostic {
        std::unique_ptr<KTextEditor::MovingRange> range;
        int severity = 1;
        QString message;
        QString source;
        QString code;
    };
    struct DocState {
        KTextEditor::Document *doc = nullptr;
        QString path;
        QString uri;
        QString languageId;
        QString serverId;
        bool open = false; // didOpen sent to the running server
        bool dirty = false;
        bool diagnosticsReceived = false;
        int version = 0;
        int pullRequest = -1;
        QString synced; // the text the server has
        std::vector<Diagnostic> diagnostics;
        std::map<int, qint64> revisions; // version sent -> locked revision
    };

    DocState *state(KTextEditor::Document *doc) const;
    DocState *stateForUri(const QString &uri) const;
    void attach(KTextEditor::Document *doc);
    void detach(DocState *s);
    LspClient *clientFor(const QString &serverId);
    void serverReady(LspClient *client);
    void serverNotification(LspClient *client, const QString &method, const QJsonObject &params);
    void sendOpen(DocState *s, LspClient *client);
    void pullDiagnostics(DocState *s, LspClient *client);
    void flushAll();
    void lockRevision(DocState *s);
    void releaseRevisions(DocState *s, int upToVersion);
    void setDiagnostics(DocState *s, const QJsonArray &diagnostics, int version);
    void clearDiagnostics(DocState *s);
    void updateStatus();
    void updateIdle();
    void stopAll(bool wait = false);

    void jumpTo(const QString &path, KTextEditor::Cursor position, bool record);
    void showMessage(KTextEditor::View *view, const QString &text);
    void showHover(KTextEditor::View *view, KTextEditor::Cursor position, const QString &diagnostics, const QJsonValue &result);
    QString diagnosticsHtml(DocState *s, KTextEditor::Cursor position) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void setLink(KTextEditor::View *view, KTextEditor::Range range);

    struct Location {
        QString path;
        KTextEditor::Cursor cursor;
    };

    MainWindow *m_window;
    StatusBar *m_status;
    QString m_root;
    std::map<KTextEditor::Document *, std::unique_ptr<DocState>> m_docs;
    QHash<QString, LspClient *> m_clients; // server id -> client
    QHash<QString, QTimer *> m_idle; // server id -> stop timer
    QHash<LspClient *, QString> m_progress; // titles of work in progress
    QList<QPointer<KTextEditor::View>> m_views;
    QPointer<KTextEditor::View> m_current;
    QTimer m_syncTimer;
    LspCompletionModel *m_completion = nullptr;
    std::unique_ptr<LspHints> m_hints;
    QAction *m_definitionAction = nullptr; // in the editor's context menu
    QPointer<KTextEditor::View> m_menuView; // where the context menu was opened
    KTextEditor::Cursor m_menuCursor = KTextEditor::Cursor::invalid();
    QElapsedTimer m_menuClock;
    int m_hoverRequest = -1;
    QPointer<LspClient> m_hoverClient;
    int m_definitionRequest = -1;
    QPointer<LspClient> m_definitionClient;

    // Ctrl+Shift+O and definitions with several answers
    SymbolPicker *m_picker = nullptr;
    QList<Location> m_pickerTargets;
    QPointer<KTextEditor::View> m_pickerView;
    KTextEditor::Cursor m_pickerOrigin;
    bool m_pickerPreview = false;

    // Alt+Left / Alt+Right
    QList<Location> m_back;
    QList<Location> m_forward;

    // Ctrl+hover: the word under the mouse looks like a link
    std::unique_ptr<KTextEditor::MovingRange> m_link;
    QPointer<KTextEditor::View> m_linkView;
    QPoint m_pressPos;
};
}
