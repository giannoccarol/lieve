#include "terminalpanel.h"

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
#include <QLabel>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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
    add->setToolTip(QStringLiteral("Nuovo terminale (Ctrl+Shift+`)"));
    add->setAutoRaise(true);
    auto *hide = new QToolButton(corner);
    hide->setIcon(QIcon::fromTheme(QStringLiteral("go-down"), QIcon::fromTheme(QStringLiteral("arrow-down"))));
    hide->setToolTip(QStringLiteral("Nascondi pannello (Ctrl+J)"));
    hide->setAutoRaise(true);
    cl->addWidget(add);
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
    while (m_tabs->count() > 0) { // error tabs, if any
        QWidget *w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete w;
    }
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
    return m_tabs->count() > 0;
}

void TerminalPanel::newTerminal(const QString &dir)
{
    const QString workDir = !dir.isEmpty() ? dir : (!m_dir.isEmpty() ? m_dir : QDir::homePath());
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), this);
    if (!result.plugin) {
        auto *error = new QLabel(QStringLiteral("Impossibile caricare il terminale di Konsole.\n%1\n\nInstalla Konsole: pacman -S konsole")
                                     .arg(result.errorString),
                                 m_tabs);
        error->setAlignment(Qt::AlignCenter);
        m_tabs->setCurrentIndex(m_tabs->addTab(error, QIcon(), QStringLiteral("Errore")));
        return;
    }

    KParts::ReadOnlyPart *part = result.plugin;
    QWidget *widget = part->widget();
    const QString shell = QFileInfo(qEnvironmentVariable("SHELL", QStringLiteral("sh"))).fileName();
    const int index = m_tabs->addTab(widget, QIcon::fromTheme(QStringLiteral("utilities-terminal")), shell);
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
        if (!Lieve::Theme::konsoleProfile().isEmpty()) {
            terminal->setCurrentProfile(Lieve::Theme::konsoleProfile()); // same colours as the editor
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
        widget->deleteLater();
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
