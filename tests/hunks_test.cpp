// The maths of staging, unstaging and reverting part of a file (linediff.h):
// texts split like a document, changes picked by a selection the way VS
// Code's "Stage Selected Ranges" does, and applied to one side.
#include "linediff.h"

#include <cstdio>

using namespace Lieve;

namespace
{
int failures = 0;

void expect(bool ok, const char *what)
{
    failures += ok ? 0 : 1;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

QList<LineHunk> diffOf(const DiffText &a, const DiffText &b)
{
    return diffLines(a.hashes(), b.hashes());
}

// `base` with the changes the selection picks from base -> work applied.
QByteArray staged(const QByteArray &base, const QByteArray &work, const LineSelection &selection)
{
    const DiffText a = DiffText::fromData(base);
    const DiffText b = DiffText::fromData(work);
    return applyHunks(a, b, selectHunks(diffOf(a, b), selection)).toData();
}

// `work` with the picked changes taken back to `base`.
QByteArray reverted(const QByteArray &base, const QByteArray &work, const LineSelection &selection)
{
    const DiffText a = DiffText::fromData(base);
    const DiffText b = DiffText::fromData(work);
    return applyHunks(b, a, invertHunks(selectHunks(diffOf(a, b), selection))).toData();
}

LineSelection lines(int first, int last, bool whole = false, DiffSide side = DiffSide::Modified)
{
    LineSelection s;
    s.side = side;
    s.first = first;
    s.last = last;
    s.wholeChanges = whole;
    return s;
}

LineSelection cursor(int line, DiffSide side = DiffSide::Modified)
{
    return lines(line, line, true, side);
}
}

int main()
{
    // Texts split like a document, byte for byte back
    {
        const DiffText t = DiffText::fromData("a\nb\n");
        expect(t.size() == 3 && t.lines.at(2).isEmpty(), "a final line break leaves an empty last line");
        expect(t.toData() == "a\nb\n", "round trip with LF");
        const DiffText crlf = DiffText::fromData("a\r\nb\r\nc");
        expect(crlf.size() == 3 && crlf.lines.at(0) == "a" && crlf.crlf.at(0) && !crlf.crlf.at(2), "CRLF lines without their \\r");
        expect(crlf.toData() == "a\r\nb\r\nc", "round trip with CRLF and no final newline");
        expect(crlf.mostlyCrlf() && !t.mostlyCrlf(), "line break style");
        const DiffText bom = DiffText::fromData("\xef\xbb\xbfx\n");
        expect(bom.prefix.size() == 3 && bom.lines.at(0) == "x" && bom.toData() == "\xef\xbb\xbfx\n", "byte order mark kept apart");
        expect(DiffText::fromData(QByteArray()).size() == 1, "an empty text is one empty line");
        expect(DiffText::fromLines({QStringLiteral("é"), QStringLiteral("b"), QString()}, "UTF-8", true).toData() == "\xc3\xa9\r\nb\r\n",
               "document lines encoded, CRLF");
        expect(DiffText::fromData("x\r\ny\n").toText("UTF-8") == QLatin1String("x\ny\n"), "decoded like a document's text()");
    }

    // Three changes: line 2 modified, a line added after 5, line 8 deleted
    const QByteArray base = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n";
    const QByteArray work = "1\ntwo\n3\n4\n5\n5a\n6\n7\n9\n10\n";
    {
        const QList<LineHunk> hunks = diffOf(DiffText::fromData(base), DiffText::fromData(work));
        expect(hunks.size() == 3, "three changes");
        expect(hunks.value(0) == LineHunk{1, 1, 1, 1} && hunks.value(1) == LineHunk{5, 0, 5, 1} && hunks.value(2) == LineHunk{7, 1, 8, 0},
               "their lines on both sides");
    }
    expect(staged(base, work, cursor(1)) == "1\ntwo\n3\n4\n5\n6\n7\n8\n9\n10\n", "cursor on a changed line: that change");
    expect(staged(base, work, cursor(5)) == "1\n2\n3\n4\n5\n5a\n6\n7\n8\n9\n10\n", "cursor on an added line");
    expect(staged(base, work, cursor(7)) == "1\n2\n3\n4\n5\n6\n7\n9\n10\n", "a deletion, from the line above it");
    expect(staged(base, work, cursor(8)) == "1\n2\n3\n4\n5\n6\n7\n9\n10\n", "a deletion, from the line below it");
    expect(staged(base, work, cursor(3)) == base, "an unchanged line picks nothing");
    expect(staged(base, work, lines(0, 9)) == work, "everything selected: everything");
    expect(staged(base, work, lines(4, 6)) == "1\n2\n3\n4\n5\n5a\n6\n7\n8\n9\n10\n", "a selection around an addition");
    expect(staged(base, work, cursor(7, DiffSide::Original)) == "1\n2\n3\n4\n5\n6\n7\n9\n10\n", "the deleted line selected on the original side");
    expect(reverted(base, work, cursor(1)) == "1\n2\n3\n4\n5\n5a\n6\n7\n9\n10\n", "revert one change");
    expect(reverted(base, work, cursor(8)) == "1\ntwo\n3\n4\n5\n5a\n6\n7\n8\n9\n10\n", "revert a deletion");
    {
        LineSelection exact;
        exact.hunk = LineHunk{5, 0, 5, 1};
        expect(staged(base, work, exact) == "1\n2\n3\n4\n5\n5a\n6\n7\n8\n9\n10\n", "exactly one change (its button)");
        exact.hunk = LineHunk{5, 0, 5, 2};
        expect(staged(base, work, exact) == base, "a change that is not there any more: nothing");
    }

    // Part of a change, like VS Code
    expect(staged("a\nb\nc\nd\n", "a\nB\nC\nd\n", lines(2, 2)) == "a\nb\nC\nd\n", "same size on both sides: line by line");
    expect(staged("x\nk\n", "a\nb\nc\nk\n", lines(1, 1)) == "b\nk\n", "different sizes: the selected new lines replace the old ones");
    expect(staged("k\n", "k\na\nb\nc\n", lines(2, 3)) == "k\nb\nc\n", "part of an addition");
    expect(staged("k\na\nb\nc\n", "k\n", lines(2, 2, false, DiffSide::Original)) == "k\na\nc\n", "part of a deletion, on the original side");
    expect(reverted("a\nb\nc\nd\n", "a\nB\nC\nd\n", lines(1, 1)) == "a\nb\nC\nd\n", "revert part of a change");

    // Final newlines, new files
    // (the editor shows the added final newline as a new empty last line)
    expect(staged("a\nb", "a\nb\n", cursor(2)) == "a\nb\n", "a final newline added");
    expect(staged("a\nb", "a\nb\n", cursor(0)) == "a\nb", "...picked on that line only");
    expect(staged("a\nb\n", "a\nb", cursor(1)) == "a\nb", "a final newline removed");
    expect(staged("a\nb", "a\nb\nc\n", cursor(2)) == "a\nb\nc\n", "lines added after a last line without newline");
    expect(staged(QByteArray(), "x\ny\n", lines(0, 0)) == "x\n", "a new file, its first line");
    expect(staged(QByteArray(), "x\ny\n", cursor(0)) == "x\ny\n", "a new file, all of it");
    expect(reverted(QByteArray(), "x\ny\n", lines(1, 1)) == "x\n", "revert a line of a new file");
    expect(reverted("a\n", "a\nb\n", cursor(1)) == "a\n", "revert the last line");

    // Line breaks: kept from the index; new lines take the style given
    {
        const DiffText index = DiffText::fromData("a\r\nb\r\nc\r\n");
        const DiffText doc = DiffText::fromLines({QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c"), QString()}, "UTF-8", index.mostlyCrlf());
        const QList<LineHunk> picked = selectHunks(diffOf(index, doc), cursor(1));
        expect(applyHunks(index, doc, picked).toData() == "a\r\nB\r\nc\r\n", "CRLF file: the staged line keeps CRLF");
        const DiffText mixed = DiffText::fromData("a\nb\r\nc\n");
        expect(applyHunks(mixed, DiffText::fromData("a\nb\r\nC\n"), {{2, 1, 2, 1}}).toData() == "a\nb\r\nC\n", "other lines untouched, byte for byte");
    }

    // Unstage: the index with some of its changes from HEAD taken back
    {
        const DiffText head = DiffText::fromData("a\nb\nc\n");
        const DiffText index = DiffText::fromData("a\nB\nc\nd\n");
        const QList<LineHunk> hunks = diffOf(head, index);
        expect(applyHunks(index, head, invertHunks(selectHunks(hunks, cursor(3)))).toData() == "a\nB\nc\n", "unstage the added line");
        expect(applyHunks(index, head, invertHunks(selectHunks(hunks, cursor(1, DiffSide::Original)))).toData() == "a\nb\nc\nd\n",
               "unstage from the HEAD side");
        expect(applyHunks(index, head, invertHunks(selectHunks(hunks, lines(0, 4)))).toData() == "a\nb\nc\n", "unstage everything");
        expect(invertHunks(invertHunks(hunks)) == hunks, "inverted twice: the same");
    }

    // Words inside a changed block
    {
        QList<TextSpan> a;
        QList<TextSpan> b;
        diffWords({QStringLiteral("call(bar, baz);")}, {QStringLiteral("call(qux, baz);")}, &a, &b);
        expect(a == QList<TextSpan>{{0, 5, 3}} && b == QList<TextSpan>{{0, 5, 3}}, "one word changed");
        a.clear();
        b.clear();
        diffWords({QStringLiteral("int x = 1;"), QStringLiteral("return x;")}, {QStringLiteral("int x = 2;"), QStringLiteral("return x + 1;")}, &a, &b);
        expect(a == QList<TextSpan>{{0, 8, 1}} && b == QList<TextSpan>{{0, 8, 1}, {1, 8, 4}}, "spans per line, on both sides");
        a.clear();
        b.clear();
        diffWords({QStringLiteral("same")}, {QStringLiteral("same  ")}, &a, &b);
        expect(a.isEmpty() && b == QList<TextSpan>{{0, 4, 2}}, "trailing spaces added");
    }

    std::printf("%s\n", failures ? "hunks: FAILED" : "hunks: all passed");
    return failures ? 1 : 0;
}
