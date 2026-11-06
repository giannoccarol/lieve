#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QIcon>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace KTextEditor
{
class Document;
class InlineNoteProvider;
class MovingRange;
class View;
}
namespace Lieve
{
class GitRepo;
struct GitEntry;
class LspManager;
}
class Explorer;
class GitGutter;
class GitPanel;
class ProjectList;
class QAction;
class QFrame;
class QMenu;
class QSplitter;
class QStackedWidget;
class DocumentBlame;
class QProcess;
class StatusBar;
class TabView;
class TitleBar;
class QToolBar;
class QuickOpen;
class SearchPanel;
class TerminalPanel;

// One window = one project (or the project chooser when it has none).
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    QString project() const
    {
        return m_root;
    }
    // Opens a project in this window (which must have none) and restores
    // that project's tabs, layout and terminal.
    void setProject(const QString &dir);
    // Saves the session and goes back to the chooser. False if the user
    // cancelled on an unsaved file.
    bool closeProject();
    void focusChooser();
    void showProjectSwitcher();

    KTextEditor::View *openFile(const QString &path, int line = -1, int column = 0, int selectLength = 0);
    void showSearch(const QString &text = QString(), bool replace = false);
    void setTerminalVisible(bool visible);
    // Re-reads the settings that apply to open editors without a restart.
    void applySettings();

Q_SIGNALS:
    void projectChanged();

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    QWidget *buildChooserPage();
    QWidget *buildEmptyPage();
    void buildSwitcher();
    void placeSwitcher();
    void buildActions();
    QAction *addShortcut(const QString &text, const QList<QKeySequence> &keys, bool keepInTerminal, const std::function<void()> &slot);

    // Side bar and panels
    void showSidePage(int page, bool toggle);
    void setSidebarVisible(bool visible);
    void toggleTerminalFocus();
    void newTerminal(const QString &dir = QString());
    void syncActivityBar();

    // Editors
    KTextEditor::View *addView(KTextEditor::Document *doc, TabView *group = nullptr);
    void configureView(KTextEditor::View *view);
    void applyKeymap(KTextEditor::View *view);
    KTextEditor::View *currentView() const;
    KTextEditor::View *viewAt(int index) const; // in the active group
    // Editor groups side by side (Ctrl+\\): m_tabs is the active one.
    TabView *createGroup();
    void removeGroup(TabView *group);
    void setActiveGroup(TabView *group);
    TabView *groupOf(KTextEditor::View *view) const;
    void activateView(KTextEditor::View *view);
    QList<KTextEditor::View *> allViews() const; // every group
    QList<KTextEditor::Document *> documents() const; // each once
    void splitEditor();
    void focusGroup(int index);
    KTextEditor::View *findView(const QString &path) const;
    bool closeView(KTextEditor::View *view);
    bool closeAllViews();
    void closeTabsWhere(const std::function<bool(int)> &predicate);
    void newUntitled();
    void saveAll();
    void openFileDialog();
    void openSettings();
    void cycleTab(int step);
    void onCurrentTabChanged();
    void updateEditorPage();
    void updateTabTitles();
    void updateWindowTitle();
    void showTabMenu(const QPoint &pos);
    void onPathRenamed(const QString &oldPath, const QString &newPath);
    void onPathRemoved(const QString &path);
    QString selectedText() const;
    void updateBranch();

    // Git
    void showDiff(const Lieve::GitEntry &entry, const QString &text);
    void showDiffTab(const QString &key, const QString &title, const QString &iconName, const QString &text);
    void colorDiff(KTextEditor::Document *doc);
    void updateGitBadge();
    void updateGutter(KTextEditor::Document *doc);
    void goToChange(bool forward);
    void showChange(KTextEditor::Document *doc, int line);
    QString documentTitle(KTextEditor::Document *doc) const;

    // Bottom status bar
    void updateStatusBar();
    void updateGitStatus();
    void updateCursorStatus();
    void showBlame();
    DocumentBlame *blameFor(KTextEditor::Document *doc);
    void onStatusClicked(const QString &name);
    void updateUpdaterStatus();
    void showUpdateDialog();

    // Prettier
    QString projectFor(const QString &path) const;
    void formatDocument(KTextEditor::Document *doc, bool onSave);

    // Per-project session (tabs, cursors, layout, window size)
    void saveSession();
    void restoreSession(bool applyGeometry);

    // Hot exit: unsaved documents kept in backups (see backups.h)
    bool hotExit() const;
    bool backupDocument(KTextEditor::Document *doc);
    void dropBackup(KTextEditor::Document *doc);
    bool keepUnsaved();
    KTextEditor::View *restoreDocument(const QString &path, const QString &backupName);

    QString m_root;
    QToolBar *m_activityBar = nullptr;
    QAction *m_actExplorer = nullptr;
    QAction *m_actSearch = nullptr;
    QAction *m_actGit = nullptr;
    QIcon m_gitIcon;
    QAction *m_actTerminal = nullptr;
    QAction *m_actSidebar = nullptr;
    QMenu *m_mainMenu = nullptr;
    QSplitter *m_mainSplit = nullptr;
    QSplitter *m_rightSplit = nullptr;
    QStackedWidget *m_side = nullptr;
    Explorer *m_explorer = nullptr;
    SearchPanel *m_search = nullptr;
    Lieve::GitRepo *m_git = nullptr;
    GitPanel *m_gitPanel = nullptr;
    QStackedWidget *m_editorStack = nullptr;
    QWidget *m_chooserPage = nullptr;
    ProjectList *m_chooser = nullptr;
    QWidget *m_emptyPage = nullptr;
    TabView *m_tabs = nullptr; // the active editor group
    QList<TabView *> m_groups;
    QSplitter *m_editorSplit = nullptr;
    TitleBar *m_titleBar = nullptr;
    StatusBar *m_status = nullptr;
    QHash<KTextEditor::Document *, DocumentBlame *> m_blames; // git blame of each open file
    QSet<KTextEditor::Document *> m_staleBlames; // HEAD moved: blame again when shown
    QString m_blameHead;
    QHash<QString, QString> m_gitEmails; // repository -> user.email
    QHash<KTextEditor::View *, KTextEditor::InlineNoteProvider *> m_blameNotes; // inline blame per view
    QHash<KTextEditor::Document *, QPointer<GitGutter>> m_gutters; // git change marks of each open file
    QWidget *m_body = nullptr;
    TerminalPanel *m_terminal = nullptr;
    QuickOpen *m_quickOpen = nullptr;
    Lieve::LspManager *m_lsp = nullptr; // language servers: definitions, problems, completion
    QFrame *m_switcher = nullptr;
    ProjectList *m_switcherList = nullptr;

    QStringList m_mru; // recently focused files, newest first
    QList<QKeySequence> m_appKeys; // window shortcuts: removed from the editor's own
    QList<QKeySequence> m_terminalKeys; // window shortcuts that also work inside the terminal
    int m_sideWidth = 260;
    int m_terminalHeight = 260;
    KTextEditor::Document *m_saveWithoutFormat = nullptr;
    QFileSystemWatcher m_gitWatcher;
    QTimer m_branchTimer;
    QTimer m_gitPoll;
    QTimer m_backupTimer; // backs up unsaved documents shortly after typing stops
    QElapsedTimer m_backupWait; // since the first edit the timer is waiting for
    bool m_keepBackups = false; // closing with hot exit: the documents go, their backups stay
    // Coloured backgrounds of the lines in diff tabs
    QHash<KTextEditor::Document *, std::shared_ptr<std::vector<std::unique_ptr<KTextEditor::MovingRange>>>> m_diffRanges;
};
