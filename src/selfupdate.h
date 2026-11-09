#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

// The parts of updating Lieve on Windows that need no network or window
// (Updater): which file of a release to download, where it goes, and the
// PowerShell script that swaps the folders once Lieve has exited. Plain
// text work, built on every platform so that the tests run everywhere.
namespace Lieve::SelfUpdate
{
// The Windows zip attached to a GitHub release ("assets" of the releases
// API): Lieve-<tag>-windows-x64.zip. An empty url when it is not there (yet).
struct Asset {
    QUrl url;
    qint64 size = -1;
};
Asset windowsAsset(const QJsonObject &release);

// The folders next to the program's own, on the same drive so that moving
// one is renaming it. Empty when the program sits at the root of a drive.
struct Folders {
    QString current; // D:/Apps/Lieve, where lieve.exe runs
    QString download; // D:/Apps/Lieve.update-0.1.5: the zip is extracted here
    QString old; // D:/Apps/Lieve.old: the replaced version, until the new one is in place
};
Folders folders(const QString &appDir, const QString &version);

// True when `dir` holds lieve.exe and only what packaging/windows/package.sh
// puts next to it, so it can be replaced as a whole: false for a build tree
// or a folder shared with other files (they would go with it).
bool isPackageFolder(const QString &dir);

// The folder with lieve.exe in an extracted zip: `dir` itself or its only
// subfolder (the zip holds a "Lieve" folder). Empty when there is none.
QString packageRoot(const QString &dir);

// A Windows command line: each argument quoted the way CommandLineToArgvW
// (and so Qt) reads it back.
QString windowsCommandLine(const QStringList &arguments);

// A single-quoted PowerShell string. PowerShell also takes the typographic
// single quotes as quotes, so those are doubled too.
QString powershellString(const QString &text);

// Windows PowerShell (powershell.exe, part of Windows 10 and 11).
QString powershell();

// The PowerShell command that extracts the zip $env:LIEVE_ZIP into the
// folder $env:LIEVE_DEST (.NET's ZipFile: fast, and it reads the zips made
// by Compress-Archive, which in Windows PowerShell separate folders with \).
QString extractCommand();

// What the restart script does: wait for Lieve (`pid`) to exit; if `fresh`
// is set, move the current folder away and `fresh` in its place (putting the
// old one back if that fails); start `exe` (when set) with `arguments`;
// remove what is left over. It notes what goes wrong in %TEMP%\lieve-update.log.
struct Restart {
    qint64 pid = 0;
    QString exe;
    QStringList arguments;
    QString fresh; // the new version's folder, holding lieve.exe; empty: just restart
    Folders folders;
};
QString restartScript(const Restart &restart);
}
