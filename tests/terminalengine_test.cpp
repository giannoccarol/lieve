// The terminal panel's decisions: Konsole or the built-in terminal
// ([terminal] engine=), and how the built-in one starts a shell (name,
// arguments, environment) and titles its tab.
#include "terminalengine.h"

#include <QCoreApplication>

#include <cstdio>

using namespace Lieve::Terminal;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };

    // engine=
    expect(chooseEngine(QStringLiteral("auto"), true) == Engine::Konsole, "auto with Konsole: Konsole");
    expect(chooseEngine(QStringLiteral("auto"), false) == Engine::Builtin, "auto without Konsole: built-in");
    expect(chooseEngine(QString(), true) == Engine::Konsole, "empty = auto");
    expect(chooseEngine(QStringLiteral("konsole"), true) == Engine::Konsole, "konsole with Konsole: Konsole");
    expect(chooseEngine(QStringLiteral("konsole"), false) == Engine::Builtin, "konsole without Konsole: built-in");
    expect(chooseEngine(QStringLiteral("builtin"), true) == Engine::Builtin, "builtin even with Konsole");
    expect(chooseEngine(QStringLiteral(" Builtin "), true) == Engine::Builtin, "builtin: case and spaces ignored");
    expect(chooseEngine(QStringLiteral("xterm"), true) == Engine::Konsole, "unknown value = auto");

    // Shell names
    expect(shellName(QStringLiteral("/usr/bin/fish"), false) == QLatin1String("fish"), "Linux shell name");
    expect(shellName(QStringLiteral("/usr/bin/bash.real"), false) == QLatin1String("bash.real"), "Linux: the whole file name");
    expect(shellName(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe"), true) == QLatin1String("pwsh"), "Windows: no .exe");
    expect(shellName(QStringLiteral("C:\\Windows\\System32\\cmd.exe"), true) == QLatin1String("cmd"), "Windows: backslashes");

    // Arguments
    expect(shellArguments(QStringLiteral("/usr/bin/bash"), false).isEmpty(), "bash on Linux: no arguments, like Konsole");
    expect(shellArguments(QStringLiteral("/usr/bin/fish"), false).isEmpty(), "fish: no arguments");
    expect(shellArguments(QStringLiteral("C:/Program Files/Git/bin/bash.exe"), true) == QStringList({QStringLiteral("--login"), QStringLiteral("-i")}),
           "Git Bash: a login shell");
    expect(shellArguments(QStringLiteral("C:\\Program Files\\PowerShell\\7\\pwsh.exe"), true).isEmpty(), "pwsh: no arguments");
    expect(shellArguments(QStringLiteral("C:/Windows/System32/cmd.exe"), true).isEmpty(), "cmd: no arguments");
    expect(shellArguments(QStringLiteral("C:/Windows/System32/wsl.exe"), true).isEmpty(), "wsl: no arguments");

    // Environment
    QProcessEnvironment base;
    base.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    base.insert(QStringLiteral("HOME"), QStringLiteral("/home/me"));
    base.insert(QStringLiteral("TERM"), QStringLiteral("xterm-kitty"));
    base.insert(QStringLiteral("KONSOLE_VERSION"), QStringLiteral("250800"));
    base.insert(QStringLiteral("KONSOLE_DBUS_SESSION"), QStringLiteral("/Sessions/1"));
    base.insert(QStringLiteral("WINDOWID"), QStringLiteral("42"));
    base.insert(QStringLiteral("COLUMNS"), QStringLiteral("80"));
    base.insert(QStringLiteral("WT_SESSION"), QStringLiteral("abc"));
    const QProcessEnvironment onLinux = shellEnvironment(base, false);
    expect(onLinux.value(QStringLiteral("TERM")) == QLatin1String("xterm-256color"), "Linux: TERM=xterm-256color");
    expect(onLinux.value(QStringLiteral("COLORTERM")) == QLatin1String("truecolor"), "Linux: COLORTERM=truecolor");
    expect(onLinux.value(QStringLiteral("TERM_PROGRAM")) == QLatin1String("lieve"), "TERM_PROGRAM=lieve");
    expect(onLinux.value(QStringLiteral("PATH")) == QLatin1String("/usr/bin") && onLinux.value(QStringLiteral("HOME")) == QLatin1String("/home/me"),
           "the user's variables are kept");
    expect(!onLinux.contains(QStringLiteral("KONSOLE_VERSION")) && !onLinux.contains(QStringLiteral("KONSOLE_DBUS_SESSION"))
               && !onLinux.contains(QStringLiteral("WINDOWID")) && !onLinux.contains(QStringLiteral("COLUMNS")),
           "the outer terminal's variables are gone");
    const QProcessEnvironment onWindows = shellEnvironment(base, true);
    expect(onWindows.value(QStringLiteral("TERM")) == QLatin1String("xterm-kitty") && !onWindows.contains(QStringLiteral("COLORTERM")),
           "Windows: TERM left as the user has it");
    expect(!onWindows.contains(QStringLiteral("WT_SESSION")), "Windows: not Windows Terminal's session");
    expect(onWindows.value(QStringLiteral("PATH")) == QLatin1String("/usr/bin"), "Windows: the user's variables are kept");

    // Tab titles
    expect(tabTitle(QStringLiteral("C:\\Windows\\system32\\cmd.exe")) == QLatin1String("cmd"), "a program path: its name");
    expect(tabTitle(QStringLiteral("C:\\Program Files\\PowerShell\\7\\pwsh.exe")) == QLatin1String("pwsh"), "a path with spaces: its name");
    expect(tabTitle(QStringLiteral("  fish  ~/Projects  ")) == QLatin1String("fish  ~/Projects"), "trimmed");
    expect(tabTitle(QStringLiteral("npm run build.exe")) == QLatin1String("npm run build.exe"), "no path: kept");
    const QString longTitle = tabTitle(QString(50, QLatin1Char('x')));
    expect(longTitle.size() == 40 && longTitle.endsWith(QStringLiteral("…")), "a long title: 40 characters");

    std::printf("%s\n", failures ? "terminalengine: FAILED" : "terminalengine: all passed");
    return failures ? 1 : 0;
}
