#include "mainwindow.h"

#include "backups.h"
#include "blame.h"
#include "commandpalette.h"
#include "diffview.h"
#include "explorer.h"
#include "git.h"
#include "gitgutter.h"
#include "gitpanel.h"
#include "gitstaging.h"
#include "linetint.h"
#include "lsp.h"
#include "markdownpreview.h"
#include "mergeconflicts.h"
#include "npmscripts.h"
#include "outline.h"
#include "prettier.h"
#include "problemspanel.h"
#include "projectlist.h"
#include "projects.h"
#include "quickopen.h"
#include "searchpanel.h"
#include "settings.h"
#include "settingsdialog.h"
#include "statusbar.h"
#include "tabview.h"
#include "terminalpanel.h"
#include "titlebar.h"
#include "updater.h"
#include "version.h"
#include "theme.h"
#include "vscodeicons.h"
#include "windowmanager.h"

#include <KActionCollection>
#include <KSelectAction>
#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/InlineNote>
#include <KTextEditor/InlineNoteProvider>
#include <KTextEditor/Message>
#include <KTextEditor/MovingRange>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextFormat>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

using namespace Lieve;

namespace
{
enum SidePage {
    ExplorerPage = 0,
    SearchPage = 1,
    GitPage = 2,
};

QIcon themeIcon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QIcon icon = QIcon::fromTheme(QLatin1String(name));
        if (!icon.isNull()) {
            return icon;
        }
    }
    return {};
}

QString localPath(const KTextEditor::Document *doc)
{
    return doc->url().isLocalFile() ? doc->url().toLocalFile() : QString();
}

QString sessionKey(const QString &root)
{
    return QStringLiteral("sessions/") + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

// Offsets <-> cursors in a document's text() (lines joined with '\n').
int offsetOf(const QString &text, const KTextEditor::Cursor &cursor)
{
    qsizetype offset = 0;
    for (int line = 0; line < cursor.line(); ++line) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), offset);
        if (newline < 0) {
            return int(text.size());
        }
        offset = newline + 1;
    }
    return int(std::min<qsizetype>(offset + cursor.column(), text.size()));
}

KTextEditor::Cursor cursorAt(const QString &text, qsizetype offset)
{
    offset = std::clamp<qsizetype>(offset, 0, text.size());
    int line = 0;
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < offset; ++i) {
        if (text.at(i) == QLatin1Char('\n')) {
            ++line;
            lineStart = i + 1;
        }
    }
    return KTextEditor::Cursor(line, int(offset - lineStart));
}

// Replaces only the part that changed, so scroll position, bookmarks and
// folding outside it are kept; one undo step reverts the formatting.
void applyFormatted(KTextEditor::Document *doc, KTextEditor::View *view, const QString &old, QString formatted, int cursor)
{
    formatted.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (formatted == old) {
        return;
    }
    const qsizetype common = std::min(old.size(), formatted.size());
    qsizetype prefix = 0;
    while (prefix < common && old.at(prefix) == formatted.at(prefix)) {
        ++prefix;
    }
    if (prefix > 0 && old.at(prefix - 1).isHighSurrogate()) {
        --prefix;
    }
    qsizetype suffix = 0;
    while (suffix < common - prefix && old.at(old.size() - 1 - suffix) == formatted.at(formatted.size() - 1 - suffix)) {
        ++suffix;
    }
    if (suffix > 0 && old.at(old.size() - suffix).isLowSurrogate()) {
        --suffix;
    }
    const KTextEditor::Range range(cursorAt(old, prefix), cursorAt(old, old.size() - suffix));
    {
        KTextEditor::Document::EditingTransaction transaction(doc);
        doc->replaceText(range, formatted.mid(prefix, formatted.size() - prefix - suffix));
    }
    if (view && cursor >= 0) {
        view->setCursorPosition(cursorAt(formatted, cursor));
    }
}

void showMessage(KTextEditor::Document *doc, const QString &text, KTextEditor::Message::MessageType type)
{
    // Wrapped in a tag so it is shown as rich text (escaped quotes render properly).
    auto *message = new KTextEditor::Message(QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped()), type);
    message->setPosition(KTextEditor::Message::BottomInView);
    message->setAutoHide(6000);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    doc->postMessage(message);
}

// Inline blame like GitLens: dim italic text after the end of the cursor line.
class BlameNotes : public KTextEditor::InlineNoteProvider
{
public:
    int line() const
    {
        return m_line;
    }
    void set(int line, const QString &text)
    {
        if ((text.isEmpty() ? -1 : line) == m_line && text == m_text) {
            return;
        }
        const int old = m_line;
        m_line = text.isEmpty() ? -1 : line;
        m_text = text;
        if (old >= 0) {
            Q_EMIT inlineNotesChanged(old);
        }
        if (m_line >= 0 && m_line != old) {
            Q_EMIT inlineNotesChanged(m_line);
        }
    }
    QList<int> inlineNotes(int line) const override
    {
        if (line != m_line || !m_doc || MergeConflicts::hasNote(m_doc, line)) { // a conflict marker line has its own note
            return {};
        }
        return {m_doc->lineLength(line)};
    }
    QSize inlineNoteSize(const KTextEditor::InlineNote &note) const override
    {
        QFont font = note.font();
        font.setItalic(true);
        const QFontMetrics fm(font);
        return QSize(gap(fm) + fm.horizontalAdvance(m_text), note.lineHeight());
    }
    void paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter, Qt::LayoutDirection) const override
    {
        QFont font = note.font();
        font.setItalic(true);
        const QFontMetrics fm(font);
        painter.setFont(font);
        const Colors &c = Theme::colors();
        painter.setPen(c.fg.isValid() ? QColor::fromRgbF(c.fg.redF() * 0.45f + c.editor.redF() * 0.55f,
                                                         c.fg.greenF() * 0.45f + c.editor.greenF() * 0.55f,
                                                         c.fg.blueF() * 0.45f + c.editor.blueF() * 0.55f)
                                      : QColor(Qt::gray));
        painter.drawText(QPoint(gap(fm), (note.lineHeight() - fm.height()) / 2 + fm.ascent()), m_text);
    }
    KTextEditor::Document *m_doc = nullptr;

private:
    static int gap(const QFontMetrics &fm)
    {
        return fm.horizontalAdvance(QStringLiteral("    "));
    }
    int m_line = -1;
    QString m_text;
};

QLabel *dimLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

// VS Code style shortcuts for the editor component, by KTextEditor action
// name. An empty string removes the default shortcut.
struct KeyRemap {
    const char *action;
    const char *keys;
};
constexpr KeyRemap kVsCodeKeymap[] = {
    {"tools_comment", ""}, // Ctrl+D is "add next occurrence" in VS Code
    {"tools_uncomment", ""},
    {"edit_find_multicursor_next_occurrence", "Ctrl+D"},
    {"edit_find_multicursor_all_occurrences", "Ctrl+Shift+L"},
    {"delete_line", "Ctrl+Shift+K"},
    {"tools_scripts_moveLinesUp", "Alt+Up"},
    {"tools_scripts_moveLinesDown", "Alt+Down"},
    {"tools_scripts_duplicateLinesUp", "Alt+Shift+Up"},
    {"tools_scripts_duplicateLinesDown", "Alt+Shift+Down"},
    {"tools_scripts_jumpIndentUp", ""},
    {"tools_scripts_jumpIndentDown", ""},
    {"edit_replace", "Ctrl+H"},
    {"edit_find_selected", ""},
    {"edit_redo", "Ctrl+Shift+Z; Ctrl+Y"},
    {"newline_below", "Ctrl+Return; Ctrl+Enter"},
    {"newline_above", "Ctrl+Shift+Return; Ctrl+Shift+Enter"},
    {"no_indent_newline", ""},
    {"tools_indent", "Ctrl+]; Ctrl+I"},
    {"tools_unindent", "Ctrl+["},
    {"to_matching_bracket", "Ctrl+Shift+\\"},
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // From the built-in resource, not the theme: on Wayland Qt then sends the
    // picture itself to KWin (xdg-toplevel-icon) instead of just a name.
    setWindowIcon(QIcon(QStringLiteral(":/lieve.svg")));
    buildUi();
    buildActions();
    if (config().value(QStringLiteral("ui/titleBar"), QStringLiteral("lieve")).toString().trimmed() == QLatin1String("system") || !Theme::styled()) {
        m_titleBar->setWindowButtonsVisible(false);
    } else {
        setWindowFlag(Qt::FramelessWindowHint, true);
        // Plasma rounds the corners of the windows it frames; without its frame
        // Lieve draws its own rounded corners on a see-through window.
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

    // Defaults for a new window: last window size and panel sizes.
    QSettings &s = state();
    if (!restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray())) {
        resize(1280, 820);
    }
    m_sideWidth = qMax(170, s.value(QStringLiteral("window/sideWidth"), 260).toInt());
    m_terminalHeight = qMax(100, s.value(QStringLiteral("window/terminalHeight"), 260).toInt());
    m_side->hide();
    syncActivityBar();
    updateEditorPage();

    m_branchTimer.setSingleShot(true);
    m_branchTimer.setInterval(150);
    connect(&m_branchTimer, &QTimer::timeout, this, &MainWindow::updateBranch);
    connect(&m_gitWatcher, &QFileSystemWatcher::directoryChanged, &m_branchTimer, qOverload<>(&QTimer::start));

    // Hot exit: started by edits, so it never runs while nothing is unsaved.
    m_backupTimer.setSingleShot(true);
    m_backupTimer.setInterval(1500);
    connect(&m_backupTimer, &QTimer::timeout, this, [this] {
        bool added = false;
        for (KTextEditor::Document *doc : documents()) {
            const bool had = doc->property("lieveBackup").isValid();
            backupDocument(doc);
            added = added || (!had && doc->property("lieveBackup").isValid());
        }
        if (added) {
            saveSession(); // lists the new backup with the tabs, in case Lieve crashes
        }
    });
}

MainWindow::~MainWindow()
{
    // Language servers stop, and their underlines go while the documents live.
    delete m_lsp;
    m_lsp = nullptr;
    // The documents die after this body: their signals must not reach the
    // window any more (nor drop the hot exit backups).
    for (KTextEditor::Document *doc : documents()) {
        doc->disconnect(this);
    }
    // The editor views are child widgets and die after this body: detach the
    // inline blame providers while views and window are both still whole.
    for (auto it = m_blameNotes.begin(); it != m_blameNotes.end(); ++it) {
        it.key()->disconnect(this);
        it.key()->unregisterInlineNoteProvider(it.value());
        delete it.value();
    }
    m_blameNotes.clear();
    for (DocumentBlame *blame : std::as_const(m_blames)) {
        blame->disconnect(this);
    }
}

// ---------------------------------------------------------------------------
// UI

void MainWindow::buildUi()
{
    // Window = title bar on top, then a body where side bar, editor and
    // terminal are rounded cards separated by small gaps.
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("central"));
    central->setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_titleBar = new TitleBar(central);
    outer->addWidget(m_titleBar);
    m_body = new QWidget(central);
    m_body->setMouseTracking(true); // the gaps around the cards resize the window
    m_body->installEventFilter(this);
    outer->addWidget(m_body, 1);
    m_status = new StatusBar(central);
    m_status->addItem(QStringLiteral("branch"), false);
    m_status->addItem(QStringLiteral("sync"), false);
    m_status->addItem(QStringLiteral("problems"), false);
    for (const char *name : {"blame", "cursor", "indent", "encoding", "eol", "mode", "eslint", "prettier", "update"}) {
        m_status->addItem(QLatin1String(name), true);
    }
    m_status->item(QStringLiteral("update"))->setObjectName(QStringLiteral("statusAccent"));
    outer->addWidget(m_status);
    connect(&Updater::instance(), &Updater::stateChanged, this, &MainWindow::updateUpdaterStatus);
    connect(m_status, &StatusBar::itemClicked, this, &MainWindow::onStatusClicked);
    auto *layout = new QHBoxLayout(m_body);
    layout->setContentsMargins(4, 0, 6, 2);
    layout->setSpacing(2);

    // Activity bar (left): explorer, search, git
    m_activityBar = new QToolBar(central);
    m_activityBar->setObjectName(QStringLiteral("activityBar"));
    m_activityBar->setOrientation(Qt::Vertical);
    m_activityBar->setMovable(false);
    m_activityBar->setFloatable(false);
    m_activityBar->setIconSize(QSize(24, 24));
    m_activityBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_activityBar->setContextMenuPolicy(Qt::PreventContextMenu);
    m_actExplorer = m_activityBar->addAction(Theme::activityIcon(themeIcon({"folder-open", "document-open-folder", "folder"})),
                                             tr("Explorer (Ctrl+Shift+E)"));
    m_actSearch = m_activityBar->addAction(Theme::activityIcon(themeIcon({"edit-find", "search", "system-search"})), tr("Search (Ctrl+Shift+F)"));
    m_gitIcon = themeIcon({"vcs-branch", "vcs-commit", "git"});
    m_actGit = m_activityBar->addAction(Theme::activityIcon(m_gitIcon), tr("Source Control (Ctrl+Shift+G)"));
    // Layout toggles live in the title bar, like VS Code's.
    m_actSidebar = new QAction(Theme::glyph(Theme::Glyph::Sidebar), tr("Side Bar (Ctrl+B)"), this);
    m_actTerminal = new QAction(Theme::glyph(Theme::Glyph::Panel), tr("Terminal (Ctrl+J)"), this);
    for (QAction *a : {m_actExplorer, m_actSearch, m_actGit, m_actTerminal, m_actSidebar}) {
        a->setCheckable(true);
    }
    m_mainMenu = new QMenu(this);
    m_titleBar->setMenu(m_mainMenu);
    m_titleBar->addToggle(m_actSidebar);
    m_titleBar->addToggle(m_actTerminal);
    connect(m_titleBar, &TitleBar::searchClicked, this, [this] {
        if (m_root.isEmpty()) {
            focusChooser();
        } else {
            m_quickOpen->popup(m_mru);
        }
    });
    for (QToolButton *button : m_activityBar->findChildren<QToolButton *>()) {
        button->setFocusPolicy(Qt::NoFocus); // no focus frame on the first icon
    }
    layout->addWidget(m_activityBar);

    m_mainSplit = new QSplitter(Qt::Horizontal, central);
    m_mainSplit->setChildrenCollapsible(false);
    m_mainSplit->setHandleWidth(6);

    m_side = new QStackedWidget(m_mainSplit);
    m_side->setObjectName(QStringLiteral("sideStack"));
    m_side->setMinimumWidth(170);
    m_explorer = new Explorer(m_side);
    m_search = new SearchPanel(m_side);
    m_git = new Lieve::GitRepo(this);
    m_staging = new GitStaging(m_git, this);
    m_gitPanel = new GitPanel(m_git, m_side);
    m_side->addWidget(m_explorer);
    m_side->addWidget(m_search);
    m_side->addWidget(m_gitPanel);

    m_rightSplit = new QSplitter(Qt::Vertical, m_mainSplit);
    m_rightSplit->setChildrenCollapsible(false);
    m_rightSplit->setHandleWidth(6);
    m_editorStack = new QStackedWidget(m_rightSplit);
    m_editorStack->setObjectName(QStringLiteral("editorCard"));
    m_editorStack->setMinimumHeight(120);
    m_chooserPage = buildChooserPage();
    m_emptyPage = buildEmptyPage();
    m_editorStack->addWidget(m_chooserPage);
    m_editorStack->addWidget(m_emptyPage);
    m_editorSplit = new QSplitter(Qt::Horizontal, m_editorStack);
    m_editorSplit->setObjectName(QStringLiteral("editorSplit"));
    m_editorSplit->setChildrenCollapsible(false);
    m_editorSplit->setHandleWidth(1);
    m_editorStack->addWidget(m_editorSplit);
    m_tabs = createGroup();
    m_terminal = new TerminalPanel(m_rightSplit);
    m_terminal->setObjectName(QStringLiteral("terminalCard"));
    m_terminal->setMinimumHeight(80);
    m_terminal->hide();
    m_rightSplit->addWidget(m_editorStack);
    m_rightSplit->addWidget(m_terminal);
    m_rightSplit->setStretchFactor(0, 1);
    m_rightSplit->setStretchFactor(1, 0);

    m_mainSplit->addWidget(m_side);
    m_mainSplit->addWidget(m_rightSplit);
    m_mainSplit->setStretchFactor(0, 0);
    m_mainSplit->setStretchFactor(1, 1);
    layout->addWidget(m_mainSplit, 1);
    setCentralWidget(central);
    Theme::makeCard(m_side);
    Theme::makeCard(m_editorStack);
    Theme::makeCard(m_terminal);
    central->installEventFilter(this); // keeps overlays placed on resize

    m_quickOpen = new QuickOpen(central);
    m_lsp = new LspManager(this, m_status, central);
    m_outline = new EditorOutline(this, m_lsp);
    for (TabView *group : std::as_const(m_groups)) {
        m_outline->addGroup(group);
    }
    buildSwitcher();

    connect(m_actExplorer, &QAction::triggered, this, [this] {
        showSidePage(ExplorerPage, true);
    });
    connect(m_actSearch, &QAction::triggered, this, [this] {
        showSidePage(SearchPage, true);
        if (!m_side->isHidden()) {
            m_search->focusQuery();
        }
    });
    connect(m_actTerminal, &QAction::triggered, this, [this] {
        setTerminalVisible(m_terminal->isHidden());
    });
    connect(m_actSidebar, &QAction::triggered, this, [this] {
        setSidebarVisible(m_side->isHidden());
    });
    connect(m_actGit, &QAction::triggered, this, [this] {
        showSidePage(GitPage, true);
        if (!m_side->isHidden()) {
            m_gitPanel->focusMessage();
        }
    });

    // Git: the panel asks, the window shows diffs and files
    connect(m_gitPanel, &GitPanel::openFileRequested, this, [this](const QString &path) {
        openFile(path);
    });
    connect(m_gitPanel, &GitPanel::diffRequested, this, [this](const Lieve::GitEntry &entry) {
        openChanges(entry, -1); // side by side, or the unified diff
    });
    connect(m_gitPanel, &GitPanel::commitDiffRequested, this, &MainWindow::showDiffTab);
    m_gitPanel->setFileSource([this] {
        KTextEditor::View *view = currentView();
        return view ? localPath(view->document()) : QString();
    });
    connect(m_git, &Lieve::GitRepo::changed, this, &MainWindow::updateGitBadge);
    connect(m_git, &Lieve::GitRepo::changed, this, &MainWindow::updateGitStatus);
    // A commit, checkout or pull (also from the terminal) moves HEAD: blame again.
    connect(m_git, &Lieve::GitRepo::changed, this, [this] {
        if (m_git->head() == m_blameHead) {
            return;
        }
        const bool first = m_blameHead.isEmpty();
        m_blameHead = m_git->head();
        if (first) {
            return;
        }
        for (auto it = m_blames.cbegin(); it != m_blames.cend(); ++it) {
            m_staleBlames.insert(it.key());
        }
        showBlame();
    });
    // Change marks: git runs again only for files whose index entry changed.
    connect(m_git, &Lieve::GitRepo::changed, this, [this] {
        for (auto it = m_gutters.begin(); it != m_gutters.end();) {
            if (it.value().isNull()) { // the document was closed
                it = m_gutters.erase(it);
            } else {
                updateGutter(it.key());
                ++it;
            }
        }
    });
    connect(&m_gitWatcher, &QFileSystemWatcher::directoryChanged, m_git, &Lieve::GitRepo::scheduleRefresh);
    m_gitPoll.setInterval(10000); // catches edits made by other programs
    connect(&m_gitPoll, &QTimer::timeout, this, [this] {
        if (isActiveWindow()) {
            m_git->scheduleRefresh();
        }
    });
    m_gitPoll.start();

    connect(m_explorer, &Explorer::openFileRequested, this, [this](const QString &path) {
        openFile(path);
    });
    connect(m_explorer, &Explorer::openFolderRequested, this, [this] {
        WindowManager::instance().openFolderDialog(this);
    });
    connect(m_explorer, &Explorer::terminalRequested, this, [this](const QString &dir) {
        newTerminal(dir);
    });
    connect(m_explorer, &Explorer::pathRenamed, this, &MainWindow::onPathRenamed);
    connect(m_explorer, &Explorer::pathRemoved, this, &MainWindow::onPathRemoved);
    connect(m_explorer, &Explorer::pathRenamed, m_git, &Lieve::GitRepo::scheduleRefresh);
    connect(m_explorer, &Explorer::pathRemoved, m_git, &Lieve::GitRepo::scheduleRefresh);
    // NPM SCRIPTS: each script runs in a terminal of the panel.
    m_explorer->npmScripts()->setRunner([this](const QString &dir, const QString &title, const QString &command, QWidget *reuse) {
        QWidget *terminal = m_terminal->runCommand(dir, title, command, reuse);
        setTerminalVisible(true);
        return terminal;
    });
    connect(m_explorer->npmScripts(), &NpmScripts::openRequested, this, [this](const QString &path, int line, int column, int length) {
        openFile(path, line, column, length);
    });
    connect(m_search, &SearchPanel::matchActivated, this, [this](const QString &path, int line, int column, int length) {
        openFile(path, line, column, length);
    });
    // Replace in files: open files are changed in their document.
    m_search->setDocumentProvider([this](const QString &path) -> KTextEditor::Document * {
        KTextEditor::View *view = findView(path);
        return view ? view->document() : nullptr;
    });
    connect(m_search, &SearchPanel::filesWritten, m_git, &Lieve::GitRepo::scheduleRefresh);
    connect(m_quickOpen, &QuickOpen::fileChosen, this, [this](const QString &path, int line) {
        openFile(path, line);
    });
    connect(m_terminal, &TerminalPanel::hideRequested, this, [this] {
        setTerminalVisible(false);
    });
    connect(m_terminal, &TerminalPanel::lastTerminalClosed, this, [this] {
        setTerminalVisible(false);
    });

    connect(m_mainSplit, &QSplitter::splitterMoved, this, [this] {
        if (!m_side->isHidden()) {
            m_sideWidth = m_mainSplit->sizes().value(0, m_sideWidth);
        }
    });
    connect(m_rightSplit, &QSplitter::splitterMoved, this, [this] {
        if (!m_terminal->isHidden()) {
            m_terminalHeight = m_rightSplit->sizes().value(1, m_terminalHeight);
        }
    });
}

// Shown in windows without a project: pick one (Enter reopens the last one).
QWidget *MainWindow::buildChooserPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->addStretch(1);

    auto *column = new QWidget(page);
    column->setMaximumWidth(780);
    auto *v = new QVBoxLayout(column);
    v->setContentsMargins(0, 0, 0, 0);
    v->addSpacing(36);

    auto *header = new QHBoxLayout;
    auto *logo = new QLabel(column);
    logo->setPixmap(windowIcon().pixmap(52, 52));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto *title = new QLabel(QStringLiteral("Lieve"), column);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.8);
    title->setFont(titleFont);
    titles->addWidget(title);
    titles->addWidget(dimLabel(tr("Choose a project"), column));
    header->addWidget(logo);
    header->addSpacing(8);
    header->addLayout(titles);
    header->addStretch(1);
    v->addLayout(header);
    v->addSpacing(14);

    auto *frame = new QFrame(column);
    frame->setObjectName(QStringLiteral("chooserFrame"));
    frame->setFrameShape(QFrame::StyledPanel);
    auto *fl = new QVBoxLayout(frame);
    fl->setContentsMargins(8, 8, 8, 8);
    m_chooser = new ProjectList(frame);
    fl->addWidget(m_chooser);
    v->addWidget(frame, 1);
    v->addSpacing(8);

    auto *buttons = new QHBoxLayout;
    auto *openFolder = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open Folder…"), column);
    openFolder->setObjectName(QStringLiteral("secondaryButton"));
    buttons->addWidget(openFolder);
    buttons->addSpacing(8);
    const QStringList roots = Projects::roots();
    QStringList prettyRoots;
    for (const QString &r : roots) {
        prettyRoots << prettyPath(r);
    }
    auto *hint = dimLabel(tr("Enter to open · Ctrl+Enter to open in a new window · projects are searched in %1 (Ctrl+, to change)")
                              .arg(prettyRoots.isEmpty() ? QStringLiteral("—") : prettyRoots.join(QStringLiteral(", "))),
                          column);
    hint->setWordWrap(true);
    buttons->addWidget(hint, 1);
    v->addLayout(buttons);
    v->addSpacing(36);

    outer->addWidget(column, 4);
    outer->addStretch(1);

    connect(openFolder, &QPushButton::clicked, this, [this] {
        WindowManager::instance().openFolderDialog(this);
    });
    connect(m_chooser, &ProjectList::projectChosen, this, [this](const QString &path, bool newWindow) {
        WindowManager::instance().openProject(path, newWindow ? nullptr : this);
    });
    connect(m_chooser, &ProjectList::cancelled, this, [this] {
        if (WindowManager::instance().windows().size() > 1) {
            close(); // an extra empty window: Esc dismisses it
        }
    });
    return page;
}

// Shown in a project window when no file is open.
QWidget *MainWindow::buildEmptyPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->addStretch(2);
    auto *logo = new QLabel(page);
    logo->setPixmap(windowIcon().pixmap(72, 72));
    logo->setAlignment(Qt::AlignCenter);
    outer->addWidget(logo);
    outer->addSpacing(20);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(8);
    const QList<QPair<QString, QString>> rows = {
        {tr("Go to File"), QStringLiteral("Ctrl+P")},
        {tr("Find in Files"), QStringLiteral("Ctrl+Shift+F")},
        {tr("Explorer"), QStringLiteral("Ctrl+Shift+E")},
        {tr("Toggle Terminal"), QStringLiteral("Ctrl+J")},
        {tr("Switch Project"), QStringLiteral("Ctrl+R")},
        {tr("Settings"), QStringLiteral("Ctrl+,")},
    };
    int row = 0;
    for (const auto &[label, keys] : rows) {
        auto *l = dimLabel(label, page);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *k = dimLabel(keys, page);
        QFont keyFont = k->font();
        keyFont.setBold(true);
        k->setFont(keyFont);
        grid->addWidget(l, row, 0);
        grid->addWidget(k, row, 1);
        ++row;
    }
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch(1);
    gridRow->addLayout(grid);
    gridRow->addStretch(1);
    outer->addLayout(gridRow);
    outer->addStretch(3);
    return page;
}

// Ctrl+R: overlay to switch project without leaving the window.
void MainWindow::buildSwitcher()
{
    m_switcher = new QFrame(centralWidget());
    m_switcher->setObjectName(QStringLiteral("overlay"));
    m_switcher->setFrameShape(QFrame::StyledPanel);
    auto *shadow = new QGraphicsDropShadowEffect(m_switcher);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 140));
    m_switcher->setGraphicsEffect(shadow);
    auto *layout = new QVBoxLayout(m_switcher);
    layout->setContentsMargins(6, 6, 6, 6);
    m_switcherList = new ProjectList(m_switcher);
    m_switcherList->setPlaceholderText(tr("Search projects… (Enter: open · Ctrl+Enter: replace in this window)"));
    layout->addWidget(m_switcherList);
    m_switcher->hide();

    connect(m_switcherList, &ProjectList::cancelled, m_switcher, &QWidget::hide);
    connect(m_switcherList, &ProjectList::projectChosen, this, [this](const QString &path, bool replace) {
        m_switcher->hide();
        WindowManager &wm = WindowManager::instance();
        if (!replace || wm.windowFor(path) || path == m_root) {
            wm.openProject(path, nullptr); // its own window (or the one already showing it)
        } else if (closeProject()) {
            wm.openProject(path, this);
        }
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (m_switcher->isVisible() && now && !m_switcher->isAncestorOf(now) && now->window() == this) {
            m_switcher->hide();
        }
    });
}

void MainWindow::placeSwitcher()
{
    QWidget *p = centralWidget();
    const int width = std::min(720, p->width() - 40);
    const int height = std::min(480, p->height() - 40);
    m_switcher->setGeometry((p->width() - width) / 2, 8, width, height);
}

void MainWindow::showProjectSwitcher()
{
    if (m_switcher->isVisible()) {
        m_switcher->hide();
        return;
    }
    m_switcherList->setCurrentProject(m_root);
    m_switcherList->clearFilter();
    m_switcherList->reload();
    placeSwitcher();
    m_switcher->show();
    m_switcher->raise();
    m_switcherList->focusFilter();
}

void MainWindow::focusChooser()
{
    if (m_root.isEmpty()) {
        m_chooser->reload();
        m_chooser->focusFilter();
    }
}

QAction *MainWindow::addShortcut(const QString &text, const QList<QKeySequence> &keys, bool keepInTerminal, const std::function<void()> &slot)
{
    auto *action = new QAction(text, this);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, [slot] {
        slot();
    });
    addAction(action);
    m_appKeys << keys;
    if (keepInTerminal) {
        m_terminalKeys << keys;
    }
    return action;
}

void MainWindow::buildActions()
{
    QAction *quickOpenAction = addShortcut(tr("Quick Open"), {QKeySequence(Qt::CTRL | Qt::Key_P), QKeySequence(Qt::CTRL | Qt::Key_E)}, true, [this] {
        if (!m_root.isEmpty()) {
            m_quickOpen->popup(m_mru);
        }
    });
    QAction *paletteAction = addShortcut(tr("Command Palette…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), QKeySequence(Qt::Key_F1)}, true, [this] {
        m_quickOpen->popup(m_mru, QStringLiteral(">"));
    });
    QAction *goToLineAction = addShortcut(tr("Go to Line/Column…"), {QKeySequence(Qt::CTRL | Qt::Key_G)}, false, [this] {
        m_quickOpen->popup(m_mru, QStringLiteral(":"));
    });
    QAction *explorerAction = addShortcut(tr("Explorer"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)}, true, [this] {
        showSidePage(ExplorerPage, false);
        m_explorer->focusTree();
    });
    QAction *findInFilesAction = addShortcut(tr("Find in Files"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)}, true, [this] {
        showSearch(selectedText());
    });
    addShortcut(tr("Replace in Files"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H)}, true, [this] {
        showSearch(selectedText(), true);
    });
    QAction *sourceControlAction = addShortcut(tr("Source Control"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)}, true, [this] {
        showSidePage(GitPage, false);
        m_gitPanel->focusMessage();
    });
    QAction *sideBarAction = addShortcut(tr("Toggle Side Bar"), {QKeySequence(Qt::CTRL | Qt::Key_B)}, false, [this] {
        setSidebarVisible(m_side->isHidden());
    });
    QAction *panelAction = addShortcut(tr("Toggle Panel"), {QKeySequence(Qt::CTRL | Qt::Key_J)}, true, [this] {
        setTerminalVisible(m_terminal->isHidden());
    });
    QAction *problemsAction = addShortcut(tr("Show Problems"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)}, true, [this] {
        toggleProblems(true);
    });
    QAction *terminalAction = addShortcut(tr("Terminal"),
                                          {QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft), QKeySequence(Qt::CTRL | Qt::Key_Ograve)},
                                          true,
                                          [this] {
                                              toggleTerminalFocus();
                                          });
    QAction *projects = addShortcut(tr("Projects…"), {QKeySequence(Qt::CTRL | Qt::Key_R)}, false, [this] {
        if (m_root.isEmpty()) {
            focusChooser();
        } else {
            showProjectSwitcher();
        }
    });
    QAction *openFolderAction = addShortcut(tr("Open Folder…"), {QKeySequence(QStringLiteral("Ctrl+K, Ctrl+O"))}, false, [this] {
        WindowManager::instance().openFolderDialog(this);
    });
    QAction *newWindow = addShortcut(tr("New Window"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)}, false, [] {
        WindowManager::instance().showChooser();
    });
    QAction *newTerm = addShortcut(tr("New Terminal"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_QuoteLeft)}, true, [this] {
        newTerminal();
    });
    QAction *newFile = addShortcut(tr("New File"), {QKeySequence(Qt::CTRL | Qt::Key_N)}, false, [this] {
        newUntitled();
    });
    QAction *openFileAction = addShortcut(tr("Open File…"), {QKeySequence(Qt::CTRL | Qt::Key_O)}, false, [this] {
        openFileDialog();
    });
    QAction *saveAllAction = addShortcut(tr("Save All"), {QKeySequence(QStringLiteral("Ctrl+K, S"))}, false, [this] {
        saveAll();
    });
    QAction *closeEditorAction = addShortcut(tr("Close Editor"), {QKeySequence(Qt::CTRL | Qt::Key_W), QKeySequence(Qt::CTRL | Qt::Key_F4)}, false, [this] {
        closeTab(m_tabs->currentWidget()); // also a preview
    });
    QAction *nextEditorAction = addShortcut(tr("Next Editor"), {QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)}, false, [this] {
        cycleTab(1);
    });
    QAction *previousEditorAction = addShortcut(tr("Previous Editor"),
                                                {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab),
                                                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                                                 QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
                                                false,
                                                [this] {
                                                    cycleTab(-1);
                                                });
    // Editor groups side by side, like VS Code.
    QAction *splitAction = addShortcut(tr("Split Editor"), {QKeySequence(Qt::CTRL | Qt::Key_Backslash)}, false, [this] {
        splitEditor();
    });
    QAction *focusFirst = addShortcut(tr("Focus First Editor Group"), {QKeySequence(Qt::CTRL | Qt::Key_1)}, false, [this] {
        focusGroup(0);
    });
    QAction *focusSecond = addShortcut(tr("Focus Second Editor Group"), {QKeySequence(Qt::CTRL | Qt::Key_2)}, false, [this] {
        focusGroup(1);
    });
    QAction *focusThird = addShortcut(tr("Focus Third Editor Group"), {QKeySequence(Qt::CTRL | Qt::Key_3)}, false, [this] {
        focusGroup(2);
    });
    // Alt+F5 too, VS Code's key in diffs.
    addShortcut(tr("Go to Next Change"), {QKeySequence(Qt::ALT | Qt::Key_F3), QKeySequence(Qt::ALT | Qt::Key_F5)}, false, [this] {
        goToChange(true);
    });
    addShortcut(tr("Go to Previous Change"), {QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F3), QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F5)}, false, [this] {
        goToChange(false);
    });
    // Part of a file's changes into or out of the index, with VS Code's keys.
    const std::pair<GitStaging::Command, const char *> stagingCommands[] = {
        {GitStaging::Stage, "Ctrl+K, Ctrl+Alt+S"},
        {GitStaging::Unstage, "Ctrl+K, Ctrl+N"},
        {GitStaging::Revert, "Ctrl+K, Ctrl+R"},
    };
    for (const auto &[command, keys] : stagingCommands) {
        QAction *action = addShortcut(GitStaging::commandText(command), {QKeySequence(QLatin1String(keys))}, false, [this, command] {
            runStagingCommand(command);
        });
        action->setIcon(GitStaging::commandIcon(command));
        setCommandCategory(action, CommandCategory::Git);
        action->setVisible(false); // in the palette only in a repository
        connect(m_git, &Lieve::GitRepo::changed, action, [this, action] {
            action->setVisible(m_git->isRepository());
        });
    }
    QAction *formatAction = addShortcut(tr("Format Document"),
                                        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F)},
                                        false,
                                        [this] {
                                            if (KTextEditor::View *view = currentView()) {
                                                formatDocument(view->document(), false);
                                            }
                                        });
    QAction *saveRaw = addShortcut(tr("Save without Formatting"), {QKeySequence(QStringLiteral("Ctrl+K, Ctrl+Shift+S"))}, false, [this] {
        if (KTextEditor::View *view = currentView()) {
            m_saveWithoutFormat = view->document();
            view->document()->documentSave();
            m_saveWithoutFormat = nullptr;
        }
    });
    QAction *closeProjectAction = addShortcut(tr("Close Project"), {QKeySequence(QStringLiteral("Ctrl+K, F"))}, false, [this] {
        closeProject();
    });
    QAction *closeWindow = addShortcut(tr("Close Window"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)}, false, [this] {
        close();
    });
    QAction *settingsAction = addShortcut(tr("Settings"), {QKeySequence(Qt::CTRL | Qt::Key_Comma)}, false, [this] {
        openSettings();
    });
    QAction *quitAction = addShortcut(tr("Exit"), {QKeySequence(Qt::CTRL | Qt::Key_Q)}, false, [] {
        WindowManager::instance().closeAll();
    });
    // package.json scripts (the Explorer's NPM SCRIPTS), listed only for projects that have them
    QAction *runScriptAction = addShortcut(tr("Run npm Script…"), {}, false, [this] {
        m_quickOpen->popupChoices(m_explorer->npmScripts()->choices(), tr("Select the npm script to run"));
    });
    QAction *rerunScriptAction = addShortcut(tr("Rerun Last Script"), {}, false, [this] {
        m_explorer->npmScripts()->rerunLast();
    });
    runScriptAction->setObjectName(QStringLiteral("actionRunNpmScript"));
    rerunScriptAction->setObjectName(QStringLiteral("actionRerunNpmScript"));
    setCommandCategory(runScriptAction, NpmScripts::Category);
    setCommandCategory(rerunScriptAction, NpmScripts::Category);
    m_explorer->npmScripts()->bindActions(runScriptAction, rerunScriptAction);
    // Go to Definition (F12), Back (Alt+Left), Go to Symbol (Ctrl+Shift+O), problems (F8)
    const QList<LspManager::Shortcut> lspShortcuts = m_lsp->shortcuts();
    for (const LspManager::Shortcut &shortcut : lspShortcuts) {
        addShortcut(shortcut.text, shortcut.keys, false, shortcut.slot);
    }
    // Markdown preview, like VS Code's (enabled on Markdown files, see updateMarkdownActions()).
    m_previewAction = addShortcut(tr("Open Preview"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)}, false, [this] {
        openMarkdownPreview(false);
    });
    m_previewSideAction = addShortcut(tr("Open Preview to the Side"), {QKeySequence(QStringLiteral("Ctrl+K, V"))}, false, [this] {
        openMarkdownPreview(true);
    });
    for (QAction *a : {m_previewAction, m_previewSideAction}) {
        a->setIcon(MarkdownPreview::icon());
        setCommandCategory(a, "Markdown"); // "Markdown: Open Preview", as in VS Code
    }
    updateMarkdownActions();

    quickOpenAction->setObjectName(QStringLiteral("actionQuickOpen"));
    sourceControlAction->setObjectName(QStringLiteral("actionSourceControl"));
    problemsAction->setObjectName(QStringLiteral("actionProblems"));
    projects->setIcon(QIcon::fromTheme(QStringLiteral("view-list-details")));
    openFolderAction->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
    newWindow->setIcon(QIcon::fromTheme(QStringLiteral("window-new")));
    newFile->setIcon(QIcon::fromTheme(QStringLiteral("document-new")));
    openFileAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    saveAllAction->setIcon(QIcon::fromTheme(QStringLiteral("document-save-all")));
    formatAction->setIcon(QIcon::fromTheme(QStringLiteral("format-indent-more")));
    newTerm->setIcon(QIcon::fromTheme(QStringLiteral("utilities-terminal")));
    closeProjectAction->setIcon(QIcon::fromTheme(QStringLiteral("folder-close"), QIcon::fromTheme(QStringLiteral("project-development-close"))));
    closeWindow->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    settingsAction->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
    quitAction->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));

    // Command palette prefixes, as in VS Code ("View: Toggle Panel").
    for (QAction *a : {explorerAction, sourceControlAction, sideBarAction, panelAction, terminalAction, closeEditorAction, nextEditorAction, previousEditorAction,
                       splitAction, focusFirst, focusSecond, focusThird, problemsAction}) {
        setCommandCategory(a, CommandCategory::View);
    }
    for (QAction *a : {projects, openFolderAction, newWindow, newFile, openFileAction, saveAllAction, saveRaw, closeProjectAction, quitAction}) {
        setCommandCategory(a, CommandCategory::File);
    }
    setCommandCategory(findInFilesAction, CommandCategory::Search);
    setCommandCategory(newTerm, CommandCategory::Terminal);
    setCommandCategory(settingsAction, CommandCategory::Preferences);

    m_mainMenu->addAction(paletteAction);
    m_mainMenu->addSeparator();
    m_mainMenu->addAction(projects);
    m_mainMenu->addAction(openFolderAction);
    m_mainMenu->addAction(newWindow);
    m_mainMenu->addSeparator();
    m_mainMenu->addAction(newFile);
    m_mainMenu->addAction(openFileAction);
    m_mainMenu->addAction(saveAllAction);
    m_mainMenu->addAction(saveRaw);
    m_mainMenu->addAction(formatAction);
    m_mainMenu->addSeparator();
    m_mainMenu->addAction(newTerm);
    m_mainMenu->addSeparator();
    m_mainMenu->addAction(closeProjectAction);
    m_mainMenu->addAction(closeWindow);
    m_mainMenu->addSeparator();
    m_mainMenu->addAction(settingsAction);
    m_mainMenu->addAction(quitAction);
    connect(m_mainMenu, &QMenu::aboutToShow, this, [this, closeProjectAction] {
        closeProjectAction->setEnabled(!m_root.isEmpty());
    });

    // "Git: Checkout to…" and the history run from the palette without showing the Git panel.
    addActions(m_gitPanel->windowActions());

    // Command palette: everything above, the Git panel's menus and the
    // current editor's own actions, gathered each time it opens.
    m_quickOpen->setCommandSource([this, paletteAction, goToLineAction] {
        QList<Command> commands;
        addMenuCommands(commands, m_mainMenu);
        addCommands(commands, actions());
        if (m_git->isRepository()) {
            const auto menus = m_gitPanel->findChildren<QMenu *>(Qt::FindDirectChildrenOnly);
            for (QMenu *menu : menus) {
                addMenuCommands(commands, menu, CommandCategory::Git, [this] {
                    showSidePage(GitPage, false); // shows progress and asks for the commit message
                });
            }
        }
        KTextEditor::View *view = currentView();
        if (view) {
            addEditorCommands(commands, view);
        }
        commands.removeIf([paletteAction, goToLineAction, view](const Command &c) {
            return c.action == paletteAction || (c.action == goToLineAction && !view);
        });
        return commands;
    });
    m_quickOpen->setEditorSource([this] {
        return currentView();
    });

    m_terminal->setReservedShortcuts(m_terminalKeys);
}

// ---------------------------------------------------------------------------
// Projects

void MainWindow::setProject(const QString &dir)
{
    m_root = dir;
    m_mru.clear();
    m_explorer->setRoot(m_root);
    m_search->setRoot(m_root);
    m_quickOpen->setRoot(m_root);
    m_terminal->setWorkingDirectory(m_root);
    m_git->setDirectory(m_root);
    m_lsp->setRoot(m_root);

    if (!m_gitWatcher.directories().isEmpty()) {
        m_gitWatcher.removePaths(m_gitWatcher.directories());
    }
    const QString git = gitDirFor(m_root);
    if (!git.isEmpty()) {
        m_gitWatcher.addPath(git);
    }
    updateBranch();

    restoreSession(!isVisible());
    updateEditorPage();
    updateWindowTitle();
    Q_EMIT projectChanged();
}

bool MainWindow::closeProject()
{
    if (m_root.isEmpty()) {
        return true;
    }
    if (hotExit()) {
        // The unsaved documents stay in backups: nothing to ask.
        if (!keepUnsaved()) {
            return false;
        }
        closeAllViews();
        m_keepBackups = false;
    } else {
        saveSession();
        if (!closeAllViews()) {
            return false;
        }
    }
    m_terminal->closeAll();
    m_terminal->hide();
    m_root.clear();
    m_mru.clear();
    m_explorer->setRoot(QString());
    m_explorer->setBranch(QString());
    m_search->setRoot(QString());
    m_quickOpen->setRoot(QString());
    m_git->setDirectory(QString());
    m_lsp->setRoot(QString());
    if (!m_gitWatcher.directories().isEmpty()) {
        m_gitWatcher.removePaths(m_gitWatcher.directories());
    }
    m_side->hide();
    syncActivityBar();
    updateEditorPage();
    updateWindowTitle();
    Q_EMIT projectChanged();
    focusChooser();
    return true;
}

void MainWindow::updateBranch()
{
    m_explorer->setBranch(gitBranch(m_root));
}

// ---------------------------------------------------------------------------
// Side bar and terminal

void MainWindow::showSidePage(int page, bool toggle)
{
    if (toggle && !m_side->isHidden() && m_side->currentIndex() == page) {
        setSidebarVisible(false);
        return;
    }
    m_side->setCurrentIndex(page);
    setSidebarVisible(true);
    if (page == ExplorerPage) {
        if (KTextEditor::View *view = currentView()) {
            m_explorer->reveal(localPath(view->document()));
        }
    }
}

void MainWindow::setSidebarVisible(bool visible)
{
    if (visible == m_side->isHidden()) {
        if (visible) {
            m_side->show();
            const int total = m_mainSplit->width();
            m_mainSplit->setSizes({m_sideWidth, qMax(200, total - m_sideWidth)});
        } else {
            m_sideWidth = m_mainSplit->sizes().value(0, m_sideWidth);
            m_side->hide();
            if (KTextEditor::View *view = currentView()) {
                view->setFocus();
            }
        }
    }
    syncActivityBar();
}

void MainWindow::showSearch(const QString &text, bool replace)
{
    showSidePage(SearchPage, false);
    m_search->focusQuery(text, replace);
}

void MainWindow::setTerminalVisible(bool visible)
{
    if (visible) {
        if (m_terminal->isEmpty()) {
            m_terminal->setWorkingDirectory(m_root.isEmpty() ? QDir::homePath() : m_root);
            m_terminal->newTerminal();
        }
        if (m_terminal->isHidden()) {
            m_terminal->show();
            const int total = m_rightSplit->height();
            const int height = qBound(100, m_terminalHeight, qMax(100, total - 140));
            m_rightSplit->setSizes({qMax(140, total - height), height});
        }
        m_terminal->focusCurrent();
    } else {
        if (!m_terminal->isHidden()) {
            m_terminalHeight = m_rightSplit->sizes().value(1, m_terminalHeight);
            m_terminal->hide();
        }
        if (KTextEditor::View *view = currentView()) {
            view->setFocus();
        }
    }
    syncActivityBar();
}

// Like VS Code's Ctrl+`: show and focus the terminal; if it already has
// focus, hide it.
void MainWindow::toggleTerminalFocus()
{
    const bool onTerminal = !m_problems || m_terminal->currentPage() != m_problems;
    if (m_terminal->isHidden()) {
        m_terminal->selectTerminal();
        setTerminalVisible(true);
    } else if (onTerminal && m_terminal->isAncestorOf(QApplication::focusWidget())) {
        setTerminalVisible(false);
    } else {
        m_terminal->selectTerminal();
        m_terminal->focusCurrent();
    }
}

void MainWindow::newTerminal(const QString &dir)
{
    if (!m_terminal->hasTerminals()) {
        m_terminal->setWorkingDirectory(m_root.isEmpty() ? QDir::homePath() : m_root);
    }
    m_terminal->newTerminal(dir);
    setTerminalVisible(true);
}

// Like VS Code: Ctrl+Shift+M (`byFocus`) shows and focuses the Problems tab
// of the bottom panel, or hides the panel if the list has the focus; the
// status bar item shows it, or hides it if it is in front.
void MainWindow::toggleProblems(bool byFocus)
{
    const bool shown = m_problems && !m_terminal->isHidden() && m_terminal->currentPage() == m_problems;
    if (shown && (!byFocus || m_problems->isAncestorOf(QApplication::focusWidget()))) {
        setTerminalVisible(false);
        return;
    }
    if (!m_problems) {
        m_problems = new ProblemsPanel(
            [this] {
                return m_lsp ? m_lsp->problems() : QList<LspManager::Problem>();
            },
            [this] {
                return m_root;
            },
            m_terminal);
        m_problems->hide();
        connect(m_lsp, &LspManager::problemsChanged, m_problems, &ProblemsPanel::refresh);
        connect(m_problems, &ProblemsPanel::problemActivated, this, [this](const QString &path, int line, int column) {
            openFile(path, line, column);
        });
    }
    m_terminal->showPage(m_problems, ProblemsPanel::tabIcon(), tr("Problems"));
    setTerminalVisible(true);
}

void MainWindow::syncActivityBar()
{
    m_actExplorer->setChecked(!m_side->isHidden() && m_side->currentIndex() == ExplorerPage);
    m_actSearch->setChecked(!m_side->isHidden() && m_side->currentIndex() == SearchPage);
    m_actGit->setChecked(!m_side->isHidden() && m_side->currentIndex() == GitPage);
    m_actTerminal->setChecked(!m_terminal->isHidden());
    m_actSidebar->setChecked(!m_side->isHidden());
}

// ---------------------------------------------------------------------------
// Editors

KTextEditor::View *MainWindow::viewAt(int index) const
{
    return qobject_cast<KTextEditor::View *>(m_tabs->widget(index));
}

KTextEditor::View *MainWindow::currentView() const
{
    return qobject_cast<KTextEditor::View *>(m_tabs->currentWidget());
}

// The file's view in the active group, else in another group.
KTextEditor::View *MainWindow::findView(const QString &path) const
{
    KTextEditor::View *other = nullptr;
    for (KTextEditor::View *view : allViews()) {
        if (localPath(view->document()) == path) {
            if (groupOf(view) == m_tabs) {
                return view;
            }
            other = other ? other : view;
        }
    }
    return other;
}

// ---------------------------------------------------------------------------
// Editor groups (VS Code's split editor)

TabView *MainWindow::createGroup()
{
    auto *group = new TabView(m_editorSplit);
    group->tabBar()->installEventFilter(this);
    group->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_editorSplit->addWidget(group);
    m_groups.append(group);
    connect(group, &TabView::tabCloseRequested, this, [this, group](int index) {
        closeTab(group->widget(index));
    });
    connect(group, &TabView::currentChanged, this, [this, group] {
        if (group == m_tabs) {
            onCurrentTabChanged();
        }
    });
    connect(group->tabBar(), &QTabBar::customContextMenuRequested, this, [this, group](const QPoint &pos) {
        setActiveGroup(group);
        showTabMenu(pos);
    });
    addPreviewButton(group);
    DiffView::createCorner(
        group,
        m_git,
        [this](QWidget *page) {
            toggleDiffView(page);
        },
        [this](QWidget *page) {
            const auto *diff = qobject_cast<DiffView *>(page);
            const auto *view = qobject_cast<KTextEditor::View *>(page);
            const QString key = view ? view->document()->property("lieveDiffKey").toString() : QString();
            openFile(diff ? diff->path() : m_git->absolutePath(key.section(QLatin1Char(':'), 1)));
        });
    if (m_outline) {
        m_outline->addGroup(group);
    }
    return group;
}

// An emptied group goes away (the last one stays).
void MainWindow::removeGroup(TabView *group)
{
    const qsizetype at = m_groups.indexOf(group);
    if (at < 0 || m_groups.size() < 2) {
        return;
    }
    m_groups.removeAt(at);
    group->disconnect(this);
    group->hide();
    group->deleteLater(); // may be inside one of its own signals
    if (m_tabs == group) {
        m_tabs = m_groups.value(std::max<qsizetype>(0, at - 1));
        onCurrentTabChanged();
        if (KTextEditor::View *view = currentView()) {
            view->setFocus();
        }
    }
}

void MainWindow::setActiveGroup(TabView *group)
{
    if (group && group != m_tabs && m_groups.contains(group)) {
        m_tabs = group;
        onCurrentTabChanged();
    }
}

TabView *MainWindow::groupOf(KTextEditor::View *view) const
{
    for (TabView *group : m_groups) {
        if (group->indexOf(view) >= 0) {
            return group;
        }
    }
    return nullptr;
}

void MainWindow::activateView(KTextEditor::View *view)
{
    if (TabView *group = groupOf(view)) {
        setActiveGroup(group);
        group->setCurrentWidget(view);
    }
}

QList<KTextEditor::View *> MainWindow::allViews() const
{
    QList<KTextEditor::View *> views;
    for (TabView *group : m_groups) {
        for (int i = 0; i < group->count(); ++i) {
            if (auto *view = qobject_cast<KTextEditor::View *>(group->widget(i))) {
                views << view;
            }
        }
    }
    return views;
}

QList<KTextEditor::Document *> MainWindow::documents() const
{
    QList<KTextEditor::Document *> docs;
    for (KTextEditor::View *view : allViews()) {
        if (!docs.contains(view->document())) {
            docs << view->document();
        }
    }
    return docs;
}

// Ctrl+\: the current file also in the group on the right (made if needed).
void MainWindow::splitEditor()
{
    KTextEditor::View *view = currentView();
    if (!view) {
        return;
    }
    const qsizetype at = m_groups.indexOf(m_tabs);
    TabView *target = at + 1 < m_groups.size() ? m_groups.at(at + 1) : createGroup();
    KTextEditor::View *copy = nullptr;
    for (int i = 0; i < target->count(); ++i) {
        auto *other = qobject_cast<KTextEditor::View *>(target->widget(i));
        if (other && other->document() == view->document()) {
            copy = other;
        }
    }
    setActiveGroup(target);
    if (!copy) {
        copy = addView(view->document(), target);
    }
    target->setCurrentWidget(copy);
    copy->setCursorPosition(view->cursorPosition());
    QList<int> sizes;
    for (qsizetype i = 0; i < m_groups.size(); ++i) {
        sizes << m_editorSplit->width() / int(m_groups.size());
    }
    m_editorSplit->setSizes(sizes);
    copy->setFocus();
}

// Ctrl+1, Ctrl+2...: focus that group; the next one after the last splits.
void MainWindow::focusGroup(int index)
{
    if (index < m_groups.size()) {
        setActiveGroup(m_groups.at(index));
        if (QWidget *page = m_tabs->currentWidget()) { // an editor or a preview
            page->setFocus();
        }
    } else if (index == m_groups.size()) {
        splitEditor();
    }
}

KTextEditor::View *MainWindow::openFile(const QString &path, int line, int column, int selectLength)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return nullptr;
    }
    const QString canonical = info.canonicalFilePath();
    KTextEditor::View *view = findView(canonical);
    if (view && groupOf(view) != m_tabs) {
        view = addView(view->document()); // open in another group: a view of it here too, like VS Code
    }
    if (!view) {
        KTextEditor::Document *doc = takeDiffDocument(canonical); // a side-by-side diff may have it open already
        if (!doc) {
            doc = KTextEditor::Editor::instance()->createDocument(this);
            doc->setConfigValue(QStringLiteral("auto-reload-on-external-changes"), true);
            if (!doc->openUrl(QUrl::fromLocalFile(canonical))) {
                delete doc;
                return nullptr;
            }
        }
        view = addView(doc);
    }
    m_tabs->setCurrentWidget(view);
    updateEditorPage();
    if (line >= 0) {
        const KTextEditor::Cursor cursor(line, column);
        view->setCursorPosition(cursor);
        if (selectLength > 0) {
            view->setSelection(KTextEditor::Range(cursor, KTextEditor::Cursor(line, column + selectLength)));
        }
        // Again once the view is laid out, so it also scrolls there in a window being opened.
        QTimer::singleShot(0, view, [view, cursor] {
            view->setCursorPosition(cursor);
        });
    }
    view->setFocus();
    return view;
}

KTextEditor::View *MainWindow::addView(KTextEditor::Document *doc, TabView *group)
{
    if (!group) {
        group = m_tabs;
    }
    KTextEditor::View *view = doc->createView(group);
    configureView(view);
    m_staging->addView(view); // Stage / Revert Selected Ranges in its context menu
    connect(view, &KTextEditor::View::focusIn, this, [this, view] {
        setActiveGroup(groupOf(view));
    });
    // (not views().size(): a side-by-side diff may show a document before its first tab)
    if (doc->property("lieveSetUp").toBool()) { // already open in another group: the document is set up
        if (GitGutter *gutter = m_gutters.value(doc)) {
            gutter->addView(view);
        }
        m_lsp->addView(view);
        group->insertTab(group->count() == 0 ? 0 : group->currentIndex() + 1, view, QString());
        updateTabTitles();
        updateEditorPage();
        return view;
    }
    doc->setProperty("lieveSetUp", true);
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this] {
        updateTabTitles();
        updateWindowTitle();
    });
    connect(doc, &KTextEditor::Document::documentUrlChanged, this, [this] {
        updateTabTitles();
        updateWindowTitle();
    });
    connect(doc, &KTextEditor::Document::documentNameChanged, this, [this] {
        updateTabTitles();
    });
    // ESLint's fixes and Prettier: just before writing; load them as soon as
    // editing starts so that the first save is fast too.
    connect(doc, &KTextEditor::Document::aboutToSave, this, [this](KTextEditor::Document *d) {
        formatDocument(d, true);
    });
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, m_git, &Lieve::GitRepo::scheduleRefresh);
    // Status bar: language, indentation, encoding, line ends, blame
    connect(doc, &KTextEditor::Document::highlightingModeChanged, this, &MainWindow::updateStatusBar);
    connect(doc, &KTextEditor::Document::configChanged, this, &MainWindow::updateStatusBar);
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, &MainWindow::updateStatusBar);
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this](KTextEditor::Document *d) {
        if (DocumentBlame *blame = m_blames.value(d)) {
            blame->refresh(); // Prettier may have rewritten it; edited lines may match git again
        }
    });
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this](KTextEditor::Document *d) {
        const QString path = localPath(d);
        if (d->isModified() && !path.isEmpty()) {
            Formatter::instance().warmUp(path, d->text(), projectFor(path));
        }
    });
    // Git change marks next to the line numbers
    if (config().value(QStringLiteral("git/gutter"), true).toBool()) {
        GitGutter *gutter = m_gutters.value(doc);
        if (!gutter) {
            gutter = new GitGutter(doc, doc); // goes away with the document
            m_gutters.insert(doc, gutter);
            connect(gutter, &GitGutter::clicked, this, [this, doc](int line) {
                showChange(doc, line);
            });
            connect(gutter, &GitGutter::menuRequested, this, [this, doc](int line, const QPoint &pos) {
                m_staging->showChangeMenu(this, doc, line, pos, [this, doc, line] {
                    showChange(doc, line);
                });
            });
            connect(doc, &KTextEditor::Document::documentUrlChanged, this, &MainWindow::updateGutter);
        }
        gutter->addView(view);
        updateGutter(doc);
    }
    // Merge conflict markers: tinted, with Accept Current/Incoming/Both
    MergeConflicts::watch(doc);
    // Hot exit: unsaved text is backed up shortly after typing stops; the
    // backup goes when the document is saved, reverted or discarded.
    connect(doc, &KTextEditor::Document::textChanged, this, [this](KTextEditor::Document *d) {
        if (!d->isModified() || m_root.isEmpty()) {
            return;
        }
        if (!m_backupTimer.isActive()) {
            m_backupWait.start();
        }
        if (m_backupWait.elapsed() < 5000) { // while typing goes on, still one every few seconds
            m_backupTimer.start();
        }
    });
    connect(doc, &KTextEditor::Document::modifiedChanged, this, [this](KTextEditor::Document *d) {
        const QString path = localPath(d);
        if (!d->isModified()) {
            dropBackup(d);
            d->setProperty("lieveDiskTime", QVariant());
        } else if (!path.isEmpty()) {
            // The version on disk the changes start from.
            d->setProperty("lieveDiskTime", QFileInfo(path).lastModified().toMSecsSinceEpoch());
        }
    });
    m_lsp->addDocument(doc);
    m_lsp->addView(view);
    const int index = group->count() == 0 ? 0 : group->currentIndex() + 1; // next to the current tab, like VS Code
    group->insertTab(index, view, QString());
    updateTabTitles();
    updateEditorPage();
    return view;
}

void MainWindow::configureView(KTextEditor::View *view)
{
    view->setStatusBarEnabled(false); // the window's status bar shows the same information
    if (config().value(QStringLiteral("git/inlineBlame"), true).toBool()) {
        auto *notes = new BlameNotes;
        notes->m_doc = view->document();
        view->registerInlineNoteProvider(notes);
        m_blameNotes.insert(view, notes);
        connect(view, &QObject::destroyed, this, [this, view] {
            delete m_blameNotes.take(view); // the view is gone: nothing left to unregister from
        });
    }
    connect(view, &KTextEditor::View::cursorPositionChanged, this, [this, view](KTextEditor::View *, KTextEditor::Cursor position) {
        if (auto *notes = static_cast<BlameNotes *>(m_blameNotes.value(view)); notes && notes->line() != position.line()) {
            notes->set(-1, QString()); // moved to another line: the old note goes away at once
        }
        if (view == currentView()) {
            updateCursorStatus();
            showBlame();
        }
    });
    connect(view, &KTextEditor::View::selectionChanged, this, [this, view] {
        if (view == currentView()) {
            updateCursorStatus();
        }
    });
    view->setContextMenu(view->defaultContextMenu());

    QSettings &c = config();
    const QStringList keys = view->configKeys();
    const auto set = [&](const QString &key, const QVariant &value) {
        if (keys.contains(key)) {
            view->setConfigValue(key, value);
        }
    };
    set(QStringLiteral("theme"), Theme::editorThemeName());
    set(QStringLiteral("line-numbers"), true);
    set(QStringLiteral("icon-bar"), false);
    set(QStringLiteral("folding-bar"), true);
    set(QStringLiteral("modification-markers"), true);
    set(QStringLiteral("scrollbar-minimap"), c.value(QStringLiteral("editor/minimap"), false).toBool());
    set(QStringLiteral("dynamic-word-wrap"), c.value(QStringLiteral("editor/wordWrap"), false).toBool());

    // Font: only when configured, otherwise KTextEditor's default (system fixed font).
    const QString family = c.value(QStringLiteral("editor/fontFamily")).toString().trimmed();
    const double size = c.value(QStringLiteral("editor/fontSize"), 0).toDouble();
    if (!family.isEmpty() || size > 0) {
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (!family.isEmpty()) {
            font.setFamily(family);
        }
        if (size > 0) {
            font.setPointSizeF(size);
        }
        set(QStringLiteral("font"), font);
    }
    applyKeymap(view);
    m_outline->addView(view);
}

void MainWindow::applyKeymap(KTextEditor::View *view)
{
    KActionCollection *collection = view->actionCollection();
    if (config().value(QStringLiteral("editor/vscodeKeys"), true).toBool()) {
        for (const KeyRemap &remap : kVsCodeKeymap) {
            if (QAction *action = collection->action(QLatin1String(remap.action))) {
                const QString keys = QLatin1String(remap.keys);
                action->setShortcuts(keys.isEmpty() ? QList<QKeySequence>() : QKeySequence::listFromString(keys));
            }
        }
    }

    // Window shortcuts win: drop editor shortcuts that would clash with them
    // (including the first key of chords such as Ctrl+K Ctrl+O).
    QList<QKeySequence> blocked = m_appKeys;
    for (const QKeySequence &key : std::as_const(m_appKeys)) {
        if (key.count() > 1) {
            blocked << QKeySequence(key[0]);
        }
    }
    const QList<QAction *> actions = collection->actions();
    for (QAction *action : actions) {
        QList<QKeySequence> shortcuts = action->shortcuts();
        const qsizetype before = shortcuts.size();
        shortcuts.removeIf([&blocked](const QKeySequence &k) {
            return blocked.contains(k);
        });
        if (shortcuts.size() != before) {
            action->setShortcuts(shortcuts);
        }
    }
}

bool MainWindow::closeView(KTextEditor::View *view)
{
    KTextEditor::Document *doc = view->document();
    TabView *group = groupOf(view);
    const QList<KTextEditor::View *> tabs = allViews(); // (a side-by-side diff's view of it does not count)
    if (std::count_if(tabs.cbegin(), tabs.cend(), [doc](KTextEditor::View *v) {
            return v->document() == doc;
        })
        > 1) { // still open in another group
        group->removeTab(group->indexOf(view));
        delete view;
    } else {
        if (!doc->closeUrl()) { // asks to save if modified; false = cancelled
            return false;
        }
        group->removeTab(group->indexOf(view));
        m_diffRanges.remove(doc); // coloured ranges are ours: delete them before the document
        m_lsp->removeDocument(doc); // closes it in the language server, deletes its underlines
        delete doc; // deletes the view too
    }
    if (group->count() == 0) {
        removeGroup(group);
    }
    if (allViews().isEmpty()) {
        updateEditorPage();
        updateWindowTitle();
    }
    return true;
}

bool MainWindow::closeAllViews()
{
    const QList<KTextEditor::View *> views = allViews();
    for (qsizetype i = views.size() - 1; i >= 0; --i) {
        if (!closeView(views.at(i))) {
            return false;
        }
    }
    const QList<DiffView *> diffs = diffPages(); // side-by-side diffs of files without a tab, of the index
    for (DiffView *diff : diffs) {
        closeTab(diff);
    }
    return true;
}

void MainWindow::closeTabsWhere(const std::function<bool(int)> &predicate)
{
    const TabView *group = m_tabs;
    for (int i = m_tabs->count() - 1; i >= 0 && m_tabs == group; --i) { // (a preview closes with its file: the group may go)
        if (predicate(i) && !closeTab(m_tabs->widget(i))) {
            return;
        }
    }
}

void MainWindow::newUntitled()
{
    KTextEditor::View *view = addView(KTextEditor::Editor::instance()->createDocument(this));
    m_tabs->setCurrentWidget(view);
    view->setFocus();
}

void MainWindow::saveAll()
{
    for (KTextEditor::Document *doc : documents()) {
        if (doc->isModified()) {
            doc->documentSave();
        }
    }
}

void MainWindow::openFileDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open File"), m_root.isEmpty() ? QDir::homePath() : m_root);
    for (const QString &file : files) {
        openFile(file);
    }
}

void MainWindow::openSettings()
{
    SettingsDialog::showFor(this);
}

// The Settings window changed lieve.conf: applies to the open editors what
// can change without a restart (font, minimap, word wrap, inline blame).
void MainWindow::applySettings()
{
    QSettings &c = config();
    const bool inlineBlame = c.value(QStringLiteral("git/inlineBlame"), true).toBool();
    const QString family = c.value(QStringLiteral("editor/fontFamily")).toString().trimmed();
    const double size = c.value(QStringLiteral("editor/fontSize"), 0).toDouble();
    QFont font = KTextEditor::Editor::instance()->font(); // none configured: the editor's default
    if (!family.isEmpty() || size > 0) {
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (!family.isEmpty()) {
            font.setFamily(family);
        }
        if (size > 0) {
            font.setPointSizeF(size);
        }
    }
    for (KTextEditor::View *view : allViews()) {
        view->setConfigValue(QStringLiteral("scrollbar-minimap"), c.value(QStringLiteral("editor/minimap"), false).toBool());
        view->setConfigValue(QStringLiteral("dynamic-word-wrap"), c.value(QStringLiteral("editor/wordWrap"), false).toBool());
        view->setConfigValue(QStringLiteral("font"), font);
        if (!inlineBlame && m_blameNotes.contains(view)) {
            KTextEditor::InlineNoteProvider *notes = m_blameNotes.take(view);
            view->unregisterInlineNoteProvider(notes);
            delete notes;
        } else if (inlineBlame && !m_blameNotes.contains(view)) {
            auto *notes = new BlameNotes;
            notes->m_doc = view->document();
            view->registerInlineNoteProvider(notes);
            m_blameNotes.insert(view, notes);
            connect(view, &QObject::destroyed, this, [this, view] {
                delete m_blameNotes.take(view);
            });
        }
    }
    showBlame();
    m_outline->applySettings(); // breadcrumbs, sticky scroll
}

void MainWindow::cycleTab(int step)
{
    const int count = m_tabs->count();
    if (count > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + step + count) % count);
    }
}

void MainWindow::updateEditorPage()
{
    if (!allViews().isEmpty() || !diffPages().isEmpty()) {
        m_editorStack->setCurrentWidget(m_editorSplit);
    } else if (m_root.isEmpty()) {
        if (m_editorStack->currentWidget() != m_chooserPage) {
            m_chooser->reload();
        }
        m_editorStack->setCurrentWidget(m_chooserPage);
    } else {
        m_editorStack->setCurrentWidget(m_emptyPage);
    }
}

void MainWindow::onCurrentTabChanged()
{
    updateWindowTitle();
    updateStatusBar();
    showBlame();
    KTextEditor::View *view = currentView();
    m_lsp->setCurrentView(view);
    if (!view) {
        updateEditorPage();
        return;
    }
    const QString path = localPath(view->document());
    if (path.isEmpty()) {
        return;
    }
    m_mru.removeAll(path);
    m_mru.prepend(path);
    if (!m_side->isHidden() && m_side->currentIndex() == ExplorerPage) {
        m_explorer->reveal(path);
    }
}

void MainWindow::updateTabTitles()
{
    QHash<QString, int> nameCount;
    for (KTextEditor::Document *doc : documents()) {
        ++nameCount[documentTitle(doc)];
    }
    for (TabView *group : std::as_const(m_groups)) {
    for (int i = 0; i < group->count(); ++i) {
        auto *view = qobject_cast<KTextEditor::View *>(group->widget(i));
        if (!view) {
            if (auto *diff = qobject_cast<DiffView *>(group->widget(i))) {
                group->setTabText(i, diff->title().replace(QLatin1Char('&'), QStringLiteral("&&")));
                group->setTabToolTip(i, prettyPath(diff->path()));
            }
            if (auto *preview = qobject_cast<MarkdownPreview *>(group->widget(i)); preview && preview->source()) {
                const QString path = localPath(preview->source());
                group->setTabText(i, preview->title().replace(QLatin1Char('&'), QStringLiteral("&&")));
                group->setTabToolTip(i, path.isEmpty() ? preview->title() : prettyPath(path));
            }
            continue;
        }
        KTextEditor::Document *doc = view->document();
        const QString name = documentTitle(doc);
        const QString path = localPath(doc);
        QString title = name;
        if (nameCount.value(name) > 1 && !path.isEmpty()) {
            title += QStringLiteral(" — ") + QFileInfo(path).dir().dirName();
        }
        if (doc->isModified()) {
            title += QStringLiteral(" ●");
        }
        title.replace(QLatin1Char('&'), QStringLiteral("&&"));
        group->setTabText(i, title);
        group->setTabToolTip(i, path.isEmpty() ? name : prettyPath(path));
        const QString iconName = doc->property("lieveIconName").toString();
        group->setTabIcon(i, VsCodeIcons::instance().fileIcon(iconName.isEmpty() ? name : iconName));
    }
    }
}

void MainWindow::updateWindowTitle()
{
    QStringList parts;
    if (KTextEditor::View *view = currentView()) {
        parts << documentTitle(view->document()) + (view->document()->isModified() ? QStringLiteral(" ●") : QString());
    } else if (auto *preview = qobject_cast<MarkdownPreview *>(m_tabs->currentWidget())) {
        parts << preview->title();
    } else if (auto *diff = qobject_cast<DiffView *>(m_tabs->currentWidget())) {
        parts << diff->title();
    }
    if (!m_root.isEmpty()) {
        parts << QFileInfo(m_root).fileName();
    }
    setWindowTitle(parts.join(QStringLiteral(" — ")));
    m_titleBar->setSearchText(m_root.isEmpty() ? tr("Choose a project") : QFileInfo(m_root).fileName());
}

void MainWindow::showTabMenu(const QPoint &pos)
{
    const int index = m_tabs->tabBar()->tabAt(pos);
    QWidget *page = m_tabs->widget(index); // an editor, or a Markdown preview
    KTextEditor::View *view = viewAt(index);
    auto *preview = qobject_cast<MarkdownPreview *>(page);
    if (!view && !(preview && preview->source())) {
        return;
    }
    KTextEditor::Document *doc = view ? view->document() : preview->source();
    const QString path = localPath(doc);
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("tab-close")), tr("Close"), this, [this, page] {
        closeTab(page);
    });
    menu.addAction(tr("Close Others"), this, [this, page] {
        closeTabsWhere([this, page](int i) {
            return m_tabs->widget(i) != page;
        });
    });
    menu.addAction(tr("Close to the Right"), this, [this, page] {
        const int at = m_tabs->indexOf(page);
        closeTabsWhere([at](int i) {
            return i > at;
        });
    });
    menu.addAction(tr("Close All"), this, [this] {
        closeAllViews();
    });
    if (view && MarkdownPreview::isMarkdown(doc)) {
        menu.addSeparator();
        menu.addAction(MarkdownPreview::icon(), tr("Open Preview to the Side"), this, [this, view] {
            activateView(view);
            openMarkdownPreview(true);
        });
    }
    if (!path.isEmpty()) {
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy Path"), this, [path] {
            QApplication::clipboard()->setText(path);
        });
        if (!m_root.isEmpty()) {
            menu.addAction(tr("Copy Relative Path"), this, [this, path] {
                QApplication::clipboard()->setText(QDir(m_root).relativeFilePath(path));
            });
        }
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Reveal in Explorer View"), this, [this, path] {
            showSidePage(ExplorerPage, false);
            m_explorer->reveal(path);
        });
        if (m_git->isRepository() && path.startsWith(m_git->root() + QLatin1Char('/'))) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("view-history")), tr("View File History"), this, [this, path] {
                m_gitPanel->showHistory(path);
            });
        }
    }
    menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
}

void MainWindow::onPathRenamed(const QString &oldPath, const QString &newPath)
{
    for (KTextEditor::Document *doc : documents()) {
        KTextEditor::View *view = doc->views().value(0);
        const QString path = localPath(doc);
        QString target;
        if (path == oldPath) {
            target = newPath;
        } else if (path.startsWith(oldPath + QLatin1Char('/'))) {
            target = newPath + path.mid(oldPath.size());
        }
        if (target.isEmpty() || doc->isModified()) {
            continue;
        }
        const KTextEditor::Cursor cursor = view->cursorPosition();
        doc->openUrl(QUrl::fromLocalFile(target));
        view->setCursorPosition(cursor);
    }
    updateTabTitles();
    updateWindowTitle();
}

void MainWindow::onPathRemoved(const QString &path)
{
    const QList<KTextEditor::View *> views = allViews();
    for (qsizetype i = views.size() - 1; i >= 0; --i) {
        KTextEditor::Document *doc = views.at(i)->document();
        const QString p = localPath(doc);
        if (!doc->isModified() && (p == path || p.startsWith(path + QLatin1Char('/')))) {
            closeView(views.at(i));
        }
    }
}

// ---------------------------------------------------------------------------
// Status bar

void MainWindow::updateGitStatus()
{
    if (!m_git->isRepository()) {
        m_status->setItem(QStringLiteral("branch"), QString());
        m_status->setItem(QStringLiteral("sync"), QString());
        return;
    }
    QString branch = m_git->branch();
    if (branch == QLatin1String("(detached)")) {
        branch = tr("Detached HEAD");
    }
    m_status->setItem(QStringLiteral("branch"), branch, m_gitIcon, tr("Branch %1 — click to check out another branch").arg(branch));
    if (!m_git->upstream().isEmpty()) {
        m_status->setItem(QStringLiteral("sync"),
                          QStringLiteral("↓%1 ↑%2").arg(m_git->behind()).arg(m_git->ahead()),
                          QIcon::fromTheme(QStringLiteral("view-refresh")),
                          tr("Synchronize with %1 (pull and push)").arg(m_git->upstream()));
    } else if (!m_git->unborn()) {
        m_status->setItem(QStringLiteral("sync"),
                          tr("Publish"),
                          QIcon::fromTheme(QStringLiteral("cloud-upload"), QIcon::fromTheme(QStringLiteral("go-up"))),
                          tr("Publish the branch to the server"));
    } else {
        m_status->setItem(QStringLiteral("sync"), QString());
    }
}

void MainWindow::updateCursorStatus()
{
    KTextEditor::View *view = currentView();
    if (!view) {
        return;
    }
    const KTextEditor::Cursor c = view->cursorPositionVirtual();
    const QString text = view->selection()
        ? tr("Ln %1, Col %2 (%3 selected)").arg(c.line() + 1).arg(c.column() + 1).arg(view->selectionText().size())
        : tr("Ln %1, Col %2").arg(c.line() + 1).arg(c.column() + 1);
    m_status->setItem(QStringLiteral("cursor"), text, QIcon(), tr("Go to Line (Ctrl+G)"));
}

void MainWindow::updateStatusBar()
{
    updateMarkdownActions(); // the language may have changed, or the tab
    updateGitStatus();
    KTextEditor::View *view = currentView();
    if (!view) {
        for (const char *name : {"blame", "cursor", "indent", "encoding", "eol", "mode", "eslint", "prettier"}) {
            m_status->setItem(QLatin1String(name), QString());
        }
        return;
    }
    KTextEditor::Document *doc = view->document();
    updateCursorStatus();
    const bool spaces = doc->configValue(QStringLiteral("replace-tabs")).toBool();
    m_status->setItem(QStringLiteral("indent"),
                      spaces ? tr("Spaces: %1").arg(doc->configValue(QStringLiteral("indent-width")).toInt())
                             : tr("Tab Size: %1").arg(doc->configValue(QStringLiteral("tab-width")).toInt()),
                      QIcon(),
                      tr("Indentation — click to change"));
    m_status->setItem(QStringLiteral("encoding"), doc->encoding().toUpper(), QIcon(), tr("Encoding — click to change"));
    QString eol = QStringLiteral("LF");
    if (auto *action = qobject_cast<KSelectAction *>(view->actionCollection()->action(QStringLiteral("set_eol")))) {
        const int i = action->currentItem();
        eol = i == 1 ? QStringLiteral("CRLF") : i == 2 ? QStringLiteral("CR") : QStringLiteral("LF");
    }
    m_status->setItem(QStringLiteral("eol"), eol, QIcon(), tr("End of line sequence — click to change"));
    m_status->setItem(QStringLiteral("mode"), doc->highlightingMode(), QIcon(), tr("Language mode — click to change"));
    const QString path = localPath(doc);
    if (!path.isEmpty() && Formatter::instance().available(path)) {
        const bool onSave = Formatter::instance().formatOnSave(projectFor(path));
        m_status->setItem(QStringLiteral("prettier"),
                          onSave ? QStringLiteral("✓ Prettier") : QStringLiteral("Prettier"),
                          QIcon(),
                          onSave ? tr("Prettier formats on save — click to format now") : tr("Prettier — click to format now"));
    } else {
        m_status->setItem(QStringLiteral("prettier"), QString());
    }
    if (!path.isEmpty() && Formatter::instance().steps(path, projectFor(path), true).eslint) {
        m_status->setItem(QStringLiteral("eslint"), QStringLiteral("✓ ESLint"), QIcon(), tr("ESLint fixes problems on save — click to fix now"));
    } else {
        m_status->setItem(QStringLiteral("eslint"), QString());
    }
    if (path.isEmpty()) {
        m_status->setItem(QStringLiteral("blame"), QString());
    }
}

// Who last changed the current line, like GitLens. The answer comes from the
// document's blame, computed once in the background (see DocumentBlame).
DocumentBlame *MainWindow::blameFor(KTextEditor::Document *doc)
{
    const QString root = m_git->root();
    const QString path = localPath(doc);
    if (root.isEmpty() || path.isEmpty() || !path.startsWith(root + QLatin1Char('/'))) {
        return nullptr;
    }
    DocumentBlame *blame = m_blames.value(doc);
    if (!blame) {
        if (!m_gitEmails.contains(root)) {
            QProcess config;
            config.setWorkingDirectory(root);
            config.start(QStringLiteral("git"), {QStringLiteral("config"), QStringLiteral("user.email")});
            config.waitForFinished(2000);
            m_gitEmails.insert(root, QString::fromUtf8(config.readAllStandardOutput()).trimmed());
        }
        blame = new DocumentBlame(doc, root, doc); // goes away with the document
        m_blames.insert(doc, blame);
        connect(blame, &QObject::destroyed, this, [this, doc] {
            m_blames.remove(doc);
            m_staleBlames.remove(doc);
        });
        connect(blame, &DocumentBlame::updated, this, [this, doc] {
            if (KTextEditor::View *view = currentView(); view && view->document() == doc) {
                showBlame();
            }
        });
        blame->refresh();
    } else if (m_staleBlames.remove(doc)) {
        blame->refresh();
    }
    return blame;
}

void MainWindow::showBlame()
{
    KTextEditor::View *view = currentView();
    for (auto it = m_blameNotes.cbegin(); it != m_blameNotes.cend(); ++it) {
        if (it.key() != view) { // only the active editor shows it (split groups)
            static_cast<BlameNotes *>(it.value())->set(-1, QString());
        }
    }
    auto *notes = view ? static_cast<BlameNotes *>(m_blameNotes.value(view)) : nullptr;
    DocumentBlame *blame = view ? blameFor(view->document()) : nullptr;
    const int line = view ? view->cursorPosition().line() : -1;
    const int entry = blame ? blame->entry(line) : DocumentBlame::Unknown;
    if (entry == DocumentBlame::Unknown) {
        if (notes) {
            notes->set(-1, QString());
        }
        m_status->setItem(QStringLiteral("blame"), QString());
        return;
    }
    if (entry == DocumentBlame::Uncommitted) {
        if (notes) {
            notes->set(line, tr("You, now • Uncommitted changes"));
        }
        m_status->setItem(QStringLiteral("blame"),
                          tr("Not committed yet"),
                          QIcon::fromTheme(QStringLiteral("vcs-commit")),
                          tr("This line has uncommitted changes"));
        return;
    }
    const DocumentBlame::Commit &c = blame->commit(entry);
    const bool mine = !c.mail.isEmpty() && c.mail.compare(m_gitEmails.value(m_git->root()), Qt::CaseInsensitive) == 0;
    const QString who = mine ? tr("You") : c.author;
    QString headline = c.summary;
    if (headline.size() > 72) {
        headline = headline.left(71) + QStringLiteral("…");
    }
    if (notes) {
        notes->set(line, QStringLiteral("%1, %2 • %3").arg(who, relativeAge(c.time), headline));
    }
    m_status->setItem(QStringLiteral("blame"),
                      QStringLiteral("%1, %2").arg(who, relativeAge(c.time)),
                      QIcon::fromTheme(QStringLiteral("vcs-commit")),
                      QStringLiteral("%1\n%2 · %3 · %4")
                          .arg(c.summary, c.hash.left(8), c.author, QLocale().toString(QDateTime::fromSecsSinceEpoch(c.time), QLocale::ShortFormat)));
}

void MainWindow::updateUpdaterStatus()
{
    const Updater &u = Updater::instance();
    switch (u.state()) {
    case Updater::State::Idle:
        m_status->setItem(QStringLiteral("update"), QString());
        break;
    case Updater::State::Available:
        m_status->setItem(QStringLiteral("update"),
                          tr("⬆ Update %1").arg(u.latestVersion()),
                          QIcon(),
                          tr("Lieve %1 is available (current: %2)").arg(u.latestVersion(), QStringLiteral(LIEVE_VERSION)));
        break;
    case Updater::State::Updating:
        m_status->setItem(QStringLiteral("update"), tr("Updating…"), QIcon(), tr("Downloading, building and installing Lieve %1").arg(u.latestVersion()));
        break;
    case Updater::State::Updated:
        m_status->setItem(QStringLiteral("update"),
                          tr("↻ Restart to use %1").arg(u.latestVersion()),
                          QIcon(),
                          tr("Lieve %1 is installed: click to restart").arg(u.latestVersion()));
        break;
    case Updater::State::Failed:
        m_status->setItem(QStringLiteral("update"), tr("Update failed"), QIcon(), tr("Click for details"));
        break;
    }
}

void MainWindow::showUpdateDialog()
{
    Updater &u = Updater::instance();
    if (u.state() == Updater::State::Updated) {
        u.restart();
        return;
    }
    QMessageBox box(this);
    box.setIcon(u.state() == Updater::State::Failed ? QMessageBox::Warning : QMessageBox::Information);
    box.setWindowTitle(tr("Lieve Update"));
    if (u.state() == Updater::State::Failed) {
        box.setText(tr("The update to Lieve %1 failed.").arg(u.latestVersion()));
        box.setInformativeText(tr("The current version was left untouched. The build details are below."));
        box.setDetailedText(u.log().right(6000));
    } else {
        box.setText(tr("Lieve %1 is available (current: %2).").arg(u.latestVersion(), QStringLiteral(LIEVE_VERSION)));
        box.setTextFormat(Qt::PlainText);
        box.setInformativeText(u.notes().isEmpty() ? tr("No release notes.") : u.notes().left(1500));
    }
    QPushButton *updateNow = nullptr;
    if (u.canSelfUpdate()) {
        updateNow = box.addButton(u.state() == Updater::State::Failed ? tr("Retry") : tr("Update Now"), QMessageBox::AcceptRole);
    }
    QPushButton *page = box.addButton(tr("Open Release Page"), QMessageBox::HelpRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.exec();
    if (updateNow && box.clickedButton() == updateNow) {
        u.update();
    } else if (box.clickedButton() == page) {
        QDesktopServices::openUrl(QUrl(u.releaseUrl()));
    }
}

void MainWindow::onStatusClicked(const QString &name)
{
    if (name == QLatin1String("update")) {
        showUpdateDialog();
        return;
    }
    if (name == QLatin1String("branch")) {
        m_gitPanel->showCheckout();
        return;
    }
    if (name == QLatin1String("sync")) {
        if (m_git->upstream().isEmpty()) {
            m_git->push();
        } else {
            m_git->sync();
        }
        return;
    }
    if (name == QLatin1String("problems")) {
        toggleProblems(false);
        return;
    }
    KTextEditor::View *view = currentView();
    if (!view) {
        return;
    }
    QToolButton *button = m_status->item(name);
    const auto showAbove = [button](QMenu *menu) {
        const QPoint at = button->mapToGlobal(QPoint(0, 0));
        menu->exec(at - QPoint(0, menu->sizeHint().height() + 2));
    };
    if (name == QLatin1String("cursor")) {
        m_quickOpen->popup(m_mru, QStringLiteral(":"));
    } else if (name == QLatin1String("prettier") || name == QLatin1String("eslint")) {
        formatDocument(view->document(), false);
    } else if (name == QLatin1String("indent")) {
        KTextEditor::Document *doc = view->document();
        const bool spaces = doc->configValue(QStringLiteral("replace-tabs")).toBool();
        const int width = doc->configValue(QStringLiteral("indent-width")).toInt();
        QMenu menu(this);
        QAction *withSpaces = menu.addAction(tr("Indent Using Spaces"), this, [doc] {
            doc->setConfigValue(QStringLiteral("replace-tabs"), true);
        });
        QAction *withTabs = menu.addAction(tr("Indent Using Tabs"), this, [doc] {
            doc->setConfigValue(QStringLiteral("replace-tabs"), false);
        });
        withSpaces->setCheckable(true);
        withSpaces->setChecked(spaces);
        withTabs->setCheckable(true);
        withTabs->setChecked(!spaces);
        menu.addSeparator();
        for (const int w : {2, 4, 8}) {
            QAction *a = menu.addAction(tr("Width %1").arg(w), this, [doc, w] {
                doc->setConfigValue(QStringLiteral("indent-width"), w);
                doc->setConfigValue(QStringLiteral("tab-width"), w);
            });
            a->setCheckable(true);
            a->setChecked(width == w);
        }
        showAbove(&menu);
        updateStatusBar();
    } else {
        // Encoding, line ends and language: the editor's own menus.
        const QString actionName = name == QLatin1String("encoding") ? QStringLiteral("set_encoding")
            : name == QLatin1String("eol")                           ? QStringLiteral("set_eol")
                                                                     : QStringLiteral("tools_highlighting");
        if (QAction *action = view->actionCollection()->action(actionName)) {
            if (QMenu *menu = action->menu<QMenu *>()) {
                showAbove(menu);
                updateStatusBar();
            }
        }
    }
}

QString MainWindow::documentTitle(KTextEditor::Document *doc) const
{
    const QString custom = doc->property("lieveTitle").toString();
    return custom.isEmpty() ? doc->documentName() : custom;
}

// ---------------------------------------------------------------------------
// Git

void MainWindow::updateGitBadge()
{
    const int count = m_git->isRepository() ? int(m_git->entries().size()) : 0;
    m_actGit->setIcon(Theme::activityIcon(m_gitIcon, count));
    m_actGit->setToolTip(count <= 0       ? tr("Source Control (Ctrl+Shift+G)")
                             : count == 1 ? tr("Source Control (Ctrl+Shift+G) — 1 change")
                                          : tr("Source Control (Ctrl+Shift+G) — %1 changes").arg(count));
}

void MainWindow::updateGutter(KTextEditor::Document *doc)
{
    GitGutter *gutter = m_gutters.value(doc);
    if (!gutter) {
        return;
    }
    const QString path = localPath(doc);
    const QString root = m_git->root();
    const bool tracked = !root.isEmpty() && path.startsWith(root + QLatin1Char('/'));
    const QString relative = tracked ? path.mid(root.size() + 1) : QString();
    gutter->setSource(root, relative, tracked ? m_git->indexState(relative) : QString());
}

// Alt+F3 / Shift+Alt+F3, like VS Code (and Alt+F5 / Shift+Alt+F5).
void MainWindow::goToChange(bool forward)
{
    if (auto *diff = qobject_cast<DiffView *>(m_tabs->currentWidget())) {
        diff->goToChange(forward);
        return;
    }
    KTextEditor::View *view = currentView();
    if (view && !view->document()->property("lieveDiffKey").toString().isEmpty()) {
        DiffView::goToHunk(view, forward); // a unified diff: its "@@" lines
        return;
    }
    GitGutter *gutter = view ? m_gutters.value(view->document()) : nullptr;
    const int line = gutter ? gutter->nextChange(view->cursorPosition().line(), forward) : -1;
    if (line >= 0) {
        view->setCursorPosition(KTextEditor::Cursor(line, 0));
    }
}

// A click on a change mark: the file's diff, at that change.
void MainWindow::showChange(KTextEditor::Document *doc, int line)
{
    const QString path = localPath(doc);
    const QList<Lieve::GitEntry> &entries = m_git->entries();
    for (const Lieve::GitEntry &entry : entries) {
        if (entry.group != Lieve::GitEntry::Changes || m_git->absolutePath(entry.path) != path) {
            continue;
        }
        openChanges(entry, line);
        return;
    }
}

// A change's diff, side by side (git/sideBySide, else VS Code's
// diffEditor.renderSideBySide) when it can be, at `line` of the new version
// (-1: its first change); in place of the tab `replace`.
void MainWindow::openChanges(const Lieve::GitEntry &entry, int line, QWidget *replace)
{
    if (DiffView::supports(entry) && configOrVscode(QStringLiteral("git/sideBySide"), m_root, QStringLiteral("diffEditor.renderSideBySide"), true)) {
        openSideBySide(entry, line, replace);
        return;
    }
    m_git->diff(entry, [this, entry, line, replace = QPointer<QWidget>(replace)](const QString &text) {
        showDiff(entry, text);
        const int hunk = line >= 0 ? GitGutter::diffLineFor(text, line) : -1;
        if (KTextEditor::View *view = currentView(); view && hunk >= 0) {
            view->setCursorPosition(KTextEditor::Cursor(hunk, 0));
        }
        if (replace) {
            closeTab(replace);
        }
    });
}

void MainWindow::openSideBySide(const Lieve::GitEntry &entry, int line, QWidget *replace)
{
    const auto showOpen = [this, entry, line](QWidget *replaced) {
        for (DiffView *diff : diffPages()) {
            if (diff->key() == DiffView::keyOf(entry)) { // open already: shown, at that change
                setActiveGroup(groupOfPage(diff));
                m_tabs->setCurrentWidget(diff);
                if (line >= 0) {
                    diff->revealLine(line);
                }
                if (replaced && replaced != diff) {
                    closeTab(replaced);
                }
                return true;
            }
        }
        return false;
    };
    if (showOpen(replace)) {
        return;
    }
    DiffView::load(m_git, entry, [this, entry, line, showOpen, replace = QPointer<QWidget>(replace)](const DiffView::Texts &texts) {
        if (showOpen(replace)) { // opened meanwhile (a second click)
            return;
        }
        if (!texts.ok) { // binary or large: the unified diff
            m_git->diff(entry, [this, entry, replace](const QString &text) {
                showDiff(entry, text);
                if (replace) {
                    closeTab(replace);
                }
            });
            return;
        }
        KTextEditor::Document *working = nullptr;
        if (entry.group == Lieve::GitEntry::Changes) {
            if (KTextEditor::View *view = findView(QFileInfo(m_git->absolutePath(entry.path)).canonicalFilePath())) {
                working = view->document(); // the right side edits the file's own document
            }
        }
        auto *diff = new DiffView(
            m_staging,
            entry,
            texts,
            working,
            [this](KTextEditor::View *view) {
                applyKeymap(view);
            },
            m_tabs);
        connect(diff, &DiffView::titleChanged, this, [this] {
            updateTabTitles();
            updateWindowTitle();
        });
        connect(diff, &DiffView::focused, this, [this, diff] {
            setActiveGroup(groupOfPage(diff));
        });
        connect(diff, &DiffView::documentEdited, this, [this, diff] {
            adoptDiffDocument(diff);
        });
        // Closing the file's last tab closes its diff, like a preview.
        connect(diff, &DiffView::documentClosed, this, [this, diff = QPointer<DiffView>(diff)] {
            QTimer::singleShot(0, this, [this, diff] {
                if (diff) {
                    closeTab(diff);
                }
            });
        });
        const int index = m_tabs->count() == 0 ? 0 : m_tabs->currentIndex() + 1; // next to the current tab
        m_tabs->insertTab(index, diff, QString());
        m_tabs->setTabIcon(index, VsCodeIcons::instance().fileIcon(QFileInfo(entry.path).fileName()));
        m_tabs->setCurrentWidget(diff);
        if (line >= 0) {
            diff->revealLine(line);
        }
        if (replace) {
            closeTab(replace);
        }
        updateTabTitles();
        updateEditorPage();
        updateWindowTitle();
    });
}

// The tab bar's Inline / Side by Side button: the same change the other way,
// in place of this tab; remembered in lieve.conf like VS Code's setting.
void MainWindow::toggleDiffView(QWidget *page)
{
    setActiveGroup(groupOfPage(page));
    const QString comment = tr("Diffs of changes side by side (true) or inline (false); auto = like VS Code (diffEditor.renderSideBySide).");
    if (auto *diff = qobject_cast<DiffView *>(page)) {
        setConfigValue(QStringLiteral("git"), QStringLiteral("sideBySide"), QStringLiteral("false"), comment);
        const Lieve::GitEntry entry = diff->entry();
        m_git->diff(entry, [this, entry, diff = QPointer<DiffView>(diff)](const QString &text) {
            showDiff(entry, text);
            if (diff) {
                closeTab(diff);
            }
        });
        return;
    }
    auto *view = qobject_cast<KTextEditor::View *>(page);
    const std::optional<Lieve::GitEntry> entry = view ? DiffView::entryOf(m_git, view->document()->property("lieveDiffKey").toString()) : std::nullopt;
    if (entry && DiffView::supports(*entry)) {
        setConfigValue(QStringLiteral("git"), QStringLiteral("sideBySide"), QStringLiteral("true"), comment);
        openSideBySide(*entry, -1, view);
    }
}

QList<DiffView *> MainWindow::diffPages() const
{
    QList<DiffView *> pages;
    for (TabView *group : m_groups) {
        for (int i = 0; i < group->count(); ++i) {
            if (auto *diff = qobject_cast<DiffView *>(group->widget(i))) {
                pages << diff;
            }
        }
    }
    return pages;
}

// The document a side-by-side diff opened for a file without a tab, now the window's.
KTextEditor::Document *MainWindow::takeDiffDocument(const QString &path)
{
    for (DiffView *diff : diffPages()) {
        if (diff->ownsDocument() && localPath(diff->document()) == path) {
            KTextEditor::Document *doc = diff->releaseDocument();
            doc->setParent(this);
            return doc;
        }
    }
    return nullptr;
}

// Edited in its diff: the file gets a tab of its own next to it (saving,
// unsaved changes on closing, hot exit work as for any file).
void MainWindow::adoptDiffDocument(DiffView *diff)
{
    TabView *group = groupOfPage(diff);
    KTextEditor::Document *doc = diff->ownsDocument() ? diff->document() : nullptr;
    if (!group || !doc) {
        return;
    }
    QWidget *focus = QApplication::focusWidget();
    diff->releaseDocument();
    doc->setParent(this);
    addView(doc, group); // after the current tab, which stays current
    if (focus) {
        focus->setFocus();
    }
}

// Stage / Unstage / Revert Selected Ranges: in a side-by-side diff, or the editor.
void MainWindow::runStagingCommand(int command)
{
    if (auto *diff = qobject_cast<DiffView *>(m_tabs->currentWidget())) {
        diff->run(GitStaging::Command(command));
    } else if (KTextEditor::View *view = currentView()) {
        m_staging->run(GitStaging::Command(command), view);
    }
}

// A read-only tab with the unified diff, added/removed lines coloured.
void MainWindow::showDiff(const Lieve::GitEntry &entry, const QString &text)
{
    const QString name = QFileInfo(entry.path).fileName();
    QString title = tr("%1 (Changes)").arg(name);
    if (entry.group == Lieve::GitEntry::Staged) {
        title = tr("%1 (Staged)").arg(name);
    } else if (entry.group == Lieve::GitEntry::Conflicts) {
        title = tr("%1 (Conflict)").arg(name);
    } else if (entry.status == QLatin1Char('U')) {
        title = tr("%1 (Untracked)").arg(name);
    }
    const QString key = QString::number(int(entry.group)) + QLatin1Char(':') + entry.path;
    showDiffTab(key, title, name, text);
}

// The tab with this key is reused (refreshed); `iconName` picks its file icon.
void MainWindow::showDiffTab(const QString &key, const QString &title, const QString &iconName, const QString &text)
{
    const QString body = text.isEmpty() ? tr("No text differences (binary file, or only the permissions changed).\n") : text;

    KTextEditor::View *view = nullptr;
    for (KTextEditor::View *v : allViews()) {
        if (v->document()->property("lieveDiffKey").toString() == key) {
            view = v;
            break;
        }
    }
    KTextEditor::Document *doc = nullptr;
    if (view) {
        doc = view->document();
        m_diffRanges.remove(doc);
        doc->setReadWrite(true);
        doc->setText(body);
    } else {
        doc = KTextEditor::Editor::instance()->createDocument(this);
        doc->setProperty("lieveDiffKey", key);
        doc->setProperty("lieveTitle", title);
        doc->setProperty("lieveIconName", iconName);
        doc->setText(body);
        view = addView(doc);
    }
    doc->setHighlightingMode(QStringLiteral("Diff"));
    doc->setModified(false);
    doc->setReadWrite(false);
    colorDiff(doc);
    // The colours to the right edge (KTextEditor paints them only behind the text).
    QList<LineTint::Span> spans;
    if (const auto ranges = m_diffRanges.value(doc)) {
        for (const auto &range : *ranges) {
            spans << LineTint::spanOf(range.get());
        }
    }
    if (LineTint *tint = LineTint::of(view)) {
        tint->set(spans);
    }
    activateView(view);
    updateEditorPage();
    updateTabTitles();
    updateWindowTitle();
}

void MainWindow::colorDiff(KTextEditor::Document *doc)
{
    enum Kind {
        Plain,
        Added,
        Removed,
        Hunk,
    };
    const bool dark = isDarkTheme();
    const auto attribute = [](const QColor &color) {
        KTextEditor::Attribute::Ptr a(new KTextEditor::Attribute());
        a->setBackground(color);
        a->setBackgroundFillWhitespace(true);
        a->setProperty(QTextFormat::FullWidthSelection, true); // colour the whole line, like VS Code
        return a;
    };
    const KTextEditor::Attribute::Ptr styles[] = {
        {},
        attribute(QColor(46, 160, 67, dark ? 60 : 45)),
        attribute(QColor(248, 81, 73, dark ? 60 : 45)),
        attribute(QColor(56, 139, 253, dark ? 45 : 35)),
    };
    const auto kindOf = [](const QString &line) {
        // file headers, and the "---" between a commit's message and its stat
        if (line.startsWith(QLatin1String("+++ ")) || line.startsWith(QLatin1String("--- ")) || line == QLatin1String("---")) {
            return Plain;
        }
        if (line.startsWith(QLatin1Char('+'))) {
            return Added;
        }
        if (line.startsWith(QLatin1Char('-'))) {
            return Removed;
        }
        if (line.startsWith(QLatin1String("@@"))) {
            return Hunk;
        }
        return Plain;
    };

    auto ranges = std::make_shared<std::vector<std::unique_ptr<KTextEditor::MovingRange>>>();
    const int lines = doc->lines();
    int start = 0;
    Kind current = Plain;
    for (int line = 0; line <= lines; ++line) {
        const Kind kind = line < lines ? kindOf(doc->line(line)) : Plain;
        if (kind == current) {
            continue;
        }
        if (current != Plain) {
            // Up to the start of the next line: the colour fills the whole width.
            const KTextEditor::Range range = line < lines ? KTextEditor::Range(start, 0, line, 0)
                                                          : KTextEditor::Range(start, 0, lines - 1, doc->lineLength(lines - 1));
            KTextEditor::MovingRange *moving = doc->newMovingRange(range);
            moving->setAttribute(styles[current]);
            ranges->emplace_back(moving);
        }
        start = line;
        current = kind;
    }
    m_diffRanges.insert(doc, ranges);
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    if (!Theme::styled()) {
        QMainWindow::paintEvent(event);
        return;
    }
    const Colors &c = Theme::colors();
    QPainter p(this);
    const bool rounded = testAttribute(Qt::WA_TranslucentBackground) && !isMaximized() && !isFullScreen();
    if (!rounded) {
        p.fillRect(rect(), c.base);
        return;
    }
    // Same radius as the cards inside, and a thin outline like Breeze windows.
    constexpr qreal radius = 10;
    p.setRenderHint(QPainter::Antialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(Qt::NoPen);
    p.setBrush(c.base);
    p.drawRoundedRect(QRectF(rect()), radius, radius);
    QColor outline = c.fg;
    outline.setAlphaF(c.dark ? 0.16f : 0.22f);
    p.setPen(QPen(outline, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        update(); // square corners when maximised
    }
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_git) {
        m_git->scheduleRefresh(); // back from another program: files may have changed
    }
}

QString MainWindow::projectFor(const QString &path) const
{
    if (!m_root.isEmpty() && path.startsWith(m_root + QLatin1Char('/'))) {
        return m_root;
    }
    return projectRootFor(QFileInfo(path).absolutePath());
}

void MainWindow::formatDocument(KTextEditor::Document *doc, bool onSave)
{
    const QString path = localPath(doc);
    if (path.isEmpty() || doc == m_saveWithoutFormat) {
        return;
    }
    Formatter &formatter = Formatter::instance();
    const QString project = projectFor(path);
    // ESLint's fixes, when on for the project, then Prettier: the command
    // gives the same text as saving.
    const FormatSteps steps = formatter.steps(path, project, onSave);
    if (!steps.eslint && !steps.prettier) {
        if (!onSave) {
            showMessage(doc, tr("Prettier is not installed in this project (node_modules/prettier)."), KTextEditor::Message::Information);
        }
        return;
    }
    KTextEditor::View *view = (currentView() && currentView()->document() == doc) ? currentView() : doc->views().value(0);
    const QString text = doc->text();
    const int cursor = view ? offsetOf(text, view->cursorPosition()) : -1;
    const FormatResult result = formatter.format(path, text, cursor, project, steps, onSave ? 4000 : 10000);
    if (result.changed()) {
        applyFormatted(doc, view, text, result.text, result.cursor); // one undo step for both
    }
    if (result.eslint == FormatResult::Failed) {
        showMessage(doc, result.eslintMessage, KTextEditor::Message::Warning);
    }
    switch (result.status) {
    case FormatResult::Failed:
        showMessage(doc, QStringLiteral("Prettier: %1").arg(result.message), KTextEditor::Message::Warning);
        break;
    case FormatResult::Skipped:
        if (!onSave && steps.prettier) {
            const QString why = result.message == QLatin1String("ignored")
                ? tr("Not formatted: the file is excluded by .prettierignore.")
                : result.message == QLatin1String("no-config") ? tr("Not formatted: no Prettier configuration found (prettier.requireConfig).")
                                                               : tr("Not formatted: Prettier does not support this file type.");
            showMessage(doc, why, KTextEditor::Message::Information);
        }
        break;
    case FormatResult::Formatted:
    case FormatResult::Unchanged:
        break;
    }
}

QString MainWindow::selectedText() const
{
    KTextEditor::View *view = currentView();
    return (view && view->selection()) ? view->selectionText() : QString();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    for (TabView *group : std::as_const(m_groups)) {
        if (watched != group->tabBar()) {
            continue;
        }
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (event->type() == QEvent::MouseButtonPress) {
            setActiveGroup(group); // before the tab bar changes its current tab
        } else if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::MiddleButton) {
            if (QWidget *page = group->widget(group->tabBar()->tabAt(mouse->position().toPoint()))) {
                closeTab(page); // an editor or a preview
                return true;
            }
        }
    }
    if (watched == m_body && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress)) {
        // The gaps around the cards are the window's resize borders.
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint pos = m_body->mapTo(this, mouse->position().toPoint());
        Qt::Edges edges;
        if (!isMaximized() && !isFullScreen() && (windowFlags() & Qt::FramelessWindowHint)) {
            const int grip = 6;
            if (pos.x() <= grip) {
                edges |= Qt::LeftEdge;
            } else if (pos.x() >= width() - grip) {
                edges |= Qt::RightEdge;
            }
            if (pos.y() >= height() - grip) {
                edges |= Qt::BottomEdge;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            if (edges == (Qt::BottomEdge | Qt::LeftEdge)) {
                m_body->setCursor(Qt::SizeBDiagCursor);
            } else if (edges == (Qt::BottomEdge | Qt::RightEdge)) {
                m_body->setCursor(Qt::SizeFDiagCursor);
            } else if (edges & Qt::BottomEdge) {
                m_body->setCursor(Qt::SizeVerCursor);
            } else if (edges) {
                m_body->setCursor(Qt::SizeHorCursor);
            } else {
                m_body->unsetCursor();
            }
        } else if (edges && mouse->button() == Qt::LeftButton && windowHandle()) {
            windowHandle()->startSystemResize(edges);
            return true;
        }
    }
    if (watched == centralWidget() && event->type() == QEvent::Resize && m_switcher && m_switcher->isVisible()) {
        placeSwitcher();
    }
    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Per-project session

void MainWindow::saveSession()
{
    if (!m_side->isHidden()) {
        m_sideWidth = m_mainSplit->sizes().value(0, m_sideWidth);
    }
    if (!m_terminal->isHidden()) {
        m_terminalHeight = m_rightSplit->sizes().value(1, m_terminalHeight);
    }
    QSettings &s = state();
    // Defaults for the next new window
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/sideWidth"), m_sideWidth);
    s.setValue(QStringLiteral("window/terminalHeight"), m_terminalHeight);
    if (m_root.isEmpty()) {
        return;
    }

    // Hot exit: each tab also records its backup ("" = none); a new document
    // (no file: path "") is kept only when it has one.
    QStringList files;
    QStringList cursors;
    QStringList backups;
    int activeIndex = -1;
    KTextEditor::View *current = currentView();
    if (auto *preview = qobject_cast<MarkdownPreview *>(m_tabs->currentWidget()); preview && preview->source()) {
        current = preview->source()->views().value(0); // previews are not kept: their file is the active one
    }
    for (KTextEditor::Document *doc : documents()) { // split groups are not kept: each file once
        KTextEditor::View *view = (current && current->document() == doc) ? current : doc->views().value(0);
        const QString path = localPath(doc);
        const QString backup = doc->property("lieveBackup").toString();
        if (!path.isEmpty() || !backup.isEmpty()) {
            if (view == current) {
                activeIndex = int(files.size());
            }
            files << path;
            const KTextEditor::Cursor c = view->cursorPosition();
            cursors << QStringLiteral("%1:%2").arg(c.line()).arg(c.column());
            backups << backup;
        }
    }
    const KTextEditor::View *active = current;
    const QString key = sessionKey(m_root);
    s.setValue(key + QStringLiteral("/path"), m_root);
    s.setValue(key + QStringLiteral("/files"), files);
    s.setValue(key + QStringLiteral("/cursors"), cursors);
    s.setValue(key + QStringLiteral("/backups"), backups);
    s.setValue(key + QStringLiteral("/active"), active ? localPath(active->document()) : QString());
    s.setValue(key + QStringLiteral("/activeIndex"), activeIndex);
    s.setValue(key + QStringLiteral("/geometry"), saveGeometry());
    s.setValue(key + QStringLiteral("/sideVisible"), !m_side->isHidden());
    s.setValue(key + QStringLiteral("/sidePage"), m_side->currentIndex());
    s.setValue(key + QStringLiteral("/sideWidth"), m_sideWidth);
    s.setValue(key + QStringLiteral("/terminalVisible"), !m_terminal->isHidden() && m_terminal->hasTerminals()); // not for Problems alone
    s.setValue(key + QStringLiteral("/terminalHeight"), m_terminalHeight);
    s.sync();
}

void MainWindow::restoreSession(bool applyGeometry)
{
    QSettings &s = state();
    const QString key = sessionKey(m_root);
    if (applyGeometry) {
        restoreGeometry(s.value(key + QStringLiteral("/geometry"), s.value(QStringLiteral("window/geometry"))).toByteArray());
    }
    m_sideWidth = qMax(170, s.value(key + QStringLiteral("/sideWidth"), m_sideWidth).toInt());
    m_terminalHeight = qMax(100, s.value(key + QStringLiteral("/terminalHeight"), m_terminalHeight).toInt());
    m_side->setCurrentIndex(qBound(0, s.value(key + QStringLiteral("/sidePage"), 0).toInt(), 2));
    setSidebarVisible(s.value(key + QStringLiteral("/sideVisible"), true).toBool());

    const QStringList files = s.value(key + QStringLiteral("/files")).toStringList();
    const QStringList cursors = s.value(key + QStringLiteral("/cursors")).toStringList();
    const QStringList backups = s.value(key + QStringLiteral("/backups")).toStringList();
    QList<KTextEditor::View *> views;
    for (qsizetype i = 0; i < files.size(); ++i) {
        KTextEditor::View *view = restoreDocument(files.at(i), backups.value(i));
        views << view;
        if (view && i < cursors.size()) {
            const QStringList lc = cursors.at(i).split(QLatin1Char(':'));
            if (lc.size() == 2) {
                view->setCursorPosition(KTextEditor::Cursor(lc.at(0).toInt(), lc.at(1).toInt()));
            }
        }
    }
    // Backups the session does not list (Lieve crashed before saving it) come back too.
    const QStringList unlisted = Backups::names(m_root);
    for (const QString &name : unlisted) {
        if (!backups.contains(name)) {
            restoreDocument(QString(), name);
        }
    }
    const QString active = s.value(key + QStringLiteral("/active")).toString();
    if (KTextEditor::View *view = views.value(s.value(key + QStringLiteral("/activeIndex"), -1).toInt())) {
        m_tabs->setCurrentWidget(view);
    } else if (!active.isEmpty()) {
        openFile(active);
    }

    // Terminal and focus once the window has its final size.
    const bool terminal = s.value(key + QStringLiteral("/terminalVisible"), false).toBool();
    const QString root = m_root;
    QTimer::singleShot(0, this, [this, root, terminal] {
        if (m_root != root) {
            return;
        }
        if (terminal) {
            setTerminalVisible(true);
        }
        if (KTextEditor::View *view = currentView()) {
            view->setFocus();
            if (!m_side->isHidden() && m_side->currentIndex() == ExplorerPage) {
                m_explorer->reveal(localPath(view->document()));
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Hot exit

bool MainWindow::hotExit() const
{
    return !m_root.isEmpty() && Backups::hotExit();
}

// Writes the backup of a document with unsaved changes, if its text changed
// since the last one. False only when it could not be written.
bool MainWindow::backupDocument(KTextEditor::Document *doc)
{
    const QString name = doc->property("lieveBackup").toString();
    if (m_root.isEmpty() || !doc->isModified() || !doc->isReadWrite() // read-only: diff tabs
        || (!name.isEmpty() && doc->property("lieveBackupRevision").toLongLong() == doc->revision())) {
        return true;
    }
    Backups::Backup backup;
    backup.path = localPath(doc);
    backup.text = doc->text();
    if (backup.path.isEmpty() && backup.text.isEmpty()) {
        dropBackup(doc); // a new document emptied again: nothing to keep
        return true;
    }
    backup.name = name.isEmpty() ? Backups::newName(backup.path) : name;
    backup.diskTime = doc->property("lieveDiskTime").toLongLong();
    if (!Backups::write(m_root, backup)) {
        qWarning().noquote() << "Lieve: cannot write the backup of" << documentTitle(doc);
        return false;
    }
    doc->setProperty("lieveBackup", backup.name);
    doc->setProperty("lieveBackupRevision", doc->revision());
    return true;
}

// The document has no unsaved changes any more (saved, reverted, discarded).
void MainWindow::dropBackup(KTextEditor::Document *doc)
{
    const QString name = doc->property("lieveBackup").toString();
    if (name.isEmpty() || m_keepBackups) {
        return;
    }
    Backups::remove(m_root, name);
    doc->setProperty("lieveBackup", QVariant());
    doc->setProperty("lieveBackupRevision", QVariant());
}

// Before closing the project's documents: backs up the unsaved ones (asking
// only about those whose backup cannot be written), records them in the
// session and marks them unmodified, so that closing them does not ask.
// False if the user cancelled.
bool MainWindow::keepUnsaved()
{
    m_backupTimer.stop();
    for (KTextEditor::Document *doc : documents()) {
        if (!backupDocument(doc)) {
            activateView(doc->views().value(0));
            if (!doc->queryClose()) {
                return false;
            }
            dropBackup(doc); // saved or discarded: an older backup must not come back
        }
    }
    saveSession();
    m_keepBackups = true;
    for (KTextEditor::Document *doc : documents()) {
        doc->setModified(false);
    }
    return true;
}

// Opens a document of the session. With a backup its unsaved text comes back,
// in the file's tab, or in a new document when there is no file (any more).
KTextEditor::View *MainWindow::restoreDocument(const QString &path, const QString &backupName)
{
    const std::optional<Backups::Backup> backup = backupName.isEmpty() ? std::nullopt : Backups::read(m_root, backupName);
    const QString file = backup ? backup->path : path;
    if (backup && !file.isEmpty() && !findView(QFileInfo(file).canonicalFilePath())) {
        // After a crash KTextEditor offers to replay its swap file, which would
        // apply the edits a second time on top of the backup: this replaces it.
        QFile::remove(QFileInfo(file).absolutePath() + QStringLiteral("/.") + QFileInfo(file).fileName() + QStringLiteral(".kate-swp"));
    }
    KTextEditor::View *view = file.isEmpty() ? nullptr : openFile(file);
    if (!backup) {
        return view;
    }
    KTextEditor::Document *doc = view ? view->document() : nullptr;
    if (doc && doc->text() == backup->text) {
        Backups::remove(m_root, backup->name); // saved in the meantime
        return view;
    }
    // Shown until the user has worked in the tab for a while.
    const auto warn = [](KTextEditor::Document *d, const QString &text) {
        auto *message = new KTextEditor::Message(QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped()), KTextEditor::Message::Warning);
        message->setPosition(KTextEditor::Message::AboveView);
        message->setAutoHide(10000);
        message->setAutoHideMode(KTextEditor::Message::AfterUserInteraction);
        d->postMessage(message);
    };
    if (doc && doc->isReadWrite()) {
        doc->setText(backup->text);
        if (backup->diskTime > 0 && QFileInfo(file).lastModified().toMSecsSinceEpoch() != backup->diskTime) {
            doc->setProperty("lieveDiskTime", backup->diskTime);
            warn(doc, tr("The file has changed on disk since these unsaved changes were made: saving them overwrites the new version."));
        }
    } else {
        const bool gone = !doc && !file.isEmpty();
        doc = KTextEditor::Editor::instance()->createDocument(this);
        view = addView(doc);
        m_tabs->setCurrentWidget(view);
        doc->setText(backup->text);
        if (gone) {
            warn(doc, tr("%1 no longer exists: its unsaved changes are in this new document.").arg(QFileInfo(file).fileName()));
        }
    }
    doc->setProperty("lieveBackup", backup->name);
    doc->setProperty("lieveBackupRevision", doc->revision());
    return view;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (hotExit()) {
        // Nothing to ask: the unsaved documents come back with the project.
        if (keepUnsaved()) {
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    saveSession();
    // Unsaved documents: Save / Discard / Cancel for each one.
    for (KTextEditor::Document *doc : documents()) {
        if (doc->isModified()) {
            activateView(doc->views().value(0));
            if (!doc->queryClose()) {
                event->ignore();
                return;
            }
        }
    }
    for (KTextEditor::Document *doc : documents()) {
        doc->setModified(false);
    }
    state().sync();
    event->accept();
}

// ---------------------------------------------------------------------------
// Markdown preview: a tab that is not an editor. It is not part of the
// session (the file is), and it closes with its document.

void MainWindow::openMarkdownPreview(bool toSide)
{
    if (auto *preview = qobject_cast<MarkdownPreview *>(m_tabs->currentWidget())) {
        // On a preview: back to its file, like VS Code's "Show Source".
        KTextEditor::View *source = nullptr;
        for (KTextEditor::View *view : allViews()) {
            if (view->document() == preview->source() && (!source || groupOf(view) == m_tabs)) {
                source = view;
            }
        }
        if (source && !toSide) {
            activateView(source);
            source->setFocus();
        }
        return;
    }
    KTextEditor::View *view = currentView();
    if (!view || !MarkdownPreview::isMarkdown(view->document())) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    TabView *group = m_tabs;
    if (toSide) { // the group on the right, else the one on the left, else a new one
        const qsizetype at = m_groups.indexOf(m_tabs);
        group = at + 1 < m_groups.size() ? m_groups.at(at + 1) : at > 0 ? m_groups.at(at - 1) : nullptr;
        if (!group) {
            group = createGroup();
            QList<int> sizes;
            for (qsizetype i = 0; i < m_groups.size(); ++i) {
                sizes << m_editorSplit->width() / int(m_groups.size());
            }
            m_editorSplit->setSizes(sizes);
        }
    }
    MarkdownPreview *preview = nullptr;
    for (int i = 0; i < group->count() && !preview; ++i) {
        auto *p = qobject_cast<MarkdownPreview *>(group->widget(i));
        preview = p && p->source() == doc ? p : nullptr;
    }
    if (!preview) {
        preview = new MarkdownPreview(doc, group);
        connect(preview, &MarkdownPreview::focused, this, [this, preview] {
            setActiveGroup(groupOfPage(preview));
        });
        connect(preview, &MarkdownPreview::titleChanged, this, [this] {
            updateTabTitles();
            updateWindowTitle();
        });
        connect(preview, &MarkdownView::openFileRequested, this, [this](const QString &path) {
            openFile(path);
        });
        // Closing the file (or the project) closes its preview.
        connect(doc, &QObject::destroyed, this, [this, preview = QPointer<MarkdownPreview>(preview)] {
            if (preview) {
                closeTab(preview);
            }
        });
        const int index = group->count() == 0 ? 0 : group->currentIndex() + 1; // next to the current tab
        group->insertTab(index, preview, QString());
        group->setTabIcon(index, MarkdownPreview::icon());
        updateTabTitles();
    }
    group->setCurrentWidget(preview);
    if (toSide) {
        view->setFocus(); // keep typing, the preview follows
    } else {
        preview->setFocus();
    }
    updateWindowTitle();
}

bool MainWindow::closeTab(QWidget *page)
{
    if (auto *view = qobject_cast<KTextEditor::View *>(page)) {
        return closeView(view);
    }
    TabView *group = groupOfPage(page);
    if (!group) {
        return true; // nothing to close
    }
    const bool focused = page->isAncestorOf(QApplication::focusWidget()) || page->hasFocus();
    group->removeTab(group->indexOf(page));
    delete page;
    if (group->count() == 0) {
        removeGroup(group);
    }
    updateEditorPage(); // a side-by-side diff may have been the last tab
    updateWindowTitle();
    if (focused) {
        if (QWidget *current = m_tabs->currentWidget()) {
            current->setFocus();
        }
    }
    return true;
}

TabView *MainWindow::groupOfPage(QWidget *page) const
{
    for (TabView *group : m_groups) {
        if (page && group->indexOf(page) >= 0) {
            return group;
        }
    }
    return nullptr;
}

// VS Code's button at the right of the tab bar, on Markdown files.
void MainWindow::addPreviewButton(TabView *group)
{
    auto *corner = new QWidget(group);
    corner->setObjectName(QStringLiteral("previewCorner"));
    auto *layout = new QHBoxLayout(corner);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(0);
    auto *button = new QToolButton(corner);
    button->setIcon(MarkdownPreview::icon());
    button->setToolTip(tr("Open Preview to the Side (Ctrl+K V)"));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);
    group->setCornerWidget(corner);
    corner->hide();
    connect(group, &TabView::currentChanged, corner, [group, corner] {
        auto *view = qobject_cast<KTextEditor::View *>(group->currentWidget());
        corner->setVisible(view && MarkdownPreview::isMarkdown(view->document()));
    });
    connect(button, &QToolButton::clicked, this, [this, group] {
        if (auto *view = qobject_cast<KTextEditor::View *>(group->currentWidget())) {
            activateView(view);
            openMarkdownPreview(true);
        }
    });
}

void MainWindow::updateMarkdownActions()
{
    if (!m_previewAction) {
        return;
    }
    KTextEditor::View *view = currentView();
    const bool markdown = view && MarkdownPreview::isMarkdown(view->document());
    m_previewAction->setEnabled(markdown || qobject_cast<MarkdownPreview *>(m_tabs->currentWidget())); // on a preview: back to the file
    m_previewSideAction->setEnabled(markdown);
    for (TabView *group : std::as_const(m_groups)) {
        if (auto *corner = group->findChild<QWidget *>(QStringLiteral("previewCorner"), Qt::FindDirectChildrenOnly)) {
            auto *v = qobject_cast<KTextEditor::View *>(group->currentWidget());
            corner->setVisible(v && MarkdownPreview::isMarkdown(v->document()));
        }
    }
}
