#include "terminalpanel.h"

#include "builtinterminal.h"
#include "platform.h"
#include "settings.h"
#include "tabview.h"
#include "terminalengine.h"
#include "theme.h"

#include <KActionCollection>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KXMLGUIClient>
#include <kde_terminal_interface.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <unistd.h>
#endif

namespace
{
#ifdef Q_OS_WIN
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

struct Shell {
    QString name; // "fish"
    QString path; // "/usr/bin/fish"
};

bool isExecutable(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

#ifdef Q_OS_WIN
// The shells of a Windows system, in order of preference: PowerShell 7,
// Windows PowerShell, cmd, Git Bash, WSL.
QStringList windowsShells()
{
    const QString system = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"))) + QStringLiteral("/System32");
    const QString programFiles = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files")));
    QStringList shells{
        QStandardPaths::findExecutable(QStringLiteral("pwsh")),
        programFiles + QStringLiteral("/PowerShell/7/pwsh.exe"),
        system + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe"),
        QDir::fromNativeSeparators(qEnvironmentVariable("ComSpec", system + QStringLiteral("/cmd.exe"))),
        programFiles + QStringLiteral("/Git/bin/bash.exe"),
        system + QStringLiteral("/wsl.exe"),
    };
    shells.removeAll(QString());
    return shells;
}
#endif

// The shell chosen for this user (chsh), the one a Konsole window opens.
QString loginShell()
{
#ifdef Q_OS_WIN
    const QStringList shells = windowsShells();
    for (const QString &shell : shells) {
        if (QFileInfo(shell).isFile()) {
            return shell;
        }
    }
    return QStringLiteral("cmd.exe");
#else
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_shell && isExecutable(QString::fromLocal8Bit(pw->pw_shell))) {
        return QString::fromLocal8Bit(pw->pw_shell);
    }
    const QString env = qEnvironmentVariable("SHELL");
    return isExecutable(env) ? env : QStringLiteral("/bin/sh");
#endif
}

// Interactive shells listed in /etc/shells, plus the login shell and $SHELL.
// Paths that lead to the same program (/bin/bash, /usr/bin/bash, /bin/sh on
// Arch) appear once.
QList<Shell> availableShells()
{
    static const QStringList skip = {QStringLiteral("rbash"), QStringLiteral("git-shell"), QStringLiteral("nologin"),
                                     QStringLiteral("false"), QStringLiteral("systemd-home-fallback-shell")};
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates = windowsShells();
#endif
    QFile file(QStringLiteral("/etc/shells"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromLocal8Bit(file.readAll()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString path = line.trimmed();
            if (path.startsWith(QLatin1Char('/'))) {
                candidates << path;
            }
        }
    }
    candidates << loginShell() << qEnvironmentVariable("SHELL");

    QList<Shell> shells;
    QStringList seen;
    for (const QString &path : std::as_const(candidates)) {
        if (!isExecutable(path)) {
            continue;
        }
        const QString real = QFileInfo(path).canonicalFilePath();
        const QString name = Lieve::Terminal::shellName(real, kWindows); // "pwsh", not "pwsh.exe"
        if (seen.contains(real) || skip.contains(name) || skip.contains(QFileInfo(path).fileName())) {
            continue;
        }
        seen << real;
        shells.append({name, real});
    }
    std::sort(shells.begin(), shells.end(), [](const Shell &a, const Shell &b) {
        return a.name < b.name;
    });
    return shells;
}

// [terminal] shell=: "auto" (or empty) = the login shell; otherwise a name
// ("fish") or a path. Falls back to the login shell if it cannot be found.
QString shellSetting()
{
    return Lieve::config().value(QStringLiteral("terminal/shell"), QStringLiteral("auto")).toString().trimmed();
}

QString resolveShell(const QString &setting)
{
    if (setting.isEmpty() || setting == QLatin1String("auto")) {
        return loginShell();
    }
    const bool isPath = setting.contains(QLatin1Char('/')) || setting.contains(QLatin1Char('\\'));
    const QString path = isPath ? QDir::fromNativeSeparators(setting) : QStandardPaths::findExecutable(setting);
    return isExecutable(path) ? path : loginShell();
}

bool konsoleAvailable()
{
    static const bool available = KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")).isValid();
    return available;
}

// [terminal] engine= (read at each new terminal), with Konsole's part if it is there.
Lieve::Terminal::Engine engine()
{
    if (qEnvironmentVariableIsSet("LIEVE_TEST_BUILTIN_TERMINAL")) { // the checks: the built-in one even where Konsole is
        return Lieve::Terminal::Engine::Builtin;
    }
    const QString setting = Lieve::config().value(QStringLiteral("terminal/engine"), QStringLiteral("auto")).toString();
    return Lieve::Terminal::chooseEngine(setting, konsoleAvailable());
}

QIcon terminalIcon()
{
    return QIcon::fromTheme(QStringLiteral("utilities-terminal"));
}
}

bool TerminalPanel::embedded()
{
    return konsoleAvailable() || BuiltinTerminal::available();
}

TerminalPanel::TerminalPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new TabView(this);
    layout->addWidget(m_tabs);

    auto *corner = new QWidget(m_tabs);
    auto *cl = new QHBoxLayout(corner);
    cl->setContentsMargins(0, 0, 4, 0);
    cl->setSpacing(0);
    auto *add = new QToolButton(corner);
    add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    add->setToolTip(tr("New Terminal (Ctrl+Shift+`)"));
    add->setAutoRaise(true);
    // Like VS Code's "Launch Profile..." next to +: a terminal with another
    // shell, or the default shell.
    auto *shells = new QToolButton(corner);
    shells->setObjectName(QStringLiteral("shellMenu"));
    shells->setIcon(Lieve::Theme::glyph(Lieve::Theme::Glyph::ChevronDown));
    shells->setToolTip(tr("Choose Shell"));
    shells->setAutoRaise(true);
    shells->setPopupMode(QToolButton::InstantPopup);
    auto *shellMenu = new QMenu(shells);
    connect(shellMenu, &QMenu::aboutToShow, this, [this, shellMenu] {
        fillShellMenu(shellMenu);
    });
    shells->setMenu(shellMenu);
    auto *hide = new QToolButton(corner);
    hide->setIcon(QIcon::fromTheme(QStringLiteral("go-down"), QIcon::fromTheme(QStringLiteral("arrow-down"))));
    hide->setToolTip(tr("Hide Panel (Ctrl+J)"));
    hide->setAutoRaise(true);
    cl->addWidget(add);
    cl->addWidget(shells);
    cl->addWidget(hide);
    m_tabs->setCornerWidget(corner);

    connect(add, &QToolButton::clicked, this, [this] {
        newTerminal();
    });
    connect(hide, &QToolButton::clicked, this, &TerminalPanel::hideRequested);
    connect(m_tabs, &TabView::tabCloseRequested, this, &TerminalPanel::closeTerminal);
}

TerminalPanel::~TerminalPanel()
{
    // Close the terminals while this panel still exists: their "destroyed"
    // handlers must not run against a half-destroyed panel.
    closeAll();
}

void TerminalPanel::closeAll()
{
    const auto parts = m_parts.values();
    m_parts.clear();
    for (const QPointer<KParts::ReadOnlyPart> &part : parts) {
        if (part) {
            part->disconnect(this);
            delete part.data(); // ends the shell, removes its tab
        }
    }
    while (m_tabs->count() > 0) { // built-in terminals and pages
        QWidget *w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        if (!m_pages.contains(w)) {
            w->disconnect(this);
            delete w; // a built-in terminal ends its shell
        }
    }
}

void TerminalPanel::showPage(QWidget *page, const QIcon &icon, const QString &title)
{
    if (!m_pages.contains(page)) {
        m_pages.append(page);
    }
    if (m_tabs->indexOf(page) < 0) {
        m_tabs->insertTab(0, page, title);
        m_tabs->setTabIcon(0, icon);
    }
    m_tabs->setCurrentWidget(page);
}

QWidget *TerminalPanel::currentPage() const
{
    return m_tabs->currentWidget();
}

bool TerminalPanel::isEmpty() const
{
    return m_tabs->count() == 0;
}

void TerminalPanel::selectTerminal()
{
    QWidget *current = m_tabs->currentWidget();
    if (!current || !m_pages.contains(current)) {
        return; // already a terminal, or nothing yet (the caller opens one)
    }
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        if (!m_pages.contains(m_tabs->widget(i))) {
            m_tabs->setCurrentIndex(i);
            return;
        }
    }
    newTerminal();
}

void TerminalPanel::setWorkingDirectory(const QString &dir)
{
    m_dir = dir;
}

void TerminalPanel::setReservedShortcuts(const QList<QKeySequence> &keys)
{
    m_reserved = keys;
}

bool TerminalPanel::hasTerminals() const
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!m_pages.contains(m_tabs->widget(i))) {
            return true;
        }
    }
    return false;
}

void TerminalPanel::fillShellMenu(QMenu *menu)
{
    menu->clear();
    const QList<Shell> shells = availableShells();
    const QString setting = shellSetting();
    const QString current = QFileInfo(resolveShell(setting)).canonicalFilePath();
    const QIcon icon = terminalIcon();

    for (const Shell &shell : shells) {
        const QString text = shell.path == current ? tr("%1  (default)").arg(shell.name) : shell.name;
        connect(menu->addAction(icon, text), &QAction::triggered, this, [this, path = shell.path] {
            newTerminal(QString(), path);
        });
    }

    // The default shell is saved in lieve.conf ([terminal] shell=) and used by
    // every new terminal, in every window.
    menu->addSeparator();
    QMenu *defaults = menu->addMenu(tr("Default Shell"));
    auto *group = new QActionGroup(defaults);
    const auto addChoice = [&](const QString &text, const QString &value, bool checked) {
        QAction *action = defaults->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [value] {
            Lieve::setConfigValue(QStringLiteral("terminal"), QStringLiteral("shell"), value,
                                  tr("Shell for new terminals: auto (your login shell) or fish, zsh, bash or a path."));
        });
    };
    const bool automatic = setting.isEmpty() || setting == QLatin1String("auto");
    addChoice(tr("Automatic (%1, your login shell)").arg(QFileInfo(loginShell()).fileName()), QStringLiteral("auto"), automatic);
    defaults->addSeparator();
    for (const Shell &shell : shells) {
        addChoice(shell.name, shell.name, !automatic && shell.path == current);
    }
}

QWidget *TerminalPanel::newTerminal(const QString &dir, const QString &shellPath)
{
    const QString shell = shellPath.isEmpty() ? resolveShell(shellSetting()) : shellPath;
    const QString workDir = !dir.isEmpty() ? dir : (!m_dir.isEmpty() ? m_dir : QDir::homePath());
    QWidget *terminal = openInPanel(workDir, shell);
    if (!terminal) {
        // The last resort: Windows Terminal, or the desktop's terminal.
        Lieve::Platform::openTerminal(workDir, shell, QString(), QString());
    }
    return terminal;
}

QWidget *TerminalPanel::openInPanel(const QString &dir, const QString &shell)
{
    const bool builtin = BuiltinTerminal::available();
    QWidget *terminal = nullptr;
    if (konsoleAvailable() && (engine() == Lieve::Terminal::Engine::Konsole || !builtin)) {
        terminal = openKonsole(dir, shell);
    }
    if (!terminal && builtin) {
        terminal = openBuiltin(dir, shell);
    }
    if (terminal) {
        terminal->setFocus();
    }
    return terminal;
}

QWidget *TerminalPanel::openKonsole(const QString &dir, const QString &shell)
{
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), this);
    if (!result.plugin) {
        qWarning("lieve: the Konsole terminal could not be loaded (%s): the built-in one is used", qPrintable(result.errorString));
        return nullptr;
    }

    KParts::ReadOnlyPart *part = result.plugin;
    QWidget *widget = part->widget();
    const int index = m_tabs->addTab(widget, terminalIcon(), Lieve::Terminal::shellName(shell, kWindows));
    m_tabs->setCurrentIndex(index);
    m_parts.insert(widget, part);

    // Let the window keep its own shortcuts while the terminal has focus.
    connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), this, SLOT(overrideShortcut(QKeyEvent *, bool &)));
    connect(part, &KParts::Part::setWindowCaption, this, [this, widget](const QString &caption) {
        setTitle(widget, caption);
    });
    // Konsole deletes the part when the shell exits ("exit" or Ctrl+D).
    connect(part, &QObject::destroyed, this, [this, widget] {
        m_parts.remove(widget);
        terminalGone();
    });

    if (auto *terminal = qobject_cast<TerminalInterface *>(part)) {
        // The profile runs the chosen shell, with the editor's colours.
        const QString profile = Lieve::Theme::konsoleProfile(shell);
        if (profile.isEmpty() || !terminal->setCurrentProfile(profile)) {
            qWarning("lieve: Konsole profile for %s not applied", qPrintable(shell));
        }
        terminal->showShellInDir(dir);
    }
    stripReservedShortcuts(part);
    QTimer::singleShot(0, this, [this, guard = QPointer<KParts::ReadOnlyPart>(part)] {
        if (guard) {
            stripReservedShortcuts(guard);
        }
    });
    return widget;
}

QWidget *TerminalPanel::openBuiltin(const QString &dir, const QString &shell)
{
    auto *terminal = new BuiltinTerminal(m_tabs);
    terminal->setReservedKeys([this](const QKeyEvent *event) {
        return isReserved(event);
    });
    const int index = m_tabs->addTab(terminal, terminalIcon(), Lieve::Terminal::shellName(shell, kWindows));
    m_tabs->setCurrentIndex(index);
    if (!terminal->start(shell, dir)) {
        qWarning("lieve: the built-in terminal could not start %s: %s", qPrintable(shell), qPrintable(terminal->errorString()));
        m_tabs->removeTab(m_tabs->indexOf(terminal));
        delete terminal;
        return nullptr;
    }
    connect(terminal, &BuiltinTerminal::titleChanged, this, [this, terminal](const QString &title) {
        setTitle(terminal, title);
    });
    // The shell has exited ("exit" or Ctrl+D): its tab goes, as Konsole's does.
    connect(terminal, &BuiltinTerminal::finished, this, [this, terminal] {
        const int i = m_tabs->indexOf(terminal);
        if (i >= 0) {
            m_tabs->removeTab(i);
        }
        terminal->disconnect(this);
        terminal->deleteLater();
        terminalGone();
    });
    return terminal;
}

void TerminalPanel::terminalGone()
{
    QTimer::singleShot(0, this, [this] {
        if (m_tabs->count() == 0) {
            Q_EMIT lastTerminalClosed();
        }
    });
}

void TerminalPanel::setTitle(QWidget *terminal, const QString &title)
{
    const int i = m_tabs->indexOf(terminal);
    if (i >= 0 && !title.trimmed().isEmpty()) {
        m_tabs->setTabText(i, Lieve::Terminal::tabTitle(title));
        m_tabs->setTabToolTip(i, title);
    }
}

bool TerminalPanel::isTerminal(QWidget *widget) const
{
    return widget && m_tabs->indexOf(widget) >= 0 && (m_parts.contains(widget) || qobject_cast<BuiltinTerminal *>(widget));
}

bool TerminalPanel::shellReady(QWidget *terminal) const
{
    if (auto *builtin = qobject_cast<BuiltinTerminal *>(terminal)) {
        return builtin->shellReady();
    }
    auto *konsole = qobject_cast<TerminalInterface *>(m_parts.value(terminal).data());
    return konsole && Lieve::Platform::shellReady(konsole->terminalProcessId());
}

bool TerminalPanel::atPrompt(QWidget *terminal) const
{
    if (auto *builtin = qobject_cast<BuiltinTerminal *>(terminal)) {
        return builtin->atPrompt();
    }
    auto *konsole = qobject_cast<TerminalInterface *>(m_parts.value(terminal).data());
    return konsole && konsole->foregroundProcessId() == -1;
}

void TerminalPanel::sendInput(QWidget *terminal, const QString &text)
{
    if (auto *builtin = qobject_cast<BuiltinTerminal *>(terminal)) {
        builtin->sendText(text);
    } else if (auto *konsole = qobject_cast<TerminalInterface *>(m_parts.value(terminal).data())) {
        konsole->sendInput(text);
    }
}

// Types `text` once the shell is ready (checked every 30 ms, for 5 s at most):
// typed before, a command would be shown twice.
void TerminalPanel::typeWhenReady(QWidget *terminal, const QString &text)
{
    if (shellReady(terminal)) {
        sendInput(terminal, text);
        return;
    }
    auto *timer = new QTimer(terminal);
    QElapsedTimer waited;
    waited.start();
    connect(timer, &QTimer::timeout, terminal, [this, timer, terminal, text, waited] {
        if (shellReady(terminal) || waited.hasExpired(5000)) {
            timer->deleteLater();
            sendInput(terminal, text);
        }
    });
    timer->start(30);
}

QWidget *TerminalPanel::runCommand(const QString &dir, const QString &title, const QString &command, QWidget *reuse)
{
    QWidget *terminal = isTerminal(reuse) && atPrompt(reuse) ? reuse : nullptr; // else closed, or still running something
    if (!terminal) {
        const QString shell = resolveShell(shellSetting());
        const QString workDir = !dir.isEmpty() ? dir : (!m_dir.isEmpty() ? m_dir : QDir::homePath());
        terminal = openInPanel(workDir, shell);
        if (!terminal) {
            Lieve::Platform::openTerminal(workDir, shell, title, command);
            return nullptr;
        }
        // The tab keeps the command's name instead of the shell's title.
        if (KParts::ReadOnlyPart *part = m_parts.value(terminal)) {
            disconnect(part, &KParts::Part::setWindowCaption, this, nullptr);
        } else if (auto *builtin = qobject_cast<BuiltinTerminal *>(terminal)) {
            disconnect(builtin, &BuiltinTerminal::titleChanged, this, nullptr);
        }
        const int index = m_tabs->indexOf(terminal);
        m_tabs->setTabText(index, title);
        m_tabs->setTabToolTip(index, command);
    }
    m_tabs->setCurrentWidget(terminal);
    // As if typed: it stays in the shell's history.
    typeWhenReady(terminal, command + QLatin1Char('\n'));
    return terminal;
}

void TerminalPanel::focusCurrent()
{
    if (QWidget *w = m_tabs->currentWidget()) {
        w->setFocus();
    }
}

void TerminalPanel::closeTerminal(int index)
{
    QWidget *widget = m_tabs->widget(index);
    if (!widget) {
        return;
    }
    if (KParts::ReadOnlyPart *part = m_parts.value(widget)) {
        delete part; // also deletes its widget and ends the shell
    } else {
        m_tabs->removeTab(index);
        if (!m_pages.contains(widget)) {
            widget->disconnect(this);
            widget->deleteLater(); // a built-in terminal ends its shell
        }
        if (m_tabs->count() == 0) {
            Q_EMIT lastTerminalClosed();
        }
    }
}

bool TerminalPanel::isReserved(const QKeyEvent *event) const
{
    const QKeySequence pressed(QKeyCombination(event->modifiers() & ~Qt::KeypadModifier, Qt::Key(event->key())));
    for (const QKeySequence &key : std::as_const(m_reserved)) {
        if (key.matches(pressed) == QKeySequence::ExactMatch) {
            return true;
        }
    }
    return false;
}

void TerminalPanel::overrideShortcut(QKeyEvent *event, bool &override)
{
    if (isReserved(event)) {
        override = false; // let the window handle it
    }
}

void TerminalPanel::stripReservedShortcuts(KXMLGUIClient *client)
{
    if (!client) {
        return;
    }
    if (KActionCollection *collection = client->actionCollection()) {
        const QList<QAction *> actions = collection->actions();
        for (QAction *action : actions) {
            QList<QKeySequence> shortcuts = action->shortcuts();
            const qsizetype before = shortcuts.size();
            shortcuts.removeIf([this](const QKeySequence &k) {
                return m_reserved.contains(k);
            });
            if (shortcuts.size() != before) {
                action->setShortcuts(shortcuts);
            }
        }
    }
    const QList<KXMLGUIClient *> children = client->childClients();
    for (KXMLGUIClient *child : children) {
        stripReservedShortcuts(child);
    }
}

QStringList TerminalPanel::shellNames()
{
    QStringList names;
    const QList<Shell> shells = availableShells();
    for (const Shell &shell : shells) {
        names << shell.name;
    }
    return names;
}

QString TerminalPanel::loginShellName()
{
    return QFileInfo(loginShell()).fileName();
}

// ---------------------------------------------------------------------------
// LIEVE_TEST_BUILTIN_TERMINAL=step|step...: the terminals are built-in ones,
// even where Konsole is installed; one step every 300 ms, from 1 s after the
// panel opens. Steps:
//   shell:<name or path>    a new terminal with that shell
//   type:<text>             typed (and Enter) once the shell is ready, as runCommand types
//   keys:<text>             key presses for each character, then Enter
//   key:<sequence>          one key (Ctrl+Shift+V, Return, Ctrl+J...): to the shell,
//                           or to the window's action if the window keeps it
//   override:<sequence>     whether the shell gets that key or the window keeps it
//   clipboard:<text>        puts text on the clipboard
//   copy                    selects all and copies, prints the clipboard
//   run:<title>:<command>   runCommand(), reusing the terminal the last run returned
//   dump                    prints the lines on screen of the current terminal
//   tabs                    prints the tabs (* = current) and their engine
//   close                   closes the current tab, as its × does
//   shot                    saves the panel next to LIEVE_SCREENSHOT (-terminal<n>.png)
//   wait:<ms>
// "1" (or nothing) runs the default sequence below.
struct TerminalTest {
    QStringList steps;
    qsizetype next = 0;
    QPointer<QWidget> lastRun;
    int shots = 0;
};

void TerminalPanel::runTest(const QString &steps)
{
    auto test = std::make_shared<TerminalTest>();
    test->steps = steps.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (test->steps.isEmpty() || test->steps == QStringList{QStringLiteral("1")}) {
        test->steps = QStringLiteral("shell:bash|type:echo lieve-$((6*7))|wait:1500|dump"
                                     "|keys:echo typed-$((2+3))|wait:1000|dump"
                                     "|override:Ctrl+J|override:Ctrl+P|override:Ctrl+C|override:Ctrl+Shift+V"
                                     "|clipboard:echo pasted-text|key:Ctrl+Shift+V|key:Return|wait:1000|dump|shot"
                                     "|type:exit|wait:1500|tabs"
                                     "|run:build:echo npm-run-build|wait:2000|tabs|dump"
                                     "|run:build:echo npm-run-again|wait:2000|tabs|dump|shot"
                                     "|close|tabs")
                          .split(QLatin1Char('|'));
    }
    QTimer::singleShot(1000, this, [this, test] {
        testStep(test);
    });
}

void TerminalPanel::testStep(const std::shared_ptr<TerminalTest> &test)
{
    if (test->next >= test->steps.size()) {
        qInfo().noquote() << "builtin-terminal done";
        return;
    }
    const QString step = test->steps.at(test->next++);
    const QString command = step.section(QLatin1Char(':'), 0, 0);
    const QString argument = step.section(QLatin1Char(':'), 1);
    QWidget *current = m_tabs->currentWidget();
    auto *builtin = qobject_cast<BuiltinTerminal *>(current);
    const auto name = [this](QWidget *w) {
        const int i = m_tabs->indexOf(w);
        return i < 0 ? QStringLiteral("(none)") : m_tabs->tabBar()->tabText(i);
    };
    // A key event the way the window system delivers it: first asked whether
    // it is a shortcut, then pressed (or the window's action runs).
    const auto press = [this, current](const QKeyCombination &combination, const QString &text) {
        QWidget *target = current && current->focusWidget() ? current->focusWidget() : current;
        if (!target) {
            return QStringLiteral("no terminal");
        }
        QKeyEvent override(QEvent::ShortcutOverride, combination.key(), combination.keyboardModifiers(), text);
        override.ignore();
        QCoreApplication::sendEvent(target, &override);
        if (!override.isAccepted()) {
            const QKeySequence sequence(combination);
            const auto actions = window()->actions();
            for (QAction *action : actions) {
                if (action->shortcuts().contains(sequence)) {
                    action->trigger();
                    return QStringLiteral("window (%1)").arg(action->text());
                }
            }
            return QStringLiteral("window (no action)");
        }
        QKeyEvent down(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(), text);
        QKeyEvent up(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers(), text);
        QCoreApplication::sendEvent(target, &down);
        QCoreApplication::sendEvent(target, &up);
        return QStringLiteral("shell");
    };
    // What a key types: a printable character without Ctrl or Alt.
    const auto typed = [](const QKeyCombination &combination) {
        const int key = combination.key();
        const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            return QStringLiteral("\r");
        }
        if (key < 0x20 || key > 0x7e || (modifiers & (Qt::ControlModifier | Qt::AltModifier))) {
            return QString();
        }
        const QChar c(key);
        return QString(modifiers & Qt::ShiftModifier ? c.toUpper() : c.toLower());
    };
    int delay = 300;

    if (command == QLatin1String("shell")) {
        const bool path = argument.contains(QLatin1Char('/')) || argument.contains(QLatin1Char('\\'));
        const QString shell = path ? argument : QStandardPaths::findExecutable(argument);
        QWidget *terminal = newTerminal(QString(), shell);
        qInfo().noquote() << "builtin-terminal shell" << (shell.isEmpty() ? argument + QStringLiteral(" (not found: the default)") : shell) << "->"
                          << (qobject_cast<BuiltinTerminal *>(terminal) ? "built-in" : terminal ? "konsole" : "outside Lieve");
    } else if (command == QLatin1String("type") && current) {
        typeWhenReady(current, argument + QLatin1Char('\n'));
    } else if (command == QLatin1String("keys")) {
        for (const QChar c : argument) {
            const int key = c.isLetter() ? c.toUpper().unicode() : c.unicode();
            press(QKeyCombination(c.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, Qt::Key(key)), QString(c));
        }
        press(QKeyCombination(Qt::Key_Return), QStringLiteral("\r"));
    } else if (command == QLatin1String("key")) {
        const QKeySequence sequence = QKeySequence::fromString(argument, QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            qInfo().noquote() << "builtin-terminal key" << argument << "-> not a key";
        } else {
            qInfo().noquote() << "builtin-terminal key" << argument << "->" << press(sequence[0], typed(sequence[0]));
        }
    } else if (command == QLatin1String("override")) {
        const QKeySequence sequence = QKeySequence::fromString(argument, QKeySequence::PortableText);
        QWidget *target = current && current->focusWidget() ? current->focusWidget() : current;
        if (target && !sequence.isEmpty()) {
            QKeyEvent event(QEvent::ShortcutOverride, sequence[0].key(), sequence[0].keyboardModifiers());
            event.ignore();
            QCoreApplication::sendEvent(target, &event);
            qInfo().noquote() << "builtin-terminal override" << argument << "->" << (event.isAccepted() ? "shell" : "window");
        }
    } else if (command == QLatin1String("clipboard")) {
        QApplication::clipboard()->setText(argument);
    } else if (command == QLatin1String("copy") && builtin) {
        builtin->selectAll();
        builtin->copyToClipboard();
        const QStringList lines = QApplication::clipboard()->text().split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            qInfo().noquote() << "builtin-terminal clipboard|" << line.trimmed();
        }
    } else if (command == QLatin1String("run")) {
        const QString title = argument.section(QLatin1Char(':'), 0, 0);
        const QString line = argument.section(QLatin1Char(':'), 1);
        QWidget *reuse = test->lastRun.data();
        test->lastRun = runCommand(m_dir, title, line, reuse);
        qInfo().noquote() << "builtin-terminal run" << title << line << "->"
                          << (!test->lastRun ? "outside Lieve" : test->lastRun == reuse ? "reused its terminal" : "new terminal");
    } else if (command == QLatin1String("dump")) {
        if (builtin) {
            const QStringList lines = builtin->screenLines();
            qInfo().noquote() << "builtin-terminal dump" << name(builtin) << "(" << builtin->shell() << ")," << lines.size() << "lines";
            for (const QString &line : lines) {
                qInfo().noquote() << "builtin-terminal |" << line;
            }
        } else {
            qInfo().noquote() << "builtin-terminal dump: the current tab is no built-in terminal" << name(current);
        }
    } else if (command == QLatin1String("tabs")) {
        QStringList tabs;
        for (int i = 0; i < m_tabs->count(); ++i) {
            QWidget *w = m_tabs->widget(i);
            const QString kind = qobject_cast<BuiltinTerminal *>(w) ? QStringLiteral("built-in") : m_parts.contains(w) ? QStringLiteral("konsole") : QStringLiteral("page");
            tabs << (i == m_tabs->currentIndex() ? QStringLiteral("*") : QString()) + m_tabs->tabBar()->tabText(i) + QStringLiteral(" (") + kind + QLatin1Char(')');
        }
        qInfo().noquote() << "builtin-terminal tabs [" + tabs.join(QStringLiteral(", ")) + QLatin1Char(']') << "panel" << (isVisible() ? "shown" : "hidden");
    } else if (command == QLatin1String("close")) {
        qInfo().noquote() << "builtin-terminal close" << name(current);
        closeTerminal(m_tabs->currentIndex());
    } else if (command == QLatin1String("shot")) {
        QString file = qEnvironmentVariable("LIEVE_SCREENSHOT");
        if (!file.isEmpty()) {
            file.replace(QStringLiteral(".png"), QStringLiteral("-terminal%1.png").arg(test->shots++));
            grab().save(file);
            qInfo().noquote() << "builtin-terminal shot" << file;
        }
    } else if (command == QLatin1String("wait")) {
        delay = argument.toInt();
    } else {
        qInfo().noquote() << "builtin-terminal step" << step << "not understood here (" << name(current) << ")";
    }
    QTimer::singleShot(delay, this, [this, test] {
        testStep(test);
    });
}
