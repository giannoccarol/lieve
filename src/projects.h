#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace Lieve
{
struct Project {
    QString path;
    QString name;
    QString branch;
    qint64 lastOpened = 0; // seconds since epoch, 0 = never
    bool pinned = false;
};

// Projects shown in the chooser: pinned ones, recently opened ones and the
// git repositories found in the project folders (by default ~/Documenti/Projects
// or similar; [projects] roots= in lieve.conf).
namespace Projects
{
QList<Project> all();
void touch(const QString &path); // just opened
void setPinned(const QString &path, bool pinned);
void forget(const QString &path); // remove from recents (and pins)
QStringList roots();
QString lastOpened();
}

// Git helpers (no git process: they only read files in .git).
QString gitDirFor(const QString &dir);
QString gitBranch(const QString &dir);
QString projectRootFor(const QString &dir);

QString prettyPath(const QString &path); // /home/me/x -> ~/x
QString relativeTime(qint64 secondsSinceEpoch); // "2 ore fa"
QString relativeAge(qint64 secondsSinceEpoch); // "6 mesi fa", for older dates too
}
