#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace Lieve
{
// A block of git conflict markers in a text, by line (0-based):
//   <<<<<<< HEAD           start: the current change (ours) follows
//   ||||||| base           base: the common ancestor (merge.conflictStyle
//                          diff3 or zdiff3 only, otherwise -1)
//   =======                separator: the incoming change (theirs) follows
//   >>>>>>> feature        end
struct Conflict {
    int start = -1;
    int base = -1;
    int separator = -1;
    int end = -1;
    QString currentLabel; // what follows the markers: "HEAD", "feature", a commit...
    QString baseLabel;
    QString incomingLabel;
    bool operator==(const Conflict &) const = default;
};

enum class ConflictSide {
    Current,
    Incoming,
    Both, // current, then incoming
};

// True if `line` is a conflict marker made of `marker` (seven of them, then
// the end of the line or a space and a label).
bool isConflictMarker(const QString &line, QChar marker);
// True if `line` is any of the four markers.
bool isAnyConflictMarker(const QString &line);

// The complete conflict blocks, in order. A start marker inside a block that
// is not finished yet starts the block again; incomplete blocks are skipped.
QList<Conflict> findConflicts(int lineCount, const std::function<QString(int)> &lineAt);
QList<Conflict> findConflicts(const QStringList &lines);

// The lines that take the place of the whole block (start to end) when
// `side` is accepted.
QStringList acceptedLines(const Conflict &conflict, ConflictSide side, const std::function<QString(int)> &lineAt);
QStringList acceptedLines(const QStringList &lines, const Conflict &conflict, ConflictSide side);

// `lines` with every conflict resolved by accepting `side`.
QStringList resolveConflicts(const QStringList &lines, ConflictSide side);
}
