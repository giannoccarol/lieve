#pragma once

#include <QList>
#include <QString>

namespace Lieve
{
// Fuzzy score of `query` (lowercase, no spaces) against `path` (with
// `lowerPath` its lowercase copy). Matches inside the last path component
// (the file or folder name) score higher. Returns INT_MIN when not matching.
int fuzzyScore(const QString &path, const QString &lowerPath, const QString &query);

// Where the letters of `query` are found in `text`, to highlight them: a
// whole run when possible, otherwise word starts first. Empty when not
// matching. `wordsOnly` tells whether every letter continues a run or starts
// a word ("tt" in "Toggle Terminal"), the kind of match VS Code accepts.
QList<int> fuzzyPositions(const QString &text, const QString &lowerText, const QString &query, bool *wordsOnly = nullptr);
}
