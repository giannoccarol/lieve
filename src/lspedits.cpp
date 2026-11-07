#include "lspedits.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QUrl>

#include <algorithm>
#include <tuple>

namespace Lieve
{
namespace
{
LspPosition positionOf(const QJsonObject &o)
{
    return {o.value(QLatin1String("line")).toInt(), o.value(QLatin1String("character")).toInt()};
}

void readRange(const QJsonObject &range, LspPosition *start, LspPosition *end)
{
    *start = positionOf(range.value(QLatin1String("start")).toObject());
    *end = positionOf(range.value(QLatin1String("end")).toObject());
}

// TextEdit and AnnotatedTextEdit (same fields); a SnippetTextEdit is not
// plain text and is left out.
QList<LspTextEdit> readEdits(const QJsonArray &edits)
{
    QList<LspTextEdit> out;
    for (const QJsonValue &value : edits) {
        const QJsonObject o = value.toObject();
        if (!o.value(QLatin1String("range")).isObject() || !o.value(QLatin1String("newText")).isString()) {
            continue;
        }
        LspTextEdit edit;
        readRange(o.value(QLatin1String("range")).toObject(), &edit.start, &edit.end);
        edit.newText = o.value(QLatin1String("newText")).toString();
        out.append(edit);
    }
    return out;
}

void addEdits(QList<FileTextEdits> &files, const QString &uri, const QList<LspTextEdit> &edits)
{
    if (uri.isEmpty() || edits.isEmpty()) {
        return;
    }
    for (FileTextEdits &file : files) {
        if (file.uri == uri) {
            file.edits << edits;
            return;
        }
    }
    files.append({uri, edits});
}

// Where each line starts: after \n, \r\n or a lone \r.
QList<qsizetype> lineStarts(const QString &text)
{
    QList<qsizetype> starts{0};
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            starts.append(i + 1);
        } else if (c == QLatin1Char('\n')) {
            starts.append(i + 1);
        }
    }
    return starts;
}

qsizetype offsetOf(const QString &text, const QList<qsizetype> &starts, LspPosition p)
{
    if (p.line < 0) {
        return 0;
    }
    if (p.line >= starts.size()) {
        return text.size();
    }
    const qsizetype start = starts.at(p.line);
    qsizetype end = p.line + 1 < starts.size() ? starts.at(p.line + 1) : text.size();
    if (end > start && text.at(end - 1) == QLatin1Char('\n')) {
        --end;
    }
    if (end > start && text.at(end - 1) == QLatin1Char('\r')) {
        --end;
    }
    return start + std::clamp<qsizetype>(p.character, 0, end - start);
}

// The text's first line break (\n when it has none).
QString lineBreakOf(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\n')) {
            return QStringLiteral("\n");
        }
        if (text.at(i) == QLatin1Char('\r')) {
            return i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n') ? QStringLiteral("\r\n") : QStringLiteral("\r");
        }
    }
    return QStringLiteral("\n");
}

QString pathOf(const QString &uri)
{
    const QUrl url(uri);
    return url.isLocalFile() ? url.toLocalFile() : QString();
}
}

int WorkspaceEditPlan::editCount() const
{
    int count = 0;
    for (const FileTextEdits &file : files) {
        count += int(file.edits.size());
    }
    return count;
}

WorkspaceEditPlan parseWorkspaceEdit(const QJsonObject &edit)
{
    WorkspaceEditPlan plan;
    const QJsonValue documentChanges = edit.value(QLatin1String("documentChanges"));
    if (documentChanges.isArray()) {
        const QJsonArray changes = documentChanges.toArray();
        for (const QJsonValue &value : changes) {
            const QJsonObject o = value.toObject();
            const QString kind = o.value(QLatin1String("kind")).toString();
            if (kind.isEmpty()) { // TextDocumentEdit
                addEdits(plan.files, o.value(QLatin1String("textDocument")).toObject().value(QLatin1String("uri")).toString(),
                         readEdits(o.value(QLatin1String("edits")).toArray()));
            } else if (kind == QLatin1String("rename")) {
                plan.operations.append({kind, o.value(QLatin1String("oldUri")).toString(), o.value(QLatin1String("newUri")).toString()});
            } else if (kind == QLatin1String("create") || kind == QLatin1String("delete")) {
                plan.operations.append({kind, o.value(QLatin1String("uri")).toString(), QString()});
            }
        }
        return plan;
    }
    const QJsonObject changes = edit.value(QLatin1String("changes")).toObject();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        addEdits(plan.files, it.key(), readEdits(it.value().toArray()));
    }
    return plan;
}

bool applyTextEdits(QString &text, const QList<LspTextEdit> &edits)
{
    struct Change {
        qsizetype from;
        qsizetype to;
        QString with;
        int order;
    };
    const QList<qsizetype> starts = lineStarts(text);
    const QString lineBreak = lineBreakOf(text);
    QList<Change> changes;
    changes.reserve(edits.size());
    for (qsizetype i = 0; i < edits.size(); ++i) {
        const LspTextEdit &edit = edits.at(i);
        const qsizetype from = offsetOf(text, starts, edit.start);
        const qsizetype to = offsetOf(text, starts, edit.end);
        if (to < from) {
            return false;
        }
        QString with = edit.newText;
        if (lineBreak != QLatin1String("\n") && (with.contains(QLatin1Char('\n')) || with.contains(QLatin1Char('\r')))) {
            with.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            with.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            with.replace(QStringLiteral("\n"), lineBreak);
        }
        changes.append({from, to, with, int(i)});
    }
    // In text order; inserts at the same place keep the server's order and
    // come before a replacement that starts there.
    std::sort(changes.begin(), changes.end(), [](const Change &a, const Change &b) {
        return std::make_tuple(a.from, a.to != a.from, a.order) < std::make_tuple(b.from, b.to != b.from, b.order);
    });
    for (qsizetype i = 1; i < changes.size(); ++i) {
        if (changes.at(i).from < changes.at(i - 1).to) {
            return false;
        }
    }
    QString out;
    out.reserve(text.size() + 64);
    qsizetype pos = 0;
    for (const Change &c : std::as_const(changes)) {
        out += QStringView(text).mid(pos, c.from - pos);
        out += c.with;
        pos = c.to;
    }
    out += QStringView(text).mid(pos);
    text = out;
    return true;
}

FileEditResult applyTextEditsToFile(const QString &path, const QList<LspTextEdit> &edits, QString *error)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        return FileEditResult::Missing;
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        return FileEditResult::Unreadable;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    QStringDecoder decoder(QStringConverter::Utf8); // drops a byte order mark, put back below
    QString text = decoder.decode(bytes);
    if (bytes.contains('\0') || decoder.hasError()) {
        return FileEditResult::NotText;
    }
    if (!applyTextEdits(text, edits)) {
        return FileEditResult::Overlapping;
    }
    QByteArray data = bytes.startsWith("\xEF\xBB\xBF") ? QByteArray("\xEF\xBB\xBF") : QByteArray();
    QStringEncoder encoder(QStringConverter::Utf8);
    data += encoder.encode(text);
    if (data == bytes) {
        return FileEditResult::Done;
    }
    if (!QFileInfo(canonical).isWritable()) {
        return FileEditResult::ReadOnly;
    }
    QSaveFile save(canonical); // keeps the permissions
    if (!save.open(QIODevice::WriteOnly) || save.write(data) != data.size() || !save.commit()) {
        if (error) {
            *error = save.errorString();
        }
        return FileEditResult::WriteFailed;
    }
    return FileEditResult::Done;
}

QStringList readTextLines(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (file.size() > maxBytes || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (bytes.contains('\0')) {
        return {};
    }
    QStringDecoder decoder(QStringConverter::Utf8);
    const QString text = decoder.decode(bytes);
    QStringList lines;
    const QList<qsizetype> starts = lineStarts(text);
    for (qsizetype i = 0; i < starts.size(); ++i) {
        qsizetype end = i + 1 < starts.size() ? starts.at(i + 1) : text.size();
        if (end > starts.at(i) && text.at(end - 1) == QLatin1Char('\n')) {
            --end;
        }
        if (end > starts.at(i) && text.at(end - 1) == QLatin1Char('\r')) {
            --end;
        }
        lines.append(text.mid(starts.at(i), end - starts.at(i)));
    }
    return lines;
}

QString serverErrorText(const QString &message)
{
    // vscode-languageserver wraps what a handler threw.
    static const QRegularExpression wrapped(QStringLiteral("^Request \\S+ failed with (?:message|unhandled error): "));
    QString text = message;
    text.remove(wrapped);
    return text.section(QLatin1Char('\n'), 0, 0).trimmed();
}

// ---------------------------------------------------------------------------

QList<ReferenceLocation> parseLocations(const QJsonValue &result)
{
    const QJsonArray list = result.isArray() ? result.toArray() : result.isObject() ? QJsonArray{result} : QJsonArray();
    QList<ReferenceLocation> out;
    for (const QJsonValue &value : list) {
        const QJsonObject o = value.toObject();
        const bool link = o.contains(QLatin1String("targetUri"));
        ReferenceLocation location;
        location.path = pathOf(o.value(link ? QLatin1String("targetUri") : QLatin1String("uri")).toString());
        if (location.path.isEmpty()) {
            continue;
        }
        readRange(o.value(link ? QLatin1String("targetSelectionRange") : QLatin1String("range")).toObject(), &location.start, &location.end);
        out.append(location);
    }
    return out;
}

QList<ReferenceFile> groupReferences(const QList<ReferenceLocation> &locations, const QString &first)
{
    struct Item {
        QString path;
        int line;
        int start;
        int end;
        bool operator==(const Item &) const = default;
    };
    QList<Item> items;
    items.reserve(locations.size());
    for (const ReferenceLocation &l : locations) {
        if (!l.path.isEmpty()) {
            items.append({l.path, l.start.line, l.start.character, l.end.line == l.start.line ? l.end.character : -1});
        }
    }
    const auto fileOrder = [&first](const QString &a, const QString &b) {
        if ((a == first) != (b == first)) {
            return a == first;
        }
        const int folded = a.compare(b, Qt::CaseInsensitive);
        return folded != 0 ? folded < 0 : a < b;
    };
    std::sort(items.begin(), items.end(), [&fileOrder](const Item &a, const Item &b) {
        if (a.path != b.path) {
            return fileOrder(a.path, b.path);
        }
        return std::make_tuple(a.line, a.start, a.end) < std::make_tuple(b.line, b.start, b.end);
    });
    items.erase(std::unique(items.begin(), items.end()), items.end());

    QList<ReferenceFile> files;
    for (const Item &item : std::as_const(items)) {
        if (files.isEmpty() || files.last().path != item.path) {
            files.append(ReferenceFile{item.path, {}, 0});
        }
        ReferenceFile &file = files.last();
        if (file.lines.isEmpty() || file.lines.last().line != item.line) {
            file.lines.append(ReferenceLine{item.line, {}});
        }
        file.lines.last().ranges.append({item.start, item.end});
        ++file.count;
    }
    return files;
}

PreviewLine previewLine(const QString &line, const QList<QPair<int, int>> &ranges, int lead, int maxLength)
{
    PreviewLine out;
    int shift = 0;
    while (shift < line.size() && line.at(shift).isSpace()) {
        ++shift;
    }
    QString shown = line.mid(shift);
    const int firstStart = ranges.isEmpty() ? shift : std::max(shift, ranges.first().first);
    if (firstStart - shift > lead + 4) {
        const int cut = firstStart - shift - lead;
        shown = QStringLiteral("…") + shown.mid(cut);
        shift += cut - 1; // the … takes one character
    }
    if (shown.size() > maxLength) {
        shown.truncate(maxLength);
    }
    shown.replace(QLatin1Char('\t'), QLatin1Char(' '));
    for (const auto &[start, end] : ranges) {
        const int from = std::clamp(start - shift, 0, int(shown.size()));
        const int to = std::clamp((end < 0 ? int(line.size()) : end) - shift, from, int(shown.size()));
        if (to > from) {
            out.ranges.append({from, to - from});
        }
    }
    out.text = shown;
    return out;
}

int referenceCount(const QJsonObject &codeLens)
{
    const QJsonObject command = codeLens.value(QLatin1String("command")).toObject();
    if (command.isEmpty()) {
        return -1;
    }
    const QJsonArray arguments = command.value(QLatin1String("arguments")).toArray();
    if (command.value(QLatin1String("command")).toString() == QLatin1String("editor.action.showReferences") && arguments.size() >= 3
        && arguments.at(2).isArray()) {
        return int(arguments.at(2).toArray().size());
    }
    // typescript-language-server gives no command for "0 references".
    static const QRegularExpression count(QStringLiteral("^\\s*(\\d+)\\s+references?\\s*$"));
    const QRegularExpressionMatch match = count.match(command.value(QLatin1String("title")).toString());
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

bool wantsReferenceCount(int kind, int parentKind)
{
    const auto typeLike = [](int k) {
        return k == 5 || k == 11 || k == 23; // class, interface, struct
    };
    switch (kind) {
    case 5: // class
    case 10: // enum
    case 11: // interface
    case 12: // function
    case 23: // struct
        return true;
    case 22: // enum member
        return parentKind == 10;
    case 6: // method
    case 7: // property
    case 8: // field
    case 9: // constructor
        return typeLike(parentKind);
    default:
        return false;
    }
}
}
