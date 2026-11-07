// Rename Symbol and Go to References without a server: WorkspaceEdits read
// into per-file edits, edits applied to text and to files (BOM, line
// endings, permissions), references grouped and previewed, code lens counts.
#include "lspedits.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

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

void check(const QString &got, const QString &wanted, const char *what)
{
    expect(got == wanted, what);
    if (got != wanted) {
        std::printf("     got:    %s\n     wanted: %s\n", qPrintable(got), qPrintable(wanted));
    }
}

QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

LspTextEdit edit(int line, int character, int endLine, int endCharacter, const QString &text)
{
    return {{line, character}, {endLine, endCharacter}, text};
}

QString applied(QString text, const QList<LspTextEdit> &edits)
{
    return applyTextEdits(text, edits) ? text : QStringLiteral("(refused)");
}

QString visible(QString s)
{
    s.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    s.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return s;
}

QString dump(const QList<ReferenceFile> &files)
{
    QStringList out;
    for (const ReferenceFile &f : files) {
        QStringList lines;
        for (const ReferenceLine &l : f.lines) {
            QStringList ranges;
            for (const auto &[start, end] : l.ranges) {
                ranges << QStringLiteral("%1-%2").arg(start).arg(end);
            }
            lines << QStringLiteral("%1[%2]").arg(l.line + 1).arg(ranges.join(QLatin1Char(',')));
        }
        out << QStringLiteral("%1(%2): %3").arg(f.path).arg(f.count).arg(lines.join(QLatin1Char(' ')));
    }
    return out.join(QStringLiteral(" | "));
}

QString shown(const PreviewLine &p)
{
    QString out = p.text;
    for (qsizetype i = p.ranges.size() - 1; i >= 0; --i) {
        out.insert(p.ranges.at(i).first + p.ranges.at(i).second, QLatin1Char(']'));
        out.insert(p.ranges.at(i).first, QLatin1Char('['));
    }
    return out;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(bytes);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- WorkspaceEdit
    {
        const WorkspaceEditPlan plan = parseWorkspaceEdit(json(R"({"changes": {
            "file:///p/b.ts": [{"range": {"start": {"line": 2, "character": 4}, "end": {"line": 2, "character": 9}}, "newText": "hello"}],
            "file:///p/a.ts": [{"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 5}}, "newText": "hello"},
                               {"range": {"start": {"line": 7, "character": 1}, "end": {"line": 7, "character": 6}}, "newText": "hello"}],
            "file:///p/empty.ts": []}})"));
        expect(plan.files.size() == 2 && plan.operations.isEmpty(), "changes: two files, the one without edits left out");
        expect(plan.editCount() == 3, "changes: three edits");
        expect(plan.files.size() == 2 && plan.files.at(0).uri == QLatin1String("file:///p/a.ts") && plan.files.at(0).edits.size() == 2
                   && plan.files.at(0).edits.at(1).start == LspPosition{7, 1} && plan.files.at(0).edits.at(1).end == LspPosition{7, 6},
               "changes: positions read");
    }
    {
        const WorkspaceEditPlan plan = parseWorkspaceEdit(json(R"({
            "changes": {"file:///ignored.ts": [{"range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}}, "newText": "x"}]},
            "documentChanges": [
              {"textDocument": {"uri": "file:///p/a.ts", "version": 3},
               "edits": [{"range": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 3}}, "newText": "Foo"},
                         {"range": {"start": {"line": 4, "character": 2}, "end": {"line": 4, "character": 5}}, "newText": "Foo", "annotationId": "a"},
                         {"range": {"start": {"line": 6, "character": 0}, "end": {"line": 6, "character": 0}}, "snippet": {"kind": "snippet", "value": "$0"}}]},
              {"kind": "rename", "oldUri": "file:///p/foo.ts", "newUri": "file:///p/bar.ts"},
              {"textDocument": {"uri": "file:///p/a.ts", "version": 3},
               "edits": [{"range": {"start": {"line": 9, "character": 0}, "end": {"line": 9, "character": 3}}, "newText": "Foo"}]},
              {"kind": "create", "uri": "file:///p/new.ts"},
              {"kind": "delete", "uri": "file:///p/old.ts"}]})"));
        expect(plan.files.size() == 1 && plan.files.at(0).edits.size() == 3, "documentChanges win over changes; same file merged; snippets left out");
        expect(plan.operations.size() == 3 && plan.operations.at(0).kind == QLatin1String("rename")
                   && plan.operations.at(0).newUri == QLatin1String("file:///p/bar.ts") && plan.operations.at(1).kind == QLatin1String("create")
                   && plan.operations.at(2).uri == QLatin1String("file:///p/old.ts"),
               "documentChanges: file operations listed");
    }
    expect(parseWorkspaceEdit(QJsonObject()).files.isEmpty(), "empty edit");

    // ---- Text edits
    const QString text = QStringLiteral("function greet() {}\ngreet();\n  greet(); greet();\n");
    check(applied(text, {edit(0, 9, 0, 14, QStringLiteral("hello")), edit(1, 0, 1, 5, QStringLiteral("hello")), edit(2, 2, 2, 7, QStringLiteral("hello")),
                         edit(2, 11, 2, 16, QStringLiteral("hello"))}),
          QStringLiteral("function hello() {}\nhello();\n  hello(); hello();\n"),
          "rename: edits in any line, two on one line");
    check(applied(text, {edit(2, 11, 2, 16, QStringLiteral("x")), edit(0, 9, 0, 14, QStringLiteral("x"))}),
          QStringLiteral("function x() {}\ngreet();\n  greet(); x();\n"),
          "edits in any order");
    check(applied(QStringLiteral("ab"), {edit(0, 1, 0, 1, QStringLiteral("1")), edit(0, 1, 0, 1, QStringLiteral("2"))}), QStringLiteral("a12b"),
          "inserts at one place keep their order");
    check(applied(QStringLiteral("abc"), {edit(0, 1, 0, 2, QStringLiteral("X")), edit(0, 1, 0, 1, QStringLiteral("+"))}), QStringLiteral("a+Xc"),
          "an insert before a replacement at the same place");
    check(applied(QStringLiteral("abcdef"), {edit(0, 1, 0, 4, QStringLiteral("X")), edit(0, 3, 0, 5, QStringLiteral("Y"))}), QStringLiteral("(refused)"),
          "overlapping edits refused");
    check(applied(QStringLiteral("abc"), {edit(0, 2, 0, 1, QStringLiteral("X"))}), QStringLiteral("(refused)"), "a range ending before its start refused");
    check(applied(QStringLiteral("ab\ncd"), {edit(0, 99, 0, 99, QStringLiteral("!")), edit(9, 0, 9, 0, QStringLiteral("?"))}), QStringLiteral("ab!\ncd?"),
          "past the end of a line, past the last line");
    check(visible(applied(QStringLiteral("one\r\ntwo\r\nthree"), {edit(1, 0, 1, 3, QStringLiteral("2")), edit(2, 5, 2, 5, QStringLiteral("\nfour"))})),
          QStringLiteral("one\\r\\n2\\r\\nthree\\r\\nfour"),
          "CRLF text: lines counted, new line breaks as CRLF");
    check(visible(applied(QStringLiteral("a\rb\rc"), {edit(2, 0, 2, 1, QStringLiteral("C"))})), QStringLiteral("a\\rb\\rC"), "old Mac line ends");
    check(applied(QStringLiteral("x = \"😀\"; y = x;"), {edit(0, 10, 0, 11, QStringLiteral("z"))}), QStringLiteral("x = \"😀\"; z = x;"),
          "UTF-16 columns after a surrogate pair");
    check(applied(QStringLiteral("abc\ndef"), {edit(0, 2, 1, 1, QString())}), QStringLiteral("abef"), "an edit across lines");

    // ---- Files on disk
    {
        QTemporaryDir dir;
        const QString crlf = dir.filePath(QStringLiteral("crlf.ts"));
        writeFile(crlf, QByteArray("\xEF\xBB\xBF") + "export function greet() {}\r\ngreet();\r\n");
        QFile::setPermissions(crlf, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup);
        expect(applyTextEditsToFile(crlf, {edit(0, 16, 0, 21, QStringLiteral("hello")), edit(1, 0, 1, 5, QStringLiteral("hello"))}) == FileEditResult::Done,
               "file: rewritten");
        expect(readFile(crlf) == QByteArray("\xEF\xBB\xBF") + "export function hello() {}\r\nhello();\r\n", "file: BOM and CRLF kept");
        const QFileDevice::Permissions user = QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser;
        expect((QFile::permissions(crlf) & ~user) == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup),
               "file: permissions kept");

        const QString link = dir.filePath(QStringLiteral("link.ts"));
        QFile::link(crlf, link);
        expect(applyTextEditsToFile(link, {edit(1, 0, 1, 5, QStringLiteral("hi"))}) == FileEditResult::Done && QFileInfo(link).isSymLink()
                   && readFile(crlf).endsWith("\r\nhi();\r\n"),
               "file: a link stays a link, its target is changed");

        const QString latin1 = dir.filePath(QStringLiteral("latin1.ts"));
        writeFile(latin1, "const caf\xE9 = 1;\n");
        expect(applyTextEditsToFile(latin1, {edit(0, 0, 0, 5, QStringLiteral("let"))}) == FileEditResult::NotText && readFile(latin1) == "const caf\xE9 = 1;\n",
               "file: not UTF-8, left alone");
        const QString binary = dir.filePath(QStringLiteral("binary.ts"));
        writeFile(binary, QByteArray("ab\0cd", 5));
        expect(applyTextEditsToFile(binary, {edit(0, 0, 0, 1, QStringLiteral("x"))}) == FileEditResult::NotText, "file: binary, left alone");
        expect(applyTextEditsToFile(dir.filePath(QStringLiteral("missing.ts")), {edit(0, 0, 0, 1, QStringLiteral("x"))}) == FileEditResult::Missing,
               "file: missing");
        const QString overlap = dir.filePath(QStringLiteral("overlap.ts"));
        writeFile(overlap, "abcdef\n");
        expect(applyTextEditsToFile(overlap, {edit(0, 0, 0, 3, QStringLiteral("x")), edit(0, 2, 0, 4, QStringLiteral("y"))}) == FileEditResult::Overlapping
                   && readFile(overlap) == "abcdef\n",
               "file: overlapping edits, left alone");
        const QString readOnly = dir.filePath(QStringLiteral("readonly.ts"));
        writeFile(readOnly, "abc\n");
        QFile::setPermissions(readOnly, QFileDevice::ReadOwner);
        // root (as in CI containers) may write any file: nothing to check then
        if (!QFileInfo(readOnly).isWritable()) {
            expect(applyTextEditsToFile(readOnly, {edit(0, 0, 0, 1, QStringLiteral("x"))}) == FileEditResult::ReadOnly && readFile(readOnly) == "abc\n",
                   "file: read-only, left alone");
        }

        const QString lines = dir.filePath(QStringLiteral("lines.ts"));
        writeFile(lines, "\xEF\xBB\xBFone\r\ntwo\nthree");
        check(readTextLines(lines).join(QLatin1Char('|')), QStringLiteral("one|two|three"), "preview lines of a file");
        expect(readTextLines(binary).isEmpty() && readTextLines(dir.filePath(QStringLiteral("missing.ts"))).isEmpty(), "no preview lines of binary or missing files");
    }

    check(serverErrorText(QStringLiteral("Request textDocument/prepareRename failed with message: You cannot rename this element.")),
          QStringLiteral("You cannot rename this element."),
          "server error text unwrapped");
    check(serverErrorText(QStringLiteral("Cannot rename\n    at stack")), QStringLiteral("Cannot rename"), "server error text: first line");

    // ---- References
    {
        const QList<ReferenceLocation> locations = parseLocations(QJsonDocument::fromJson(R"([
            {"uri": "file:///p/src/z.ts", "range": {"start": {"line": 4, "character": 2}, "end": {"line": 4, "character": 7}}},
            {"uri": "file:///p/main.ts", "range": {"start": {"line": 9, "character": 10}, "end": {"line": 9, "character": 15}}},
            {"uri": "file:///p/main.ts", "range": {"start": {"line": 1, "character": 9}, "end": {"line": 1, "character": 14}}},
            {"uri": "file:///p/main.ts", "range": {"start": {"line": 9, "character": 0}, "end": {"line": 9, "character": 5}}},
            {"uri": "file:///p/main.ts", "range": {"start": {"line": 9, "character": 0}, "end": {"line": 9, "character": 5}}},
            {"uri": "file:///p/A.ts", "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 1}}},
            {"targetUri": "file:///p/b.ts", "targetRange": {"start": {"line": 0, "character": 0}, "end": {"line": 9, "character": 0}},
             "targetSelectionRange": {"start": {"line": 3, "character": 6}, "end": {"line": 3, "character": 11}}},
            {"uri": "untitled:Untitled-1", "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}}}])")
                                                                         .array());
        expect(locations.size() == 7, "locations: local files only");
        check(dump(groupReferences(locations, QStringLiteral("/p/main.ts"))),
              QStringLiteral("/p/main.ts(3): 2[9-14] 10[0-5,10-15] | /p/A.ts(1): 1[0--1] | /p/b.ts(1): 4[6-11] | /p/src/z.ts(1): 5[2-7]"),
              "grouped: the asking file first, then by path; lines sorted; duplicates dropped; multi-line ranges to the line end");
        expect(parseLocations(QJsonDocument::fromJson(R"({"uri": "file:///p/a.ts", "range": {"start": {"line": 1, "character": 2}, "end": {"line": 1, "character": 3}}})")
                                  .object())
                       .size()
                   == 1,
               "a single location");
        expect(parseLocations(QJsonValue()).isEmpty(), "null result");
    }

    // ---- Previews
    check(shown(previewLine(QStringLiteral("    const r = greet(name);"), {{14, 19}})), QStringLiteral("const r = [greet](name);"), "preview: no indentation");
    check(shown(previewLine(QStringLiteral("\tgreet(); greet();"), {{1, 6}, {10, 15}})), QStringLiteral("[greet](); [greet]();"), "preview: two ranges");
    check(shown(previewLine(QStringLiteral("  ") + QString(60, QLatin1Char('x')) + QStringLiteral(" greet()"), {{63, 68}}, 10)),
          QStringLiteral("…xxxxxxxxx [greet]()"),
          "preview: starts a little before a range far in");
    check(shown(previewLine(QStringLiteral("abc\tdef"), {{4, -1}})), QStringLiteral("abc [def]"), "preview: tabs as spaces, range to the end");
    check(shown(previewLine(QStringLiteral("greet greet"), {{6, 11}}, 40, 5)), QStringLiteral("greet"), "preview: at most maxLength, ranges past it dropped");
    check(shown(previewLine(QStringLiteral("greet greet"), {{6, 11}}, 40, 7)), QStringLiteral("greet [g]"), "preview: a range cut by maxLength");

    // ---- Code lens counts
    expect(referenceCount(json(R"({"range": {}, "command": {"title": "3 references", "command": "editor.action.showReferences",
                                    "arguments": ["file:///a.ts", {"line": 1, "character": 2}, [{}, {}, {}]]}})"))
               == 3,
           "lens: count from the locations");
    expect(referenceCount(json(R"({"command": {"title": "0 references", "command": "", "arguments": ["file:///a.ts", {}, []]}})")) == 0,
           "lens: 0 references (no command)");
    expect(referenceCount(json(R"({"command": {"title": "1 reference", "command": "x"}})")) == 1, "lens: count from the title");
    expect(referenceCount(json(R"({"range": {}, "data": {"type": 0}})")) == -1, "lens: not resolved");
    expect(referenceCount(json(R"({"command": {"title": "", "command": ""}})")) == -1, "lens: cancelled");
    expect(referenceCount(json(R"({"command": {"title": "Could not determine references", "command": ""}})")) == -1, "lens: failed");
    expect(referenceCount(json(R"({"command": {"title": "2 implementations", "command": "editor.action.showReferences", "arguments": [1, 2]}})")) == -1,
           "lens: implementations are not references");

    expect(wantsReferenceCount(5, 0) && wantsReferenceCount(11, 0) && wantsReferenceCount(12, 0) && wantsReferenceCount(10, 0), "symbols: types and functions");
    expect(wantsReferenceCount(6, 5) && wantsReferenceCount(7, 11) && wantsReferenceCount(8, 5) && wantsReferenceCount(9, 5) && wantsReferenceCount(22, 10),
           "symbols: members of classes, interfaces, enums");
    expect(!wantsReferenceCount(13, 0) && !wantsReferenceCount(14, 0) && !wantsReferenceCount(6, 13) && !wantsReferenceCount(7, 12) && !wantsReferenceCount(22, 5),
           "symbols: not variables, object literal members, locals");

    std::printf("%s\n", failures ? "lspedits: FAILED" : "lspedits: all passed");
    return failures ? 1 : 0;
}
