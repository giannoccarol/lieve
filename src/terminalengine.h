#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// What the terminal panel decides without a terminal: which engine draws the
// terminals, and how the built-in one starts a shell (tests/terminalengine_test.cpp).
namespace Lieve::Terminal
{
enum class Engine {
    Konsole, // the Konsole part (kf6/parts/konsolepart)
    Builtin, // Lieve's own (BuiltinTerminal)
};

// [terminal] engine=: "builtin" always uses the built-in terminal; "auto"
// (or empty, "konsole", anything else) Konsole's when its part is
// installed, else the built-in one.
Engine chooseEngine(const QString &setting, bool konsoleAvailable);

// The name a shell goes by in tabs and menus: "fish", "pwsh" (not "pwsh.exe").
QString shellName(const QString &shell, bool windows);

// The arguments that start `shell` as an interactive shell in the built-in
// terminal, the way its own window would: Git Bash (bash.exe on Windows) as a
// login shell; none for the others (Konsole runs them so too).
QStringList shellArguments(const QString &shell, bool windows);

// The environment of a shell in the built-in terminal: Lieve's own, without
// what describes the terminal Lieve may have been started from (Konsole,
// Windows Terminal...); on Linux TERM=xterm-256color and COLORTERM=truecolor.
QProcessEnvironment shellEnvironment(QProcessEnvironment environment, bool windows);

// A terminal's title (OSC 0/2) as a tab text: a program path is shortened to
// the program's name ("C:\Windows\system32\cmd.exe" -> "cmd"), a long title
// to 40 characters.
QString tabTitle(const QString &title);
}
