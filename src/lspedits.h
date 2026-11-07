#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// The plain parts of Go to References, Rename Symbol and the reference
// counts (QtCore only, tested in tests/lspedits_test.cpp): language server
// answers turned into data, text edits applied to strings and to files.
namespace Lieve
{
// A position as language servers count it: 0-based line, UTF-16 column.
struct LspPosition {
    int line = 0;
    int character = 0;
    friend bool operator==(const LspPosition &, const LspPosition &) = default;
    friend auto operator<=>(const LspPosition &, const LspPosition &) = default;
};

struct LspTextEdit {
    LspPosition start;
    LspPosition end;
    QString newText;
};

// The edits of one file, in the server's order.
struct FileTextEdits {
    QString uri;
    QList<LspTextEdit> edits;
};

// A file the server wants created, renamed or deleted.
struct ResourceOperation {
    QString kind; // "create", "rename" or "delete"
    QString uri; // the file (for a rename, its old name)
    QString newUri; // a rename's new name
};

struct WorkspaceEditPlan {
    QList<FileTextEdits> files; // one entry per file that has edits
    QList<ResourceOperation> operations;
    int editCount() const;
};

// A WorkspaceEdit: its `documentChanges` (text edits and file operations)
// when present, else its `changes`. Edits of the same file are merged.
WorkspaceEditPlan parseWorkspaceEdit(const QJsonObject &edit);

// Applies `edits` to `text`, whose lines end with \n, \r\n or \r. A column
// past the end of a line means its end, a line past the last one the end of
// the text; line breaks in the new text become the text's own. False (and
// `text` unchanged) if two edits overlap or a range ends before it starts.
bool applyTextEdits(QString &text, const QList<LspTextEdit> &edits);

enum class FileEditResult {
    Done,
    Missing,
    Unreadable,
    NotText, // binary or not UTF-8: would not survive the round trip
    Overlapping,
    ReadOnly,
    WriteFailed,
};
// Applies `edits` to the file at `path` (through a link to its target),
// only if it is UTF-8 text, keeping its byte order mark, line endings and
// permissions. `error` gets the reason of a failed write.
FileEditResult applyTextEditsToFile(const QString &path, const QList<LspTextEdit> &edits, QString *error = nullptr);

// The lines of a text file (without line breaks), for previews; empty if
// it cannot be read, is binary or is larger than `maxBytes`.
QStringList readTextLines(const QString &path, qint64 maxBytes = 8 * 1024 * 1024);

// "Request textDocument/rename failed with message: You cannot rename
// this element." -> "You cannot rename this element."
QString serverErrorText(const QString &message);

// ---------------------------------------------------------------------------
// References

struct ReferenceLocation {
    QString path; // local file
    LspPosition start;
    LspPosition end;
};
// Location[] or LocationLink[] (also a single one), local files only.
QList<ReferenceLocation> parseLocations(const QJsonValue &result);

struct ReferenceLine {
    int line = 0;
    QList<QPair<int, int>> ranges; // start and end column on the line, sorted; end -1: to the end of the line
};
struct ReferenceFile {
    QString path;
    QList<ReferenceLine> lines; // sorted
    int count = 0; // references in the file
};
// Grouped by file, the file `first` first and the others by path, then by
// line, without duplicates.
QList<ReferenceFile> groupReferences(const QList<ReferenceLocation> &locations, const QString &first);

// A line as a list shows it: without its indentation, from a little before
// the first range when that is far in (cut text marked with …), at most
// `maxLength` characters, tabs as spaces. Its ranges as (start, length).
struct PreviewLine {
    QString text;
    QList<QPair<int, int>> ranges;
};
PreviewLine previewLine(const QString &line, const QList<QPair<int, int>> &ranges, int lead = 40, int maxLength = 300);

// The number of references of a resolved code lens, from the locations of
// its editor.action.showReferences command or its title ("3 references");
// -1 if it is not resolved or not a reference count.
int referenceCount(const QJsonObject &codeLens);

// Without code lenses from the server, the document symbols that get a
// count (like VS Code's TypeScript lenses with showOnAllFunctions):
// classes, interfaces, structs, enums and their members, functions, and
// the methods, constructors, properties and fields of classes, interfaces
// and structs. `kind` and `parentKind` are LSP SymbolKinds (0: top level).
bool wantsReferenceCount(int kind, int parentKind);
}
