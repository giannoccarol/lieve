#include "terminalengine.h"

#include <QFileInfo>

namespace Lieve::Terminal
{
Engine chooseEngine(const QString &setting, bool konsoleAvailable)
{
    if (setting.trimmed().compare(QLatin1String("builtin"), Qt::CaseInsensitive) == 0) {
        return Engine::Builtin;
    }
    return konsoleAvailable ? Engine::Konsole : Engine::Builtin;
}

QString shellName(const QString &shell, bool windows)
{
    if (!windows) {
        return QFileInfo(shell).fileName();
    }
    return QFileInfo(QString(shell).replace(QLatin1Char('\\'), QLatin1Char('/'))).completeBaseName();
}

QStringList shellArguments(const QString &shell, bool windows)
{
    if (windows && shellName(shell, windows).compare(QLatin1String("bash"), Qt::CaseInsensitive) == 0) {
        return {QStringLiteral("--login"), QStringLiteral("-i")}; // Git Bash, as its own window starts it
    }
    return {};
}

QProcessEnvironment shellEnvironment(QProcessEnvironment environment, bool windows)
{
    // Left by the terminal Lieve was started from: programs would believe
    // they run in that one (Konsole's D-Bus session, Windows Terminal's
    // features...), or in a window of its size.
    static const char *const others[] = {"KONSOLE_DBUS_SERVICE", "KONSOLE_DBUS_SESSION", "KONSOLE_DBUS_WINDOW", "KONSOLE_VERSION",
                                         "SHELL_SESSION_ID", "WINDOWID", "VTE_VERSION", "TERM_PROGRAM_VERSION",
                                         "TERM_SESSION_ID", "KITTY_WINDOW_ID", "KITTY_PID", "ALACRITTY_WINDOW_ID",
                                         "ALACRITTY_SOCKET", "WEZTERM_PANE", "WT_SESSION", "WT_PROFILE_ID",
                                         "COLUMNS", "LINES"};
    for (const char *name : others) {
        environment.remove(QString::fromLatin1(name));
    }
    environment.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("lieve"));
    if (!windows) {
        // What the terminal understands (xterm's keys and 256 colours, and
        // 24-bit colours), like Konsole's own sessions.
        environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
        environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    }
    return environment;
}

QString tabTitle(const QString &title)
{
    QString text = title.trimmed();
    const QString path = QString(text).replace(QLatin1Char('\\'), QLatin1Char('/')); // a Windows console's title
    if (path.contains(QLatin1Char('/')) && path.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        text = QFileInfo(path).completeBaseName();
    }
    return text.size() > 40 ? text.left(39) + QStringLiteral("…") : text;
}
}
