#include "linediff.h"

#include <QHash>

#include <algorithm>
#include <utility>
#include <vector>

namespace Lieve
{
namespace
{
// Myers gives up on a stretch past this many added + removed lines: the work
// grows with its square (a whole file rewritten, for instance).
constexpr int kMaxEdits = 1000;
// Small stretches go straight to Myers; deeper patience levels too.
constexpr int kPatienceMinLines = 8;
constexpr int kMaxDepth = 32;

class Differ
{
public:
    Differ(const size_t *a, const size_t *b)
        : m_a(a)
        , m_b(b)
    {
    }

    QList<LineHunk> hunks;

    void diff(int a0, int a1, int b0, int b1, int depth)
    {
        while (a0 < a1 && b0 < b1 && m_a[a0] == m_b[b0]) {
            ++a0;
            ++b0;
        }
        while (a0 < a1 && b0 < b1 && m_a[a1 - 1] == m_b[b1 - 1]) {
            --a1;
            --b1;
        }
        if (a0 == a1 || b0 == b1) {
            add(a0, a1, b0, b1);
            return;
        }
        if ((a1 - a0) + (b1 - b0) > kPatienceMinLines && depth < kMaxDepth && patience(a0, a1, b0, b1, depth)) {
            return;
        }
        myers(a0, a1, b0, b1);
    }

private:
    // a[a0, a1) became b[b0, b1); joins the previous hunk when they touch.
    void add(int a0, int a1, int b0, int b1)
    {
        if (a0 == a1 && b0 == b1) {
            return;
        }
        if (!hunks.isEmpty()) {
            LineHunk &last = hunks.last();
            if (last.aStart + last.aCount == a0 && last.bStart + last.bCount == b0) {
                last.aCount += a1 - a0;
                last.bCount += b1 - b0;
                return;
            }
        }
        hunks.append({a0, a1 - a0, b0, b1 - b0});
    }

    // Lines that appear exactly once on each side, the longest run of them in
    // the same order on both, split the stretch into smaller ones.
    bool patience(int a0, int a1, int b0, int b1, int depth)
    {
        std::vector<std::pair<int, int>> anchors; // (line in a, line in b)
        {
            struct Seen {
                int inA = 0;
                int inB = 0;
                int posB = -1;
            };
            QHash<size_t, Seen> seen;
            seen.reserve(a1 - a0);
            for (int i = a0; i < a1; ++i) {
                ++seen[m_a[i]].inA;
            }
            for (int j = b0; j < b1; ++j) {
                const auto it = seen.find(m_b[j]);
                if (it != seen.end()) {
                    ++it->inB;
                    it->posB = j;
                }
            }
            for (int i = a0; i < a1; ++i) {
                const Seen &s = seen.value(m_a[i]);
                if (s.inA == 1 && s.inB == 1) {
                    anchors.emplace_back(i, s.posB);
                }
            }
        }
        if (anchors.empty()) {
            return false;
        }
        // Longest increasing run of the b positions (patience sorting).
        std::vector<int> tails; // per run length: index of the anchor ending the best run
        std::vector<int> previous(anchors.size(), -1);
        for (int k = 0; k < int(anchors.size()); ++k) {
            const auto it = std::lower_bound(tails.begin(), tails.end(), anchors[k].second, [&anchors](int index, int value) {
                return anchors[index].second < value;
            });
            if (it != tails.begin()) {
                previous[k] = *(it - 1);
            }
            if (it == tails.end()) {
                tails.push_back(k);
            } else {
                *it = k;
            }
        }
        std::vector<int> chain;
        for (int k = tails.back(); k >= 0; k = previous[k]) {
            chain.push_back(k);
        }
        int i = a0;
        int j = b0;
        for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
            const auto [ai, bj] = anchors[*it];
            diff(i, ai, j, bj, depth + 1);
            i = ai + 1;
            j = bj + 1;
        }
        diff(i, a1, j, b1, depth + 1);
        return true;
    }

    // Greedy Myers: the shortest edit script, found by rounds of growing
    // length; each round is kept to walk the path back at the end.
    void myers(int a0, int a1, int b0, int b1)
    {
        const int n = a1 - a0;
        const int m = b1 - b0;
        const int maxD = std::min(n + m, kMaxEdits);
        const int offset = maxD + 1;
        std::vector<int> v(size_t(2 * maxD + 3), 0); // furthest x on each diagonal k = x - y
        std::vector<int> trace; // after round d: x on diagonals -d, -d + 2, ..., d
        std::vector<size_t> roundStart;
        int rounds = -1;
        for (int d = 0; d <= maxD && rounds < 0; ++d) {
            roundStart.push_back(trace.size());
            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) ? v[offset + k + 1] : v[offset + k - 1] + 1;
                int y = x - k;
                while (x < n && y < m && m_a[a0 + x] == m_b[b0 + y]) {
                    ++x;
                    ++y;
                }
                v[offset + k] = x;
                trace.push_back(x);
                if (x >= n && y >= m) {
                    rounds = d;
                }
            }
        }
        if (rounds < 0) {
            add(a0, a1, b0, b1); // too different to be worth aligning
            return;
        }
        struct Edit {
            int x;
            int y;
            bool insert; // b[y] added before a[x]; else a[x] removed before b[y]
        };
        std::vector<Edit> edits;
        edits.reserve(size_t(rounds));
        int x = n;
        int y = m;
        for (int d = rounds; d > 0; --d) {
            const int *before = trace.data() + roundStart[size_t(d - 1)];
            const auto at = [before, d](int k) {
                return before[(k + d - 1) / 2];
            };
            const int k = x - y;
            const bool insert = k == -d || (k != d && at(k - 1) < at(k + 1));
            const int previousK = insert ? k + 1 : k - 1;
            x = at(previousK);
            y = x - previousK;
            edits.push_back({x, y, insert});
        }
        for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
            if (it->insert) {
                add(a0 + it->x, a0 + it->x, b0 + it->y, b0 + it->y + 1);
            } else {
                add(a0 + it->x, a0 + it->x + 1, b0 + it->y, b0 + it->y);
            }
        }
    }

    const size_t *m_a;
    const size_t *m_b;
};
}

QList<LineHunk> diffLines(const QList<size_t> &a, const QList<size_t> &b)
{
    Differ differ(a.constData(), b.constData());
    differ.diff(0, int(a.size()), 0, int(b.size()), 0);
    return std::move(differ.hunks);
}
}
