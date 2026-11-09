#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <memory>

class KXMLGUIClient;
class QIcon;
class QKeyEvent;
class QMenu;
class TabView;
namespace KParts
{
class ReadOnlyPart;
}
struct TerminalTest;

// Bottom panel with one or more terminals in tabs: embedded Konsole ones
// (konsolepart: the user's shell, profile and fonts, with the editor's
// colours) or, where the Konsole part is missing or [terminal] engine=builtin,
// Lieve's built-in terminal (BuiltinTerminal) with the same colours.
class TerminalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget *parent = nullptr);
    ~TerminalPanel() override;
    void setWorkingDirectory(const QString &dir);
    // Keys the window keeps even while the terminal has focus (Ctrl+J, Ctrl+P, ...).
    void setReservedShortcuts(const QList<QKeySequence> &keys);
    bool hasTerminals() const;
    // `shell` is a path; empty = the default shell ([terminal] shell=).
    // Returns the new terminal; null if it had to open outside Lieve.
    QWidget *newTerminal(const QString &dir = QString(), const QString &shell = QString());
    // Types `command` into the shell of a new terminal in `dir`, titled
    // `title`; the shell stays open afterwards (↑ Enter runs it again).
    // `reuse`, a terminal it returned before, runs it again if its shell is
    // idle. Returns the terminal (null if it opened outside Lieve).
    QWidget *runCommand(const QString &dir, const QString &title, const QString &command, QWidget *reuse = nullptr);
    void focusCurrent();
    void closeAll();
    // A page that is not a terminal (Problems), shown as the first tab and
    // brought to the front. Its × only takes the tab away: the page is kept.
    void showPage(QWidget *page, const QIcon &icon, const QString &title);
    QWidget *currentPage() const;
    bool isEmpty() const; // no tab at all
    // From a page back to a terminal (a new one if there is none).
    void selectTerminal();
    // True when terminals live in the panel, with either engine (Konsole's
    // part or the built-in terminal); false only where neither can run
    // (Windows older than 10 1903 without Konsole): they open outside Lieve.
    static bool embedded();
    // For [terminal] shell=: the names of the installed shells ("fish") and
    // of the login shell, the one "auto" runs.
    static QStringList shellNames();
    static QString loginShellName();
    // Automated checks (LIEVE_TEST_BUILTIN_TERMINAL): steps separated by '|',
    // see the end of terminalpanel.cpp.
    void runTest(const QString &steps);

Q_SIGNALS:
    void hideRequested();
    void lastTerminalClosed();

private Q_SLOTS:
    void overrideShortcut(QKeyEvent *event, bool &override);

private:
    QWidget *openInPanel(const QString &dir, const QString &shell);
    QWidget *openKonsole(const QString &dir, const QString &shell);
    QWidget *openBuiltin(const QString &dir, const QString &shell);
    bool isReserved(const QKeyEvent *event) const;
    void stripReservedShortcuts(KXMLGUIClient *client);
    void fillShellMenu(QMenu *menu);
    void closeTerminal(int index);
    void terminalGone();
    bool isTerminal(QWidget *widget) const;
    void setTitle(QWidget *terminal, const QString &title);
    bool shellReady(QWidget *terminal) const;
    bool atPrompt(QWidget *terminal) const;
    void sendInput(QWidget *terminal, const QString &text);
    void typeWhenReady(QWidget *terminal, const QString &text);
    void testStep(const std::shared_ptr<TerminalTest> &test);

    TabView *m_tabs = nullptr;
    QString m_dir;
    QList<QKeySequence> m_reserved;
    QHash<QWidget *, QPointer<KParts::ReadOnlyPart>> m_parts; // Konsole's terminals
    QList<QPointer<QWidget>> m_pages; // see showPage()
};
