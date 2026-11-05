#pragma once

#include <QList>

#include <cstddef>

namespace Lieve
{
// Lines a[aStart, aStart + aCount) became b[bStart, bStart + bCount).
struct LineHunk {
    int aStart = 0;
    int aCount = 0;
    int bStart = 0;
    int bCount = 0;
};

// Line diff of two texts given as one hash per line, in order, like the hunks
// of git diff -U0. Patience diff (lines found once on each side anchor the
// rest), Myers between the anchors; a stretch too costly to align becomes a
// single hunk, so the time stays bounded on any input.
QList<LineHunk> diffLines(const QList<size_t> &a, const QList<size_t> &b);
}
