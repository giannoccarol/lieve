#include "builtinterminal.h"

#include "3rdparty/ptyqt/ptyqt.h"
#include "platform.h"
#include "settings.h"
#include "terminalengine.h"
#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>
#include <array>

namespace
{
#ifdef Q_OS_WIN
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// The view's 20 colours: the 16 ANSI ones of Lieve's Konsole profile, then
// text, background, selection and search match from the editor's theme.
std::array<QColor, 20> terminalColors()
{
    const Lieve::Colors &c = Lieve::Theme::colors();
    const QList<QColor> ansi = Lieve::Theme::ansiColors(c.dark);
    std::array<QColor, 20> colors;
    for (int i = 0; i < 16; ++i) {
        colors[size_t(i)] = ansi.value(i);
    }
    colors[size_t(TerminalSolution::TerminalView::WidgetColorIdx::Foreground)] = c.fg.isValid() ? c.fg : QColor(Qt::lightGray);
    colors[size_t(TerminalSolution::TerminalView::WidgetColorIdx::Background)] = c.chrome.isValid() ? c.chrome : QColor(Qt::black);
    colors[size_t(TerminalSolution::TerminalView::WidgetColorIdx::Selection)] = c.selection.isValid() ? c.selection : QColor(0x26, 0x4f, 0x78);
    QColor match = c.accent.isValid() ? c.accent : QColor(0x3d, 0xae, 0xe9);
    match.setAlphaF(0.4f);
    colors[size_t(TerminalSolution::TerminalView::WidgetColorIdx::FindMatch)] = match;
    return colors;
}

// The editor's font: [editor] fontFamily= and fontSize= over the fixed-width one.
QFont editorFont()
{
    QFont font = Lieve::Platform::fixedFont();
    const QString family = Lieve::config().value(QStringLiteral("editor/fontFamily")).toString().trimmed();
    const double size = Lieve::config().value(QStringLiteral("editor/fontSize"), 0).toDouble();
    if (!family.isEmpty()) {
        font.setFamily(family);
    }
    if (size > 0) {
        font.setPointSizeF(size);
    }
    font.setStyleHint(QFont::Monospace);
    return font;
}

bool matches(const QKeyEvent *event, Qt::KeyboardModifiers modifiers, Qt::Key key)
{
    return (event->modifiers() & ~Qt::KeypadModifier) == modifiers && event->key() == key;
}
}

BuiltinTerminal::BuiltinTerminal(QWidget *parent)
    : TerminalSolution::TerminalView(parent)
{
    setFrameShape(QFrame::NoFrame);
    // The margin of Lieve's Konsole profile (TerminalMargin=6).
    setViewportMargins(6, 6, 2, 6);
    setSurfaceIntegration(this);
#ifdef Q_OS_WIN
    // A Windows console adds rows at its bottom when it grows, it does not
    // take them back from the scrollback.
    surface()->setRefillFromScrollback(false);
#endif
    applyLook();
}

BuiltinTerminal::~BuiltinTerminal()
{
    if (m_pty) {
        m_pty->notifier()->disconnect(this);
        m_pty->kill(); // ends the shell (SIGHUP, as when a terminal window closes)
    }
}

bool BuiltinTerminal::available()
{
#ifdef Q_OS_WIN
    return PtyQt::isUsingConPTY();
#else
    return true;
#endif
}

void BuiltinTerminal::applyLook()
{
    setFont(editorFont());
    setColors(terminalColors());
}

void BuiltinTerminal::setReservedKeys(const std::function<bool(const QKeyEvent *)> &reserved)
{
    m_reserved = reserved;
}

bool BuiltinTerminal::start(const QString &shell, const QString &dir)
{
    m_shell = shell;
    m_pty.reset(PtyQt::createPtyProcess(IPtyProcess::AutoPty));
    if (!m_pty) {
        m_error = tr("The built-in terminal needs Windows 10 version 1903 or later.");
        return false;
    }
    QSize size = surface()->liveSize();
    if (size.width() < 20 || size.height() < 5) {
        // Not laid out yet: a terminal's usual size until it is (the shell
        // learns the real one then, as after any resize).
        size = QSize(80, 24);
        surface()->resize(size);
    }
    const QStringList environment = Lieve::Terminal::shellEnvironment(QProcessEnvironment::systemEnvironment(), kWindows).toStringList();
    // The notifier is the pty's: "aboutToClose" comes while it is still
    // handling the shell's end, so the tab goes away a moment later.
    connect(m_pty->notifier(), &QIODevice::readyRead, this, &BuiltinTerminal::readPty);
    connect(
        m_pty->notifier(),
        &QIODevice::aboutToClose,
        this,
        [this] {
            if (!m_finished) {
                m_finished = true;
                readPty();
                Q_EMIT finished(m_pty->exitCode());
            }
        },
        Qt::QueuedConnection);
    const bool started = m_pty->startProcess(shell,
                                             Lieve::Terminal::shellArguments(shell, kWindows),
                                             QDir::toNativeSeparators(dir),
                                             environment,
                                             qint16(std::clamp(size.width(), 1, 32767)),
                                             qint16(std::clamp(size.height(), 1, 32767)));
    if (!started) {
        m_error = m_pty->lastError().isEmpty() ? tr("Could not start %1.").arg(shell) : m_pty->lastError();
        m_pty->notifier()->disconnect(this);
        m_pty->kill();
        m_pty.reset();
        return false;
    }
    m_sinceStart.start();
    return true;
}

QString BuiltinTerminal::errorString() const
{
    return m_error;
}

QString BuiltinTerminal::shell() const
{
    return m_shell;
}

qint64 BuiltinTerminal::shellPid() const
{
    return m_pty && !m_finished ? m_pty->pid() : 0;
}

void BuiltinTerminal::readPty()
{
    if (!m_pty) {
        return;
    }
    const QByteArray data = m_pty->readAll();
    if (!data.isEmpty()) {
        m_sinceOutput.start();
        writeToTerminal(data, false);
    }
    // A password prompt (echo off, a whole line read at once): a lock instead of the cursor.
    setPasswordMode(m_pty->inputFlags().testFlag(IPtyProcess::InputModeHidden));
}

qint64 BuiltinTerminal::writeToPty(const QByteArray &data)
{
    if (!m_pty || m_finished) {
        return 0;
    }
    m_pending += data;
    flushPending();
    return data.size();
}

void BuiltinTerminal::flushPending()
{
    if (!m_pty || m_finished || m_pending.isEmpty()) {
        return;
    }
    const qint64 written = m_pty->write(m_pending);
    if (written > 0) {
        m_pending.remove(0, written);
    }
    // The pty takes what fits; the rest goes when the shell has read some.
    if (!m_pending.isEmpty()) {
        QTimer::singleShot(10, this, &BuiltinTerminal::flushPending);
    }
}

bool BuiltinTerminal::resizePty(QSize newSize)
{
    if (m_pty && !m_finished) {
        m_pty->resize(qint16(std::clamp(newSize.width(), 1, 32767)), qint16(std::clamp(newSize.height(), 1, 32767)));
    }
    return true; // not started yet: it starts at the new size
}

void BuiltinTerminal::sendText(const QString &text)
{
    QString typed = text;
    typed.replace(QLatin1Char('\n'), QLatin1Char('\r')); // what the Enter key sends
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    writeToPty(typed.toUtf8());
}

bool BuiltinTerminal::shellReady() const
{
#ifdef Q_OS_WIN
    // The console reports nothing about the shell's input: it is ready once
    // its prompt is out (the console host writes first, at once), and
    // nothing more has come for a moment.
    return m_sinceStart.isValid() && m_sinceStart.elapsed() > 800 && m_sinceOutput.isValid() && m_sinceOutput.elapsed() > 300;
#else
    return Lieve::Platform::shellReady(shellPid());
#endif
}

bool BuiltinTerminal::atPrompt() const
{
    return Lieve::Platform::shellAtPrompt(shellPid());
}

QStringList BuiltinTerminal::screenLines() const
{
    QStringList lines;
    const TerminalSolution::TerminalSurface *s = surface();
    const QSize live = s->liveSize();
    const int first = s->fullSize().height() - live.height();
    for (int y = first; y < s->fullSize().height(); ++y) {
        std::u32string line;
        for (int x = 0; x < live.width(); ++x) {
            const char32_t ch = s->fetchCharAt(x, y);
            if (ch != 0 || s->cellWidthAt(x, y) != 0) { // the second half of a wide character holds 0
                line += ch == 0 ? U' ' : ch;
            }
        }
        lines << QString::fromUcs4(line.data(), qsizetype(line.size())).trimmed();
    }
    while (!lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

void BuiltinTerminal::setClipboard(const QString &text)
{
    QApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

void BuiltinTerminal::selectionChanged(const std::optional<Selection> &newSelection)
{
    // Like other X11 terminals: what is selected can be pasted with the middle button.
    if (newSelection && newSelection->final && QApplication::clipboard()->supportsSelection()) {
        const QString text = textFromSelection();
        if (!text.isEmpty()) {
            QApplication::clipboard()->setText(text, QClipboard::Selection);
        }
    }
}

void BuiltinTerminal::clearScreen()
{
    clearContents();
    // The shell draws its prompt again (Ctrl+L); cmd has no such key.
    if (atPrompt() && Lieve::Terminal::shellName(m_shell, kWindows).compare(QLatin1String("cmd"), Qt::CaseInsensitive) != 0) {
        writeToPty(QByteArray("\x0c"));
    }
}

void BuiltinTerminal::contextMenuRequested(const QPoint &pos)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy"), this, [this] {
        copyToClipboard();
    });
    copy->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copy->setEnabled(selection().has_value() && selection()->start != selection()->end);
    QAction *paste = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste"), this, [this] {
        pasteFromClipboard();
    });
    paste->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    const QMimeData *clipboard = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
    paste->setEnabled(clipboard && clipboard->hasText());
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-select-all")), tr("Select All"), this, [this] {
        selectAll();
    });
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), tr("Clear"), this, [this] {
        clearScreen();
    });
    menu.exec(viewport()->mapToGlobal(pos));
}

void BuiltinTerminal::mousePressEvent(QMouseEvent *event)
{
    // The right button opens the menu, as in Konsole (the view would copy
    // or paste instead, as Windows Terminal does).
    if (event->button() == Qt::RightButton) {
        event->accept();
        contextMenuRequested(event->position().toPoint());
        return;
    }
    TerminalSolution::TerminalView::mousePressEvent(event);
}

void BuiltinTerminal::keyPressEvent(QKeyEvent *event)
{
    // Konsole's keys for the clipboard.
    if (matches(event, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C)) {
        copyToClipboard();
        return;
    }
    if (matches(event, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V) || matches(event, Qt::ShiftModifier, Qt::Key_Insert)) {
        pasteFromClipboard();
        return;
    }
#ifdef Q_OS_WIN
    // Windows Terminal's: Ctrl+C copies what is selected (else it interrupts), Ctrl+V pastes.
    if (matches(event, Qt::ControlModifier, Qt::Key_C) && selection() && selection()->start != selection()->end) {
        copyToClipboard();
        return;
    }
    if (matches(event, Qt::ControlModifier, Qt::Key_V)) {
        pasteFromClipboard();
        return;
    }
#endif
    TerminalSolution::TerminalView::keyPressEvent(event);
}

bool BuiltinTerminal::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        // Every key is the shell's (Ctrl+C, Ctrl+R, Ctrl+W...), except those
        // the window keeps (Ctrl+P, Ctrl+J, Ctrl+`...).
        auto *key = static_cast<QKeyEvent *>(event);
        if (m_reserved && m_reserved(key)) {
            event->ignore();
            return false;
        }
        event->accept();
        return true;
    }
    return TerminalSolution::TerminalView::event(event);
}

void BuiltinTerminal::onOsc(int, std::string_view, bool, bool)
{
    // Shell integration (OSC 133, 633...) is not used.
}

void BuiltinTerminal::onTitle(const QString &title)
{
    Q_EMIT titleChanged(title);
}
