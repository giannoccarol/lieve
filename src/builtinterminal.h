#pragma once

#include "3rdparty/terminal/surfaceintegration.h"
#include "3rdparty/terminal/terminalview.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QStringList>

#include <functional>
#include <memory>

class IPtyProcess;
class QKeyEvent;

// A terminal Lieve draws itself, for where the Konsole part is missing
// (Windows; Linux without Konsole): Qt Creator's terminal view over libvterm,
// with a pty (ConPTY on Windows) running the shell. Same colours as Lieve's
// Konsole profile and the editor's font; nothing of it exists until a
// terminal is opened.
class BuiltinTerminal : public TerminalSolution::TerminalView, private TerminalSolution::SurfaceIntegration
{
    Q_OBJECT
public:
    explicit BuiltinTerminal(QWidget *parent = nullptr);
    ~BuiltinTerminal() override;

    // False where no pty can be had (Windows older than 10 1903).
    static bool available();

    // Keys the window keeps while the terminal has focus (Ctrl+P, Ctrl+J...):
    // `reserved` says whether a key is one of them.
    void setReservedKeys(const std::function<bool(const QKeyEvent *)> &reserved);
    // Runs `shell` in `dir`. False if it could not start: errorString() says why.
    bool start(const QString &shell, const QString &dir);
    QString errorString() const;
    QString shell() const;
    qint64 shellPid() const;

    // As if typed ("\n" is the Enter key).
    void sendText(const QString &text);
    // The shell reads its command line (see Platform::shellReady(); on
    // Windows: it has shown its prompt and been quiet since).
    bool shellReady() const;
    // The shell runs no command (Platform::shellAtPrompt()).
    bool atPrompt() const;
    // The lines on screen, for the checks.
    QStringList screenLines() const;

Q_SIGNALS:
    void titleChanged(const QString &title);
    // The shell has exited ("exit", Ctrl+D): the tab closes, like Konsole's.
    void finished(int exitCode);

protected:
    qint64 writeToPty(const QByteArray &data) override;
    bool resizePty(QSize newSize) override;
    void setClipboard(const QString &text) override;
    void selectionChanged(const std::optional<Selection> &newSelection) override;
    void contextMenuRequested(const QPoint &pos) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

private:
    // SurfaceIntegration: what the shell asks of the terminal.
    void onOsc(int cmd, std::string_view text, bool initial, bool final) override;
    void onTitle(const QString &title) override;

    void applyLook();
    void readPty();
    void flushPending();
    void clearScreen();

    std::unique_ptr<IPtyProcess> m_pty;
    QString m_shell;
    QString m_error;
    QByteArray m_pending; // not taken by the pty yet (a large paste)
    QElapsedTimer m_sinceStart;
    QElapsedTimer m_sinceOutput; // invalid until the shell has written something
    bool m_finished = false;
    std::function<bool(const QKeyEvent *)> m_reserved;
};
