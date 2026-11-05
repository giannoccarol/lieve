#pragma once

#include <QString>

namespace Lieve
{
// Fuzzy score of `query` (lowercase, no spaces) against `path` (with
// `lowerPath` its lowercase copy). Matches inside the last path component
// (the file or folder name) score higher. Returns INT_MIN when not matching.
int fuzzyScore(const QString &path, const QString &lowerPath, const QString &query);
}
