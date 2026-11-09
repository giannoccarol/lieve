#pragma once

#include <QColor>
#include <QFont>
#include <QStringList>

#include <optional>

class QProcess;

// What differs between Linux and Windows, in one place.
namespace Lieve::Platform
{
// The process and what it starts form a group that endGroup() ends at once
// (a language server and its tsserver): a process group on Linux, a job
// object on Windows (which also ends them if Lieve itself goes away).
void ownGroup(QProcess *process);
// Ends the group of the process `pid` started with ownGroup(): SIGTERM, or
// SIGKILL with `force`, on Linux; on Windows the job is always terminated.
void endGroup(qint64 pid, bool force);

// Where Node.js, its version managers and "npm install -g" put programs on
// Windows (empty elsewhere: the Linux places are listed where they are used).
QStringList nodeBinDirs();

// The fixed-width font for code when none is set: the system's on Linux;
// Cascadia Mono or Consolas on Windows (its "system" one is Courier New).
QFont fixedFont();

// True in a KDE Plasma session, whose palette and icon theme Lieve follows as
// they are.
bool isKdeSession();

// A terminal outside Lieve, in `dir`, for when the Konsole part is not there:
// Windows Terminal (or a console window) on Windows, the desktop's terminal
// emulator on Linux. With a `command`, the shell runs it and stays open.
void openTerminal(const QString &dir, const QString &shell, const QString &title, const QString &command);

// The shell `pid` of a terminal in the panel. shellReady(): it reads its
// command line (Linux: it has turned the terminal's echo off; always true on
// Windows, where it cannot be told). shellAtPrompt(): it runs no command
// (Linux: the terminal's foreground process group is its own; Windows: it
// has no child processes); false when it cannot be told.
bool shellReady(qint64 pid);
bool shellAtPrompt(qint64 pid);

// The desktop's light/dark preference and accent colour, where Qt's palette
// does not tell (GNOME and other desktops: the freedesktop portal; Windows:
// its colour mode). Unknown values are left empty.
struct Appearance {
    std::optional<bool> dark;
    QColor accent;
};
Appearance desktopAppearance();

// Windows has no icon theme: Lieve brings Breeze (light or dark). Outside
// Plasma, Breeze fills in the icons the desktop's theme lacks.
void setupIcons(bool dark);
}
