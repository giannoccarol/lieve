#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Hot exit, like VS Code's files.hotExit: the text of unsaved documents is
// kept in ~/.local/state/lieve/backups/<project>/ (one folder per project),
// so closing a window does not have to ask and a crash loses nothing.
// Each backup names its own file, so the folder alone is enough to restore.
namespace Lieve::Backups
{
struct Backup {
    QString name; // file name in the project's backup folder
    QString path; // the file the text belongs to; empty for a new document
    qint64 diskTime = 0; // the file's modification time the changes started from (ms)
    QString text;
};

// [files] hotExit: closing a window keeps unsaved documents instead of asking.
bool hotExit();
// A new, unique backup name for a document of `path` (empty: a new document).
QString newName(const QString &path);
QStringList names(const QString &project);
// Atomic (QSaveFile).
bool write(const QString &project, const Backup &backup);
std::optional<Backup> read(const QString &project, const QString &name);
void remove(const QString &project, const QString &name);
}
