#pragma once

#include <QFont>
#include <QStringList>

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

// Starts `program` in a console window of its own (Windows; elsewhere just
// starts it), detached from Lieve.
void startInConsole(const QString &program, const QStringList &arguments, const QString &dir);

// Windows has no icon theme: Lieve brings Breeze (light or dark). Elsewhere
// the desktop's theme is used.
void setupIcons(bool dark);
}
