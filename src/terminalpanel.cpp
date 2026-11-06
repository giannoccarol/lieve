#include "terminalpanel.h"

#include "settings.h"
#include "tabview.h"
#include "theme.h"

#include <KActionCollection>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KXMLGUIClient>
#include <kde_terminal_interface.h>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QActionGroup>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <pwd.h>
#include <unistd.h>

namespace
{
struct Shell {
    QString name; // "fish"
    QString path; // "/usr/bin/fish"
};

bool isExecutable(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

// The shell chosen for this user (chsh), the one a Konsole window opens.
QString loginShell()
{
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_shell && isExecutable(QString::fromLocal8Bit(pw->pw_shell))) {
        return QString::fromLocal8Bit(pw->pw_shell);
    }
    const QString env = qEnvironmentVariable("SHELL");
    return isExecutable(env) ? env : QStringLiteral("/bin/sh");
}

// Interactive shells listed in /etc/shells, plus the login shell and $SHELL.
// Paths that lead to the same program (/bin/bash, /usr/bin/bash, /bin/sh on
// Arch) appear once.
QList<Shell> availableShells()
{
    static const QStringList skip = {QStringLiteral("rbash"), QStringLiteral("git-shell"), QStringLiteral("nologin"),
                                     QStringLiteral("false"), QStringLiteral("systemd-home-fallback-shell")};
    QStringList candidates;
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
        const QString name = QFileInfo(real).fileName();
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
    const QString path = setting.contains(QLatin1Char('/')) ? setting : QStandardPaths::findExecutable(setting);
    return isExecutable(path) ? path : loginShell();
}
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
    while (m_tabs->count() > 0) { // error tabs and pages, if any
        QWidget *w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        if (!m_pages.contains(w)) {
            delete w;
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
    const QIcon icon = QIcon::fromTheme(QStringLiteral("utilities-terminal"));

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

void TerminalPanel::newTerminal(const QString &dir, const QString &shellPath)
{
    const QString shell = shellPath.isEmpty() ? resolveShell(shellSetting()) : shellPath;
    const QString workDir = !dir.isEmpty() ? dir : (!m_dir.isEmpty() ? m_dir : QDir::homePath());
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), this);
    if (!result.plugin) {
        auto *error = new QLabel(tr("Could not load the Konsole terminal.\n%1\n\nInstall Konsole: pacman -S konsole")
                                     .arg(result.errorString),
                                 m_tabs);
        error->setAlignment(Qt::AlignCenter);
        m_tabs->setCurrentIndex(m_tabs->addTab(error, QIcon(), tr("Error")));
        return;
    }

    KParts::ReadOnlyPart *part = result.plugin;
    QWidget *widget = part->widget();
    const int index = m_tabs->addTab(widget, QIcon::fromTheme(QStringLiteral("utilities-terminal")), QFileInfo(shell).fileName());
    m_tabs->setCurrentIndex(index);
    m_parts.insert(widget, part);

    // Let the window keep its own shortcuts while the terminal has focus.
    connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), this, SLOT(overrideShortcut(QKeyEvent *, bool &)));
    connect(part, &KParts::Part::setWindowCaption, this, [this, widget](const QString &caption) {
        const int i = m_tabs->indexOf(widget);
        if (i >= 0 && !caption.isEmpty()) {
            m_tabs->setTabText(i, caption.size() > 40 ? caption.left(39) + QStringLiteral("…") : caption);
            m_tabs->setTabToolTip(i, caption);
        }
    });
    // Konsole deletes the part when the shell exits ("exit" or Ctrl+D).
    connect(part, &QObject::destroyed, this, [this, widget] {
        m_parts.remove(widget);
        QTimer::singleShot(0, this, [this] {
            if (m_tabs->count() == 0) {
                Q_EMIT lastTerminalClosed();
            }
        });
    });

    if (auto *terminal = qobject_cast<TerminalInterface *>(part)) {
        // The profile runs the chosen shell, with the editor's colours.
        const QString profile = Lieve::Theme::konsoleProfile(shell);
        if (profile.isEmpty() || !terminal->setCurrentProfile(profile)) {
            qWarning("lieve: Konsole profile for %s not applied", qPrintable(shell));
        }
        terminal->showShellInDir(workDir);
    }
    stripReservedShortcuts(part);
    QTimer::singleShot(0, this, [this, guard = QPointer<KParts::ReadOnlyPart>(part)] {
        if (guard) {
            stripReservedShortcuts(guard);
        }
    });
    widget->setFocus();
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
            widget->deleteLater();
        }
        if (m_tabs->count() == 0) {
            Q_EMIT lastTerminalClosed();
        }
    }
}

void TerminalPanel::overrideShortcut(QKeyEvent *event, bool &override)
{
    const QKeySequence pressed(QKeyCombination(event->modifiers() & ~Qt::KeypadModifier, Qt::Key(event->key())));
    for (const QKeySequence &key : std::as_const(m_reserved)) {
        if (key.matches(pressed) == QKeySequence::ExactMatch) {
            override = false; // let the window handle it
            return;
        }
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
