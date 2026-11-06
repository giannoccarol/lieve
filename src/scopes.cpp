#include "scopes.h"

#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>

#include <algorithm>
#include <climits>

namespace Lieve
{
namespace
{
bool before(int line, int column, int otherLine, int otherColumn)
{
    return line < otherLine || (line == otherLine && column < otherColumn);
}

bool contains(const Scope &outer, const Scope &inner)
{
    return !before(inner.startLine, inner.startColumn, outer.startLine, outer.startColumn)
        && !before(outer.endLine, outer.endColumn, inner.endLine, inner.endColumn);
}

bool containsPosition(const Scope &s, int line, int column)
{
    return !before(line, column, s.startLine, s.startColumn) && !before(s.endLine, s.endColumn, line, column);
}

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$';
}

int visualIndent(const QString &line, int tabWidth)
{
    int width = 0;
    for (const QChar c : line) {
        if (c == u' ') {
            ++width;
        } else if (c == u'\t') {
            width += tabWidth - width % tabWidth;
        } else {
            return width;
        }
    }
    return -1; // blank
}

// ---------------------------------------------------------------------------
// Reading the text: code, comments and strings, line by line

struct LineInfo {
    bool code = false; // has code: not only blanks, comments or the rest of a multi-line string
    int indent = -1;
    int commentAt = -1; // where a line comment starts
    int parens = 0; // parentheses open at the start of the line
};

struct Open {
    int line;
    int column;
    int parens; // parentheses open before it
    int calleeLine; // the innermost of them
    int calleeColumn;
    bool inCall; // an argument of a call made in the enclosing block: a lambda, a callback
    QChar ch;
};

struct Block {
    Open open;
    int closeLine;
};

class Lexer
{
public:
    explicit Lexer(const ScopeOptions &options)
        : m_o(options)
        , m_multilineBackticks(options.style == ScopeOptions::Style::Auto)
        , m_rawStrings(options.style == ScopeOptions::Style::Auto)
    {
    }

    // Reads the next line; blocks it closes (opened on an earlier line) go to `blocks`.
    LineInfo read(const QString &s, int line, std::vector<Block> &blocks)
    {
        LineInfo info;
        info.parens = int(m_parens.size());
        info.indent = visualIndent(s, std::max(1, m_o.tabWidth));
        const int n = int(s.size());
        const QStringView view(s);
        int i = 0;
        while (i < n) {
            if (m_state == State::BlockComment || m_state == State::RawString) {
                const QString &end = m_state == State::BlockComment ? m_o.blockCommentEnd : m_rawEnd;
                const qsizetype at = s.indexOf(end, i);
                if (at < 0) {
                    i = n;
                } else {
                    i = int(at + end.size());
                    m_state = State::Code;
                }
                continue;
            }
            if (m_state == State::Quote) {
                while (i < n) {
                    if (s.at(i) == u'\\') {
                        i += 2;
                        continue;
                    }
                    if (s.at(i) == m_quote && (!m_triple || (i + 2 < n && s.at(i + 1) == m_quote && s.at(i + 2) == m_quote))) {
                        i += m_triple ? 3 : 1;
                        m_state = State::Code;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            const QChar c = s.at(i);
            if (c.isSpace()) {
                ++i;
                continue;
            }
            if (!m_o.lineComment.isEmpty() && view.mid(i).startsWith(m_o.lineComment)) {
                info.commentAt = i;
                break;
            }
            if (!m_o.blockCommentStart.isEmpty() && !m_o.blockCommentEnd.isEmpty() && view.mid(i).startsWith(m_o.blockCommentStart)) {
                m_state = State::BlockComment;
                i += int(m_o.blockCommentStart.size());
                continue;
            }
            info.code = true;
            if (c == u'"' || c == u'\'' || c == u'`') {
                if (c != u'`' && i + 2 < n && s.at(i + 1) == c && s.at(i + 2) == c) { // """ or '''
                    m_state = State::Quote;
                    m_quote = c;
                    m_triple = true;
                    i += 3;
                    continue;
                }
                if (c == u'`' && m_multilineBackticks) { // template literals, Go raw strings
                    m_state = State::Quote;
                    m_quote = c;
                    m_triple = false;
                    ++i;
                    continue;
                }
                if (c == u'"' && m_rawStrings && rawPrefix(s, i)) { // C++ R"delimiter(...)delimiter"
                    const qsizetype paren = s.indexOf(u'(', i + 1);
                    const QString delimiter = paren > i ? s.mid(i + 1, paren - i - 1) : QString();
                    if (paren > i && delimiter.size() <= 16 && !delimiter.contains(u' ') && !delimiter.contains(u'"')) {
                        m_rawEnd = QLatin1Char(')') + delimiter + QLatin1Char('"');
                        m_state = State::RawString;
                        i = int(paren) + 1;
                        continue;
                    }
                }
                int j = i + 1;
                while (j < n && s.at(j) != c) {
                    j += s.at(j) == u'\\' ? 2 : 1;
                }
                // A string ends on its line; a lone quote is an apostrophe, a Rust lifetime...
                i = j < n ? j + 1 : i + 1;
                continue;
            }
            switch (c.unicode()) {
            case '(':
                m_parens.append({line, i});
                break;
            case ')':
                if (!m_parens.isEmpty()) {
                    m_parens.removeLast();
                }
                break;
            case '{':
            case '[': {
                const QPair<int, int> callee = m_parens.isEmpty() ? qMakePair(-1, -1) : m_parens.last();
                const int enclosing = m_open.empty() ? 0 : m_open.back().parens;
                m_open.push_back({line, i, int(m_parens.size()), callee.first, callee.second, int(m_parens.size()) > enclosing, c});
                break;
            }
            case '}':
            case ']': {
                const QChar opening = c == u'}' ? QChar(u'{') : QChar(u'[');
                int k = int(m_open.size()) - 1;
                while (k >= 0 && m_open[k].ch != opening) {
                    --k;
                }
                if (k >= 0) {
                    const Open o = m_open[k];
                    m_open.resize(k);
                    while (m_parens.size() > o.parens) { // left open inside the block
                        m_parens.removeLast();
                    }
                    if (o.line < line) {
                        blocks.push_back({o, line});
                    }
                }
                break;
            }
            default:
                break;
            }
            ++i;
        }
        return info;
    }

private:
    enum class State {
        Code,
        BlockComment,
        Quote, // a string that can span lines: """, ''' or `
        RawString,
    };

    static bool rawPrefix(const QString &s, int quote)
    {
        const int r = quote - 1;
        if (r < 0 || s.at(r) != u'R') {
            return false;
        }
        int start = r;
        if (r >= 2 && s.at(r - 2) == u'u' && s.at(r - 1) == u'8') {
            start = r - 2;
        } else if (r >= 1 && (s.at(r - 1) == u'L' || s.at(r - 1) == u'u' || s.at(r - 1) == u'U')) {
            start = r - 1;
        }
        return start == 0 || !isIdentChar(s.at(start - 1));
    }

    const ScopeOptions &m_o;
    const bool m_multilineBackticks;
    const bool m_rawStrings;
    State m_state = State::Code;
    QChar m_quote;
    bool m_triple = false;
    QString m_rawEnd;
    QList<QPair<int, int>> m_parens; // where each open parenthesis is
    std::vector<Open> m_open; // open braces and brackets
};

// ---------------------------------------------------------------------------
// Naming a block from its first line

struct Named {
    int kind = 0;
    QString name;
};

// Position of the first `wanted` outside strings and nested brackets, or -1.
int topLevel(const QString &t, QChar wanted)
{
    int depth = 0;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == u'"' || c == u'\'' || c == u'`') {
            int j = i + 1;
            while (j < t.size() && t.at(j) != c) {
                j += t.at(j) == u'\\' ? 2 : 1;
            }
            if (j < t.size()) {
                i = j;
                continue;
            }
        }
        if (depth == 0 && c == wanted) {
            if (wanted == u'=') { // an assignment, not == != <= >= => += ...
                const QChar next = i + 1 < t.size() ? t.at(i + 1) : QChar();
                const QChar previous = i > 0 ? t.at(i - 1) : QChar();
                if (next == u'=' || next == u'>' || QStringView(u"=!<>+-*/%&|^~").contains(previous)) {
                    continue;
                }
            }
            if (wanted == u':') { // a single colon, not ::
                const bool doubled = (i + 1 < t.size() && t.at(i + 1) == u':') || (i > 0 && t.at(i - 1) == u':');
                if (doubled) {
                    ++i;
                    continue;
                }
            }
            return i;
        }
        if (c == u'(' || c == u'[' || c == u'{') {
            ++depth;
        } else if ((c == u')' || c == u']' || c == u'}') && depth > 0) {
            --depth;
        }
    }
    return -1;
}

QString simplified(const QString &text)
{
    QString t = text.simplified();
    while (!t.isEmpty() && QStringView(u"{[(:").contains(t.back())) {
        t.chop(1);
        t = t.trimmed();
    }
    return t;
}

QString unquoted(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && (s.front() == u'"' || s.front() == u'\'') && s.back() == s.front()) {
        s = s.mid(1, s.size() - 2);
    }
    return s;
}

bool isKeyword(const QString &word)
{
    static const QSet<QString> keywords = {
        QStringLiteral("if"),      QStringLiteral("for"),    QStringLiteral("foreach"), QStringLiteral("while"),  QStringLiteral("switch"),
        QStringLiteral("catch"),   QStringLiteral("return"), QStringLiteral("sizeof"),  QStringLiteral("new"),    QStringLiteral("delete"),
        QStringLiteral("throw"),   QStringLiteral("typeof"), QStringLiteral("await"),   QStringLiteral("yield"),  QStringLiteral("assert"),
        QStringLiteral("elif"),    QStringLiteral("with"),   QStringLiteral("match"),   QStringLiteral("decltype"), QStringLiteral("alignof"),
        QStringLiteral("static_assert"),
    };
    return keywords.contains(word);
}

// The kind (LSP SymbolKind) and name of a block from the code of its first
// line; no name for plain blocks (if, for, else...). `callee`: the block is
// an argument of that call (a lambda, a callback).
Named classify(const QString &code, bool css, const QString &callee)
{
    QString raw = code.trimmed();
    while (!raw.isEmpty() && QStringView(u"}]),").contains(raw.front())) { // "} else {", "}, {"
        raw = raw.mid(1).trimmed();
    }
    const QString t = simplified(raw);
    if (t.isEmpty()) {
        return {};
    }
    if (css) {
        return {t.startsWith(u'@') ? 2 : 5, t};
    }
    if (t.startsWith(u'<')) { // HTML and XML elements
        static const QRegularExpression tag(QStringLiteral("^<([A-Za-z][\\w:.-]*)"));
        const QRegularExpressionMatch m = tag.match(t);
        return m.hasMatch() ? Named{19, m.captured(1)} : Named{};
    }
    static const QRegularExpression control(QStringLiteral(
        "^(?:#|(?:if|else|elif|elsif|for|foreach|while|do|switch|case|default|try|catch|finally|except|with|unless|until|loop|match|when|"
        "select|guard|synchronized|return|throw|yield|await|rescue|ensure|extern)\\b)"));
    static const QSet<QString> plain = {QStringLiteral("namespace"), QStringLiteral("static"), QStringLiteral("unsafe"),
                                        QStringLiteral("async"),     QStringLiteral("then"),   QStringLiteral("repeat")};
    if (control.match(raw).hasMatch() || plain.contains(t)) {
        return {};
    }
    const int firstParen = topLevel(raw, u'(');
    const int firstEquals = topLevel(raw, u'=');
    const auto declarationAt = [&](qsizetype at) {
        return (firstParen < 0 || at < firstParen) && (firstEquals < 0 || at < firstEquals);
    };

    static const QRegularExpression classLike(QStringLiteral(
        "(?:^|\\s)(class|struct|interface|enum|union|trait|impl|record|protocol|extension|object|type)\\s+(?:(?:class|struct)\\s+)?"
        "(?:[A-Z][A-Z0-9]*_[A-Z0-9_]*\\s+)?([A-Za-z_$][\\w$]*(?:(?:::|\\.)[A-Za-z_$][\\w$]*)*)"));
    if (const QRegularExpressionMatch m = classLike.match(raw); m.hasMatch() && declarationAt(m.capturedStart(1))) {
        static const QHash<QString, int> kinds = {
            {QStringLiteral("struct"), 23},    {QStringLiteral("union"), 23}, {QStringLiteral("interface"), 11}, {QStringLiteral("trait"), 11},
            {QStringLiteral("protocol"), 11}, {QStringLiteral("enum"), 10},  {QStringLiteral("type"), 26},
        };
        return {kinds.value(m.captured(1), 5), m.captured(2)};
    }
    static const QRegularExpression module(QStringLiteral("(?:^|\\s)(namespace|module|package|mod)\\s+([A-Za-z_$][\\w$]*(?:(?:::|\\.)[A-Za-z_$][\\w$]*)*)"));
    if (const QRegularExpressionMatch m = module.match(raw); m.hasMatch() && declarationAt(m.capturedStart(1))) {
        const QString word = m.captured(1);
        return {word == QLatin1String("namespace") ? 3 : word == QLatin1String("package") ? 4 : 2, m.captured(2)};
    }
    static const QRegularExpression function(QStringLiteral(
        "^(?:(?:export|default|public|private|protected|internal|static|async|pub(?:\\([\\w ]+\\))?|unsafe|const|extern|inline|override|open|"
        "suspend|final|abstract|local|virtual)\\s+)*(?:def|function\\*?|fn|func|fun|sub|proc)\\s*(?:\\([^)]*\\)\\s*)?\\*?\\s*"
        "([A-Za-z_$][\\w$]*(?:(?:::|\\.)[A-Za-z_$][\\w$]*)*)?"));
    if (const QRegularExpressionMatch m = function.match(raw); m.hasMatch() && !m.captured(1).isEmpty()) {
        return {12, m.captured(1)};
    }
    if (firstEquals > 0) {
        QString left = raw.left(firstEquals).trimmed();
        if (left.endsWith(u':')) { // Go's :=
            left.chop(1);
        }
        if (const int colon = topLevel(left, u':'); colon > 0) { // a type annotation
            left = left.left(colon);
        }
        left = left.trimmed();
        while (left.endsWith(u']')) { // int values[] = ...
            const qsizetype open = left.lastIndexOf(u'[');
            if (open < 0) {
                break;
            }
            left = left.left(open).trimmed();
        }
        static const QRegularExpression lastName(QStringLiteral("([A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)*)$"));
        const QRegularExpressionMatch m = lastName.match(left);
        if (m.hasMatch()) {
            const QString right = raw.mid(firstEquals + 1).trimmed();
            static const QRegularExpression lambda(QStringLiteral("^(?:async\\b|function\\b|lambda\\b|\\[[^\\]]*\\]\\s*(?:\\(|\\{|mutable\\b|$))"));
            const bool isFunction = right.contains(QLatin1String("=>")) || lambda.match(right).hasMatch();
            return {isFunction ? 12 : 13, m.captured(1)};
        }
        return {};
    }
    static const QRegularExpression key(QStringLiteral(
        "^(-\\s+)?(?:\"((?:[^\"\\\\]|\\\\.)*)\"|'((?:[^'\\\\]|\\\\.)*)'|([A-Za-z_$@][\\w$.\\-@/]*))\\s*:(?!:)\\s*(.*)$"));
    if (const QRegularExpressionMatch m = key.match(raw); m.hasMatch()) {
        const QString name = m.captured(2) + m.captured(3) + m.captured(4);
        const QString value = simplified(m.captured(5));
        const bool isFunction = value.contains(QLatin1String("=>")) || value.startsWith(QLatin1String("function")) || value.startsWith(QLatin1String("async"));
        // YAML list items "- name: Build" are named by their value
        if (!m.captured(1).isEmpty() && !value.isEmpty() && !QStringView(u"|>&*{[").contains(value.front())) {
            return {7, unquoted(value)};
        }
        return {isFunction ? 12 : 7, name};
    }
    if (!callee.isEmpty()) {
        return {12, callee + QStringLiteral("()")};
    }
    if (firstParen > 0) { // a function definition: name(parameters)
        QString prefix = raw.left(firstParen).trimmed();
        static const QRegularExpression op(QStringLiteral("((?:[A-Za-z_$][\\w$]*::)*operator\\s*\\S*)$"));
        if (const QRegularExpressionMatch m = op.match(prefix); m.hasMatch()) {
            return {6, m.captured(1)};
        }
        if (prefix.endsWith(u'>')) { // name<T>(
            int depth = 0;
            for (qsizetype i = prefix.size() - 1; i >= 0; --i) {
                if (prefix.at(i) == u'>') {
                    ++depth;
                } else if (prefix.at(i) == u'<' && --depth == 0) {
                    prefix = prefix.left(i).trimmed();
                    break;
                }
            }
        }
        static const QRegularExpression name(QStringLiteral("(~?[A-Za-z_$][\\w$]*(?:(?:::|\\.)~?[A-Za-z_$][\\w$]*)*)$"));
        const QRegularExpressionMatch m = name.match(prefix);
        if (m.hasMatch() && !isKeyword(m.captured(1))) {
            const QString found = m.captured(1);
            return {found.contains(QLatin1String("::")) || found.contains(u'.') ? 6 : 12, found};
        }
        return {};
    }
    static const QRegularExpression single(QStringLiteral("^[A-Za-z_$][\\w$.\\-]*$")); // QML objects: Item {
    if (single.match(t).hasMatch()) {
        return {19, t};
    }
    return {};
}

QString codeOf(const QStringList &lines, const std::vector<LineInfo> &info, int line)
{
    const QString &s = lines.at(line);
    return (info[line].commentAt >= 0 ? s.left(info[line].commentAt) : s).trimmed();
}

int previousCode(const std::vector<LineInfo> &info, int line)
{
    for (int k = line - 1; k >= 0; --k) {
        if (info[k].code) {
            return k;
        }
    }
    return -1;
}

int firstNonSpace(const QString &s)
{
    for (int i = 0; i < s.size(); ++i) {
        if (!s.at(i).isSpace()) {
            return i;
        }
    }
    return 0;
}

Scope makeScope(int start, int end, const Named &named, const QStringList &lines)
{
    Scope s;
    s.startLine = start;
    s.startColumn = 0;
    s.endLine = end;
    s.endColumn = INT_MAX;
    s.targetLine = start;
    s.targetColumn = firstNonSpace(lines.at(start));
    s.name = named.name.size() > 80 ? named.name.left(79) + QChar(0x2026) : named.name;
    s.kind = named.name.isEmpty() ? 0 : named.kind;
    return s;
}

QString calleeAt(const QStringList &lines, int line, int column)
{
    if (line < 0 || line >= lines.size()) {
        return {};
    }
    const QString &s = lines.at(line);
    int end = column;
    while (end > 0 && s.at(end - 1).isSpace()) {
        --end;
    }
    int start = end;
    while (start > 0 && (isIdentChar(s.at(start - 1)) || s.at(start - 1) == u'.' || s.at(start - 1) == u':')) {
        --start;
    }
    QString callee = s.mid(start, end - start);
    while (!callee.isEmpty() && (callee.front() == u'.' || callee.front() == u':')) {
        callee.remove(0, 1);
    }
    return isKeyword(callee) ? QString() : callee;
}

void braceScopes(const QStringList &lines, const std::vector<LineInfo> &info, const std::vector<Block> &blocks, const ScopeOptions &o, ScopeTree &tree)
{
    QHash<int, int> byStart; // header line -> scope
    for (const Block &b : blocks) {
        const Open &open = b.open;
        int header = open.line;
        if (lines.at(open.line).left(open.column).trimmed().isEmpty()) {
            // The brace stands alone: the block belongs to the line before
            // (Allman style), unless that one ends a statement.
            const int previous = previousCode(info, open.line);
            if (previous >= 0) {
                const QString t = codeOf(lines, info, previous);
                if (!t.isEmpty() && !QStringView(u";{}[(,").contains(t.back()) && !t.startsWith(u'#')) {
                    header = previous;
                }
            }
        }
        // Back to the first line of the declaration: parameters on several
        // lines, constructor initializer lists.
        for (int guard = 0; header > 0 && guard < 64; ++guard) {
            const QString t = codeOf(lines, info, header);
            const bool continuation = (t.startsWith(u':') && !t.startsWith(QLatin1String("::"))) || t.startsWith(u',');
            if (!continuation && info[header].parens <= open.parens) {
                break;
            }
            const int previous = previousCode(info, header);
            if (previous < 0) {
                break;
            }
            header = previous;
        }
        QString code = codeOf(lines, info, header);
        if (o.css) { // selector lists on several lines: ".a,\n.b {"
            for (int previous = previousCode(info, header); previous >= 0 && codeOf(lines, info, previous).endsWith(u','); previous = previousCode(info, header)) {
                header = previous;
            }
            QStringList selectors;
            for (int line = header; line <= open.line; ++line) {
                const QString part = line == open.line ? lines.at(line).left(open.column) : codeOf(lines, info, line);
                selectors << part.trimmed();
            }
            code = selectors.join(QLatin1Char(' '));
        }
        if (b.closeLine <= header) {
            continue;
        }
        const QString callee = open.inCall && open.ch == u'{' ? calleeAt(lines, open.calleeLine, open.calleeColumn) : QString();
        const Named named = classify(code, o.css, callee);
        // One scope per header line: the outermost block.
        if (const auto it = byStart.constFind(header); it != byStart.cend()) {
            Scope &existing = tree.scopes[it.value()];
            if (b.closeLine > existing.endLine) {
                existing = makeScope(header, b.closeLine, named, lines);
            }
            continue;
        }
        byStart.insert(header, int(tree.scopes.size()));
        tree.scopes.push_back(makeScope(header, b.closeLine, named, lines));
    }
}

void indentationScopes(const QStringList &lines, const std::vector<LineInfo> &info, ScopeTree &tree)
{
    struct Entry {
        int indent;
        int line;
    };
    std::vector<Entry> stack;
    int last = -1; // the last line with code so far
    const auto close = [&](const Entry &e) {
        if (last > e.line) {
            tree.scopes.push_back(makeScope(e.line, last, classify(codeOf(lines, info, e.line), false, QString()), lines));
        }
    };
    for (int i = 0; i < int(lines.size()); ++i) {
        if (!info[i].code) {
            continue;
        }
        const int indent = info[i].indent;
        while (!stack.empty() && stack.back().indent >= indent) {
            close(stack.back());
            stack.pop_back();
        }
        stack.push_back({indent, i});
        last = i;
    }
    while (!stack.empty()) {
        close(stack.back());
        stack.pop_back();
    }
}

void markdownScopes(const QStringList &lines, ScopeTree &tree)
{
    struct Heading {
        int line;
        int level;
        QString text;
    };
    static const QRegularExpression heading(QStringLiteral("^ {0,3}(#{1,6})[ \\t]+(.*?)(?:[ \\t]+#+)?[ \\t]*$"));
    QList<Heading> headings;
    QString fence;
    for (int i = 0; i < int(lines.size()); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
            const QString marker = trimmed.left(3);
            if (fence.isEmpty()) {
                fence = marker;
            } else if (marker == fence) {
                fence.clear();
            }
            continue;
        }
        if (!fence.isEmpty()) {
            continue;
        }
        if (const QRegularExpressionMatch m = heading.match(lines.at(i)); m.hasMatch()) {
            headings.append({i, int(m.capturedLength(1)), m.captured(2).trimmed()});
        }
    }
    for (qsizetype k = 0; k < headings.size(); ++k) {
        const Heading &h = headings.at(k);
        int end = int(lines.size()) - 1;
        for (qsizetype next = k + 1; next < headings.size(); ++next) {
            if (headings.at(next).level <= h.level) {
                end = headings.at(next).line - 1;
                break;
            }
        }
        while (end > h.line && lines.at(end).trimmed().isEmpty()) {
            --end;
        }
        if (end > h.line) {
            Scope s = makeScope(h.line, end, Named{15, h.text}, lines);
            s.targetColumn = 0;
            tree.scopes.push_back(s);
        }
    }
}
}

// ---------------------------------------------------------------------------

void ScopeTree::build()
{
    std::stable_sort(scopes.begin(), scopes.end(), [](const Scope &a, const Scope &b) {
        if (a.startLine != b.startLine) {
            return a.startLine < b.startLine;
        }
        if (a.startColumn != b.startColumn) {
            return a.startColumn < b.startColumn;
        }
        if (a.endLine != b.endLine) {
            return a.endLine > b.endLine; // the outer one first
        }
        return a.endColumn > b.endColumn;
    });
    roots.clear();
    std::vector<int> stack;
    for (int i = 0; i < int(scopes.size()); ++i) {
        scopes[i].children.clear();
        while (!stack.empty() && !contains(scopes[stack.back()], scopes[i])) {
            stack.pop_back();
        }
        scopes[i].parent = stack.empty() ? -1 : stack.back();
        if (scopes[i].parent < 0) {
            roots.append(i);
        } else {
            scopes[scopes[i].parent].children.append(i);
        }
        stack.push_back(i);
    }
}

QList<int> ScopeTree::chainAt(int line, int column) const
{
    QList<int> chain;
    const QList<int> *level = &roots;
    while (!level->isEmpty()) {
        // The last scope starting at or before the position (siblings may
        // share a line: "} else {").
        const auto after = std::upper_bound(level->begin(), level->end(), qMakePair(line, column), [this](const QPair<int, int> &position, int index) {
            return before(position.first, position.second, scopes[index].startLine, scopes[index].startColumn);
        });
        int found = -1;
        for (auto it = after; it != level->begin() && after - it < 4;) {
            --it;
            if (containsPosition(scopes[*it], line, column)) {
                found = *it;
                break;
            }
        }
        if (found < 0) {
            break;
        }
        chain.append(found);
        level = &scopes[found].children;
    }
    return chain;
}

bool ScopeTree::isSymbol(int index) const
{
    return index >= 0 && index < int(scopes.size()) && !scopes[index].name.isEmpty();
}

QList<int> ScopeTree::symbolsUnder(int parent) const
{
    QList<int> symbols;
    const QList<int> &level = parent < 0 ? roots : scopes[parent].children;
    for (const int i : level) {
        if (isSymbol(i)) {
            symbols.append(i);
        } else {
            symbols.append(symbolsUnder(i));
        }
    }
    return symbols;
}

QList<int> ScopeTree::sticky(const std::function<int(int)> &lineAtRow, int maxLines) const
{
    QList<int> lines;
    const QList<int> *level = &roots;
    for (int row = 0; row < maxLines && !level->isEmpty(); ++row) {
        // Row `row` is the first one not covered by the sticky lines so far:
        // the next scope sticks while its first line is above it and its
        // last line is on it or below.
        const int line = lineAtRow(row);
        if (line < 0) {
            break;
        }
        const auto after = std::lower_bound(level->begin(), level->end(), line, [this](int index, int l) {
            return scopes[index].startLine < l;
        });
        int found = -1;
        for (auto it = after; it != level->begin() && after - it < 4;) {
            --it;
            const Scope &s = scopes[*it];
            if (s.startLine < line && line <= s.endLine) {
                found = *it;
                break;
            }
        }
        if (found < 0) {
            break;
        }
        lines.append(found);
        level = &scopes[found].children;
    }
    return lines;
}

ScopeOptions::Style scopeStyleFor(const QString &highlightingMode, const QString &fileName)
{
    static const QSet<QString> indentation = {
        QStringLiteral("python"),   QStringLiteral("yaml"),  QStringLiteral("coffeescript"), QStringLiteral("nim"),     QStringLiteral("haskell"),
        QStringLiteral("literate haskell"), QStringLiteral("f#"), QStringLiteral("elm"),   QStringLiteral("pug"),     QStringLiteral("jade"),
        QStringLiteral("sass"),     QStringLiteral("stylus"), QStringLiteral("restructuredtext"), QStringLiteral("makefile"), QStringLiteral("bash"),
        QStringLiteral("zsh"),      QStringLiteral("fish"),  QStringLiteral("ruby"),        QStringLiteral("lua"),     QStringLiteral("elixir"),
        QStringLiteral("julia"),    QStringLiteral("cmake"), QStringLiteral("xml"),         QStringLiteral("html"),    QStringLiteral("crystal"),
        QStringLiteral("fortran"),  QStringLiteral("pascal"), QStringLiteral("ada"),        QStringLiteral("matlab"),  QStringLiteral("octave"),
        QStringLiteral("toml"),     QStringLiteral("ini files"), QStringLiteral("dockerfile"), QStringLiteral("gdscript"), QStringLiteral("meson"),
        QStringLiteral("r script"), QStringLiteral("sql"),   QStringLiteral("vue"),
    };
    static const QSet<QString> markdown = {QStringLiteral("markdown"), QStringLiteral("multimarkdown"), QStringLiteral("r markdown")};
    const QString mode = highlightingMode.trimmed().toLower();
    if (markdown.contains(mode)) {
        return ScopeOptions::Style::Markdown;
    }
    if (indentation.contains(mode)) {
        return ScopeOptions::Style::Indentation;
    }
    if (mode.isEmpty() || mode == QLatin1String("normal") || mode == QLatin1String("none")) {
        const QString suffix = QFileInfo(fileName).suffix().toLower();
        if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
            return ScopeOptions::Style::Markdown;
        }
        if (suffix == QLatin1String("py") || suffix == QLatin1String("yml") || suffix == QLatin1String("yaml")) {
            return ScopeOptions::Style::Indentation;
        }
    }
    return ScopeOptions::Style::Auto;
}

ScopeTree heuristicScopes(const QStringList &lines, const ScopeOptions &options)
{
    ScopeTree tree;
    if (options.style == ScopeOptions::Style::Markdown) {
        markdownScopes(lines, tree);
        tree.build();
        return tree;
    }
    Lexer lexer(options);
    std::vector<LineInfo> info(lines.size());
    std::vector<Block> blocks;
    for (int i = 0; i < int(lines.size()); ++i) {
        info[i] = lexer.read(lines.at(i), i, blocks);
    }
    if (options.style == ScopeOptions::Style::Auto && !blocks.empty()) {
        braceScopes(lines, info, blocks, options, tree);
    } else {
        indentationScopes(lines, info, tree);
    }
    tree.build();
    return tree;
}
}
