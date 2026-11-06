#include "conflicts.h"

namespace Lieve
{
namespace
{
constexpr int kMarkerSize = 7; // git's default conflict-marker-size

QString labelOf(const QString &line)
{
    return line.mid(kMarkerSize).trimmed();
}
}

bool isConflictMarker(const QString &line, QChar marker)
{
    if (line.size() < kMarkerSize) {
        return false;
    }
    for (int i = 0; i < kMarkerSize; ++i) {
        if (line.at(i) != marker) {
            return false;
        }
    }
    // Not a longer run (a row of "=" under a heading, an ASCII rule...).
    return line.size() == kMarkerSize || line.at(kMarkerSize) == QLatin1Char(' ') || line.at(kMarkerSize) == QLatin1Char('\t')
        || line.at(kMarkerSize) == QLatin1Char('\r');
}

bool isAnyConflictMarker(const QString &line)
{
    if (line.size() < kMarkerSize) {
        return false;
    }
    const QChar first = line.at(0);
    return (first == QLatin1Char('<') || first == QLatin1Char('|') || first == QLatin1Char('=') || first == QLatin1Char('>'))
        && isConflictMarker(line, first);
}

QList<Conflict> findConflicts(int lineCount, const std::function<QString(int)> &lineAt)
{
    enum State {
        Outside,
        InCurrent,
        InBase,
        InIncoming,
    };
    QList<Conflict> conflicts;
    Conflict conflict;
    State state = Outside;
    for (int i = 0; i < lineCount; ++i) {
        const QString line = lineAt(i);
        if (!isAnyConflictMarker(line)) {
            continue; // the cheap test: most lines stop at their first character
        }
        const QChar marker = line.at(0);
        if (marker == QLatin1Char('<')) {
            conflict = Conflict();
            conflict.start = i;
            conflict.currentLabel = labelOf(line);
            state = InCurrent;
            continue;
        }
        switch (state) {
        case Outside:
            break;
        case InCurrent:
            if (marker == QLatin1Char('|')) {
                conflict.base = i;
                conflict.baseLabel = labelOf(line);
                state = InBase;
            } else if (marker == QLatin1Char('=')) {
                conflict.separator = i;
                state = InIncoming;
            }
            break;
        case InBase:
            if (marker == QLatin1Char('=')) {
                conflict.separator = i;
                state = InIncoming;
            }
            break;
        case InIncoming:
            if (marker == QLatin1Char('>')) {
                conflict.end = i;
                conflict.incomingLabel = labelOf(line);
                conflicts.append(conflict);
                state = Outside;
            }
            break;
        }
    }
    return conflicts;
}

QList<Conflict> findConflicts(const QStringList &lines)
{
    return findConflicts(int(lines.size()), [&lines](int i) {
        return lines.at(i);
    });
}

QStringList acceptedLines(const Conflict &conflict, ConflictSide side, const std::function<QString(int)> &lineAt)
{
    QStringList kept;
    if (side != ConflictSide::Incoming) {
        const int currentEnd = conflict.base >= 0 ? conflict.base : conflict.separator;
        for (int i = conflict.start + 1; i < currentEnd; ++i) {
            kept.append(lineAt(i));
        }
    }
    if (side != ConflictSide::Current) {
        for (int i = conflict.separator + 1; i < conflict.end; ++i) {
            kept.append(lineAt(i));
        }
    }
    return kept;
}

QStringList acceptedLines(const QStringList &lines, const Conflict &conflict, ConflictSide side)
{
    return acceptedLines(conflict, side, [&lines](int i) {
        return lines.at(i);
    });
}

QStringList resolveConflicts(const QStringList &lines, ConflictSide side)
{
    QStringList result = lines;
    const QList<Conflict> conflicts = findConflicts(lines);
    for (qsizetype n = conflicts.size() - 1; n >= 0; --n) { // from the last: the earlier lines stay where they are
        const Conflict &c = conflicts.at(n);
        result.remove(c.start, c.end - c.start + 1);
        const QStringList kept = acceptedLines(lines, c, side);
        for (qsizetype k = kept.size() - 1; k >= 0; --k) {
            result.insert(c.start, kept.at(k));
        }
    }
    return result;
}
}
