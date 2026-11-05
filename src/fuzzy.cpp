#include "fuzzy.h"

#include <QStringView>

#include <algorithm>
#include <climits>

namespace Lieve
{
int fuzzyScore(const QString &path, const QString &lower, const QString &query)
{
    int qi = 0;
    for (qsizetype i = 0; i < lower.size() && qi < query.size(); ++i) {
        if (lower.at(i) == query.at(qi)) {
            ++qi;
        }
    }
    if (qi < query.size()) {
        return INT_MIN;
    }

    const qsizetype nameStart = lower.lastIndexOf(QLatin1Char('/')) + 1;
    const auto scoreFrom = [&](qsizetype from) {
        int score = 0;
        int matched = 0;
        qsizetype previous = -2;
        for (qsizetype i = from; i < lower.size() && matched < query.size(); ++i) {
            if (lower.at(i) != query.at(matched)) {
                continue;
            }
            int s = 1;
            if (i == previous + 1) {
                s += 6; // consecutive letters
            } else if (matched > 0) {
                s -= std::min<int>(int(i - previous - 1), 6); // gap since the previous letter
            }
            if (matched == 0 && i == nameStart) {
                s += 8; // typing the beginning of the name
            }
            if (i >= nameStart) {
                s += 2; // inside the name
            }
            const bool wordStart = i == 0 || i == nameStart || QStringView(u"/._- ").contains(path.at(i - 1))
                || (path.at(i).isUpper() && path.at(i - 1).isLower());
            if (wordStart) {
                s += 5;
            }
            score += s;
            previous = i;
            ++matched;
        }
        return matched == query.size() ? score : INT_MIN;
    };

    int best = scoreFrom(0);
    const int inName = scoreFrom(nameStart);
    if (inName != INT_MIN) {
        best = std::max(best, inName + 15);
    }
    const QStringView name = QStringView(lower).mid(nameStart);
    if (name.startsWith(query)) {
        best += 25;
    } else if (name.contains(query)) {
        best += 12;
    }
    return best - int(name.size() / 4) - int(lower.size() / 8);
}

namespace
{
bool startsWord(const QString &text, qsizetype i)
{
    return i == 0 || QStringView(u"/._-: ").contains(text.at(i - 1)) || (text.at(i).isUpper() && text.at(i - 1).isLower());
}

// True when query[qi..] is a subsequence of lower[from..].
bool matchesFrom(const QString &lower, qsizetype from, const QString &query, qsizetype qi)
{
    for (qsizetype i = from; i < lower.size() && qi < query.size(); ++i) {
        if (lower.at(i) == query.at(qi)) {
            ++qi;
        }
    }
    return qi == query.size();
}
}

QList<int> fuzzyPositions(const QString &text, const QString &lower, const QString &query, bool *wordsOnly)
{
    QList<int> positions;
    if (wordsOnly) {
        *wordsOnly = false;
    }
    if (query.isEmpty() || !matchesFrom(lower, 0, query, 0)) {
        return positions;
    }
    // A contiguous run, preferably at a word start.
    qsizetype run = -1;
    for (qsizetype i = lower.indexOf(query); i >= 0; i = lower.indexOf(query, i + 1)) {
        if (run < 0 || startsWord(text, i)) {
            run = i;
        }
        if (startsWord(text, i)) {
            break;
        }
    }
    if (run >= 0) {
        for (qsizetype k = 0; k < query.size(); ++k) {
            positions << int(run + k);
        }
        if (wordsOnly) {
            *wordsOnly = true;
        }
        return positions;
    }
    // Letter by letter: continue the current run, else jump to the next word
    // that starts with the letter, else take the next occurrence.
    bool words = true;
    qsizetype from = 0;
    for (qsizetype qi = 0; qi < query.size(); ++qi) {
        const QChar c = query.at(qi);
        qsizetype pick = -1;
        if (!positions.isEmpty() && from < lower.size() && lower.at(from) == c) {
            pick = from;
        } else {
            for (qsizetype i = from; i < lower.size() && pick < 0; ++i) {
                if (lower.at(i) == c && startsWord(text, i) && matchesFrom(lower, i + 1, query, qi + 1)) {
                    pick = i;
                }
            }
            if (pick < 0) {
                pick = lower.indexOf(c, from);
                words = false;
            }
        }
        positions << int(pick);
        from = pick + 1;
    }
    if (wordsOnly) {
        *wordsOnly = words;
    }
    return positions;
}
}
