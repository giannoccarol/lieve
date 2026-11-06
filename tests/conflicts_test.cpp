// The conflict marker parser: git's blocks (with and without the diff3 base),
// what each side keeps, and lines that only look like markers.
#include "conflicts.h"

#include <cstdio>

using namespace Lieve;

namespace
{
QStringList lines(const char *text)
{
    return QString::fromUtf8(text).split(QLatin1Char('\n'));
}
}

int main()
{
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };

    // Two conflicts, the second with a diff3 base section.
    const QStringList text = lines(
        "start\n" // 0
        "<<<<<<< HEAD\n" // 1
        "ours 1\n" // 2
        "ours 2\n" // 3
        "=======\n" // 4
        "theirs\n" // 5
        ">>>>>>> feature/x\n" // 6
        "middle\n" // 7
        "<<<<<<< HEAD\n" // 8
        "ours b\n" // 9
        "||||||| merged common ancestors\n" // 10
        "base b\n" // 11
        "=======\n" // 12
        "theirs b\n" // 13
        ">>>>>>> feature/x\n" // 14
        "end");
    const QList<Conflict> found = findConflicts(text);
    expect(found.size() == 2, "two conflicts");
    if (found.size() == 2) {
        const Conflict &a = found.at(0);
        expect(a.start == 1 && a.base == -1 && a.separator == 4 && a.end == 6, "first block lines");
        expect(a.currentLabel == QLatin1String("HEAD") && a.incomingLabel == QLatin1String("feature/x"), "labels");
        const Conflict &b = found.at(1);
        expect(b.start == 8 && b.base == 10 && b.separator == 12 && b.end == 14, "diff3 block lines");
        expect(b.baseLabel == QLatin1String("merged common ancestors"), "base label");

        expect(acceptedLines(text, a, ConflictSide::Current) == QStringList({QStringLiteral("ours 1"), QStringLiteral("ours 2")}), "accept current");
        expect(acceptedLines(text, a, ConflictSide::Incoming) == QStringList({QStringLiteral("theirs")}), "accept incoming");
        expect(acceptedLines(text, a, ConflictSide::Both) == QStringList({QStringLiteral("ours 1"), QStringLiteral("ours 2"), QStringLiteral("theirs")}),
               "accept both");
        expect(acceptedLines(text, b, ConflictSide::Current) == QStringList({QStringLiteral("ours b")}), "diff3: current leaves the base out");
        expect(acceptedLines(text, b, ConflictSide::Both) == QStringList({QStringLiteral("ours b"), QStringLiteral("theirs b")}),
               "diff3: both leaves the base out");
    }
    expect(resolveConflicts(text, ConflictSide::Incoming) == lines("start\ntheirs\nmiddle\ntheirs b\nend"), "resolve all incoming");
    expect(resolveConflicts(text, ConflictSide::Both) == lines("start\nours 1\nours 2\ntheirs\nmiddle\nours b\ntheirs b\nend"), "resolve all both");

    // Empty sides (one branch deleted the lines) and a conflict at the very end.
    const QStringList empty = lines("a\n<<<<<<< HEAD\n=======\ngone\n>>>>>>> other");
    const QList<Conflict> emptyFound = findConflicts(empty);
    expect(emptyFound.size() == 1 && emptyFound.at(0).end == 4, "empty current side");
    expect(emptyFound.size() == 1 && acceptedLines(empty, emptyFound.at(0), ConflictSide::Current).isEmpty(), "accept empty side keeps nothing");
    expect(resolveConflicts(empty, ConflictSide::Current) == lines("a"), "resolved to nothing");

    // Lines that only look like markers, and incomplete blocks.
    expect(findConflicts(lines("<<<<<<<< HEAD\nx\n=======\ny\n>>>>>>> b")).isEmpty(), "eight '<' is no marker");
    expect(findConflicts(lines("<<<<<<< a\nx\n========\ny\n>>>>>>> b")).isEmpty(), "eight '=' is no separator");
    expect(findConflicts(lines("<<<<<<<a\nx\n=======\ny\n>>>>>>> b")).isEmpty(), "a label needs a space");
    expect(findConflicts(lines("Title\n=======\ntext\n>>>>>>> b")).isEmpty(), "separator outside a block");
    expect(findConflicts(lines("<<<<<<< a\nx\n>>>>>>> b")).isEmpty(), "no separator");
    expect(findConflicts(lines("<<<<<<< a\nx\n=======\ny")).isEmpty(), "no end");
    const QList<Conflict> restarted = findConflicts(lines("<<<<<<< a\nx\n<<<<<<< c\ny\n=======\nz\n>>>>>>> d"));
    expect(restarted.size() == 1 && restarted.at(0).start == 2 && restarted.at(0).currentLabel == QLatin1String("c"), "a new start restarts the block");
    const QList<Conflict> inside = findConflicts(lines("<<<<<<<\nx\n=======\n=======\n|||||||\n>>>>>>>"));
    expect(inside.size() == 1 && inside.at(0).separator == 2 && inside.at(0).end == 5 && inside.at(0).currentLabel.isEmpty(),
           "markers without labels; '=======' after the separator is text");
    const QList<Conflict> crlf = findConflicts(lines("<<<<<<< HEAD\r\nx\r\n=======\r\ny\r\n>>>>>>> b\r"));
    expect(crlf.size() == 1 && crlf.at(0).incomingLabel == QLatin1String("b"), "CRLF line ends");
    expect(isAnyConflictMarker(QStringLiteral("||||||| base")) && !isAnyConflictMarker(QStringLiteral("|||")) && !isAnyConflictMarker(QString()),
           "marker test");

    std::printf("%s\n", failures ? "conflicts_test: FAILED" : "conflicts_test: all passed");
    return failures ? 1 : 0;
}
