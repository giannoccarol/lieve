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
}
