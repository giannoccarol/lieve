#include "linediff.h"

#include <QHash>
#include <QStringDecoder>
#include <QStringEncoder>

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

namespace
{
const char kBom[] = "\xef\xbb\xbf";
// Word diffs stop past this many words, spaces and signs on a side.
constexpr int kMaxTokens = 4000;

QStringEncoder encoderFor(const QByteArray &encoding)
{
    QStringEncoder encoder(encoding.isEmpty() ? "UTF-8" : encoding.constData());
    return encoder.isValid() ? std::move(encoder) : QStringEncoder(QStringEncoder::Utf8);
}

QStringDecoder decoderFor(const QByteArray &encoding)
{
    QStringDecoder decoder(encoding.isEmpty() ? "UTF-8" : encoding.constData());
    return decoder.isValid() ? std::move(decoder) : QStringDecoder(QStringDecoder::Utf8);
}

// Words (letters, digits, '_'), runs of spaces, and single other characters;
// a line break between lines is a token of its own.
struct Token {
    int line;
    int column;
    int length; // 0: the line break
    size_t hash;
};

void tokenize(const QStringList &lines, std::vector<Token> &tokens)
{
    for (int l = 0; l < lines.size(); ++l) {
        if (l > 0) {
            tokens.push_back({l - 1, int(lines.at(l - 1).size()), 0, qHash(QStringView(u"\n"))});
        }
        const QString &text = lines.at(l);
        int i = 0;
        while (i < text.size()) {
            const QChar c = text.at(i);
            int end = i + 1;
            const auto word = [](QChar ch) {
                return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch.isHighSurrogate() || ch.isLowSurrogate();
            };
            if (word(c)) {
                while (end < text.size() && word(text.at(end))) {
                    ++end;
                }
            } else if (c.isSpace()) {
                while (end < text.size() && text.at(end).isSpace()) {
                    ++end;
                }
            }
            tokens.push_back({l, i, end - i, qHash(QStringView(text).mid(i, end - i))});
            i = end;
        }
    }
}

void addSpans(const std::vector<Token> &tokens, int from, int count, QList<TextSpan> *spans)
{
    for (int k = from; k < from + count; ++k) {
        const Token &t = tokens[size_t(k)];
        if (t.length == 0) {
            continue;
        }
        if (!spans->isEmpty() && spans->last().line == t.line && spans->last().column + spans->last().length == t.column) {
            spans->last().length += t.length;
        } else {
            spans->append({t.line, t.column, t.length});
        }
    }
}
}

DiffText DiffText::fromData(const QByteArray &data)
{
    DiffText text;
    qsizetype start = 0;
    if (data.startsWith(kBom)) {
        text.prefix = data.left(3);
        start = 3;
    }
    for (;;) {
        const qsizetype end = data.indexOf('\n', start);
        if (end < 0) {
            text.lines.append(data.mid(start));
            text.crlf.append(false);
            break;
        }
        const bool cr = end > start && data.at(end - 1) == '\r';
        text.lines.append(data.mid(start, end - start - (cr ? 1 : 0)));
        text.crlf.append(cr);
        start = end + 1;
    }
    return text;
}

DiffText DiffText::fromLines(const QStringList &lines, const QByteArray &encoding, bool crlf)
{
    DiffText text;
    QStringEncoder encoder = encoderFor(encoding);
    for (const QString &line : lines) {
        text.lines.append(encoder.encode(line));
        text.crlf.append(crlf);
    }
    if (text.lines.isEmpty()) {
        text.lines.append(QByteArray());
        text.crlf.append(false);
    }
    text.crlf.last() = false;
    return text;
}

QByteArray DiffText::toData() const
{
    QByteArray data = prefix;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        data += lines.at(i);
        if (i + 1 < lines.size()) {
            data += crlf.value(i) ? QByteArrayView("\r\n") : QByteArrayView("\n");
        }
    }
    return data;
}

QString DiffText::toText(const QByteArray &encoding) const
{
    QStringDecoder decoder = decoderFor(encoding);
    QStringList decoded;
    decoded.reserve(lines.size());
    for (const QByteArray &line : lines) {
        decoded.append(decoder.decode(line));
    }
    return decoded.join(QLatin1Char('\n'));
}

QList<size_t> DiffText::hashes() const
{
    QList<size_t> result;
    result.reserve(lines.size());
    for (const QByteArray &line : lines) {
        result.append(qHash(line));
    }
    return result;
}

bool DiffText::mostlyCrlf() const
{
    qsizetype crlfs = 0;
    for (qsizetype i = 0; i + 1 < crlf.size(); ++i) {
        crlfs += crlf.at(i) ? 1 : 0;
    }
    return crlfs * 2 > crlf.size() - 1;
}

QList<LineHunk> selectHunks(const QList<LineHunk> &hunks, const LineSelection &selection)
{
    if (selection.hunk) {
        return hunks.contains(*selection.hunk) ? QList<LineHunk>{*selection.hunk} : QList<LineHunk>();
    }
    const bool modified = selection.side == DiffSide::Modified;
    QList<LineHunk> chosen;
    for (const LineHunk &h : hunks) {
        const int start = modified ? h.bStart : h.aStart;
        const int count = modified ? h.bCount : h.aCount;
        if (count == 0) { // lines only on the other side, between start - 1 and start on this one
            if (selection.first <= start && selection.last >= start - 1) {
                chosen.append(h);
            }
            continue;
        }
        const int from = std::max(selection.first, start);
        const int to = std::min(selection.last, start + count - 1);
        if (from > to) {
            continue;
        }
        if (selection.wholeChanges || (from == start && to == start + count - 1)) {
            chosen.append(h);
            continue;
        }
        int otherStart = modified ? h.aStart : h.bStart;
        int otherCount = modified ? h.aCount : h.bCount;
        if (otherCount == count) { // line by line
            otherStart += from - start;
            otherCount = to - from + 1;
        }
        chosen.append(modified ? LineHunk{otherStart, otherCount, from, to - from + 1} : LineHunk{from, to - from + 1, otherStart, otherCount});
    }
    return chosen;
}

QList<LineHunk> invertHunks(const QList<LineHunk> &hunks)
{
    QList<LineHunk> inverted;
    inverted.reserve(hunks.size());
    for (const LineHunk &h : hunks) {
        inverted.append({h.bStart, h.bCount, h.aStart, h.aCount});
    }
    return inverted;
}

DiffText applyHunks(const DiffText &base, const DiffText &other, const QList<LineHunk> &changes)
{
    DiffText result;
    result.prefix = base.prefix;
    const auto take = [&result](const DiffText &from, int line) {
        result.lines.append(from.lines.value(line));
        result.crlf.append(from.crlf.value(line));
    };
    int line = 0;
    for (const LineHunk &h : changes) {
        for (; line < h.aStart && line < base.size(); ++line) {
            take(base, line);
        }
        for (int j = h.bStart; j < h.bStart + h.bCount && j < other.size(); ++j) {
            take(other, j);
        }
        line = std::max(line, h.aStart + h.aCount);
    }
    for (; line < base.size(); ++line) {
        take(base, line);
    }
    if (result.lines.isEmpty()) { // an empty text is one empty line
        result.lines.append(QByteArray());
        result.crlf.append(false);
    }
    result.crlf.last() = false;
    return result;
}

std::optional<QByteArray>
stagedContent(const QByteArray &index, const QStringList &documentLines, const QByteArray &encoding, bool documentCrlf, const LineSelection &selection)
{
    const DiffText base = DiffText::fromData(index);
    const DiffText work = DiffText::fromLines(documentLines, encoding, base.hasLineBreaks() ? base.mostlyCrlf() : documentCrlf);
    const QList<LineHunk> chosen = selectHunks(diffLines(base.hashes(), work.hashes()), selection);
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    return applyHunks(base, work, chosen).toData();
}

std::optional<QByteArray> unstagedContent(const QByteArray &head, const QByteArray &index, const LineSelection &selection)
{
    const DiffText original = DiffText::fromData(head);
    const DiffText staged = DiffText::fromData(index);
    const QList<LineHunk> chosen = selectHunks(diffLines(original.hashes(), staged.hashes()), selection);
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    return applyHunks(staged, original, invertHunks(chosen)).toData();
}

std::optional<QString> revertedText(const QByteArray &index, const QStringList &documentLines, const QByteArray &encoding, const LineSelection &selection)
{
    const DiffText base = DiffText::fromData(index);
    const DiffText work = DiffText::fromLines(documentLines, encoding, false);
    const QList<LineHunk> chosen = selectHunks(diffLines(base.hashes(), work.hashes()), selection);
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    // The document's own lines stay as they are (not through the encoding).
    QStringDecoder decoder = decoderFor(encoding);
    QStringList lines;
    int line = 0;
    for (const LineHunk &h : chosen) {
        for (; line < h.bStart && line < documentLines.size(); ++line) {
            lines.append(documentLines.at(line));
        }
        for (int j = h.aStart; j < h.aStart + h.aCount && j < base.size(); ++j) {
            lines.append(decoder.decode(base.lines.at(j)));
        }
        line = std::max(line, h.bStart + h.bCount);
    }
    for (; line < documentLines.size(); ++line) {
        lines.append(documentLines.at(line));
    }
    return lines.join(QLatin1Char('\n'));
}

void diffWords(const QStringList &a, const QStringList &b, QList<TextSpan> *changedA, QList<TextSpan> *changedB)
{
    std::vector<Token> ta;
    std::vector<Token> tb;
    tokenize(a, ta);
    tokenize(b, tb);
    if (ta.size() > size_t(kMaxTokens) || tb.size() > size_t(kMaxTokens)) {
        return;
    }
    QList<size_t> ha;
    QList<size_t> hb;
    ha.reserve(qsizetype(ta.size()));
    hb.reserve(qsizetype(tb.size()));
    for (const Token &t : ta) {
        ha.append(t.hash);
    }
    for (const Token &t : tb) {
        hb.append(t.hash);
    }
    const QList<LineHunk> hunks = diffLines(ha, hb);
    for (const LineHunk &h : hunks) {
        addSpans(ta, h.aStart, h.aCount, changedA);
        addSpans(tb, h.bStart, h.bCount, changedB);
    }
}
}
