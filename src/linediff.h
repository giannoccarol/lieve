#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>

namespace Lieve
{
// Lines a[aStart, aStart + aCount) became b[bStart, bStart + bCount).
struct LineHunk {
    int aStart = 0;
    int aCount = 0;
    int bStart = 0;
    int bCount = 0;
    bool operator==(const LineHunk &) const = default;
};

// Line diff of two texts given as one hash per line, in order, like the hunks
// of git diff -U0. Patience diff (lines found once on each side anchor the
// rest), Myers between the anchors; a stretch too costly to align becomes a
// single hunk, so the time stays bounded on any input.
QList<LineHunk> diffLines(const QList<size_t> &a, const QList<size_t> &b);

// One side of a diff, split like a KTextEditor document: the pieces between
// the '\n's, so that the last one is "" when the text ends with a line break
// (a missing final newline is then a change like any other, and line numbers
// match the editor's). Lines read from git stay byte for byte as they were.
struct DiffText {
    QByteArray prefix; // a UTF-8 byte order mark, kept as it was
    QList<QByteArray> lines; // without their "\n" or "\r\n"
    QList<bool> crlf; // the line ended with "\r\n" (never set on the last one)

    static DiffText fromData(const QByteArray &data);
    // A document's lines, encoded; each line break is "\r\n" when `crlf`.
    static DiffText fromLines(const QStringList &lines, const QByteArray &encoding, bool crlf);
    QByteArray toData() const;
    // Decoded, the lines joined with '\n' (a document's text()).
    QString toText(const QByteArray &encoding) const;
    QList<size_t> hashes() const;
    int size() const
    {
        return int(lines.size());
    }
    // Most of its line breaks are "\r\n" (false when it has none).
    bool mostlyCrlf() const;
    bool hasLineBreaks() const
    {
        return lines.size() > 1;
    }
};

enum class DiffSide {
    Original, // a: the index for unstaged changes, HEAD for staged ones
    Modified, // b: the working file, or the index
};

// Lines first..last of one side, and the changes they pick: with
// wholeChanges every change they touch, entirely (a change with no lines on
// that side is touched by the lines just above and below it); otherwise only
// the selected lines of each change, like VS Code's "Stage Selected Ranges":
// a change with as many lines on both sides is taken line by line, any other
// gives all its lines on the other side for the selected ones. `hunk` picks
// exactly that change instead (a button next to it).
struct LineSelection {
    DiffSide side = DiffSide::Modified;
    int first = 0;
    int last = 0;
    bool wholeChanges = true;
    std::optional<LineHunk> hunk;
};
QList<LineHunk> selectHunks(const QList<LineHunk> &hunks, const LineSelection &selection);
// The same changes seen from b: a and b swapped.
QList<LineHunk> invertHunks(const QList<LineHunk> &hunks);
// `base` with lines [aStart, aStart + aCount) of each change replaced by
// lines [bStart, bStart + bCount) of `other` (sorted, not overlapping).
DiffText applyHunks(const DiffText &base, const DiffText &other, const QList<LineHunk> &changes);

// Part of a file's changes, as VS Code stages, unstages and reverts them;
// nullopt when the selection picks no change. Texts from git are as stored
// (`index` empty for a file not in the index); a document's lines are encoded
// with its encoding.
//
// The index with the selected changes of the document staged. Lines that
// come from the document end like the index's (CRLF if most of its lines do)
// or, in a new file, like the document's.
std::optional<QByteArray>
stagedContent(const QByteArray &index, const QStringList &documentLines, const QByteArray &encoding, bool documentCrlf, const LineSelection &selection);
// The index with the selected staged changes (HEAD -> index, the selection on
// either side) taken back.
std::optional<QByteArray> unstagedContent(const QByteArray &head, const QByteArray &index, const LineSelection &selection);
// The document's text with the selected changes taken back to the index.
std::optional<QString> revertedText(const QByteArray &index, const QStringList &documentLines, const QByteArray &encoding, const LineSelection &selection);

// Inside a changed block (lines a became lines b): what differs, word by word
// like VS Code's inner highlights, as (line, column, length) on each side.
// Nothing for blocks too long to be worth it.
struct TextSpan {
    int line = 0;
    int column = 0;
    int length = 0;
    bool operator==(const TextSpan &) const = default;
};
void diffWords(const QStringList &a, const QStringList &b, QList<TextSpan> *changedA, QList<TextSpan> *changedB);
}
