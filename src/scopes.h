#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace Lieve
{
// A part of a document that encloses other lines: a class, a function, an
// `if` block... Read from the language server (document symbols) or found
// from the braces and the indentation. Lines and columns are 0-based.
struct Scope {
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0; // inclusive
    int endColumn = 0;
    int targetLine = 0; // where choosing it jumps to (its name)
    int targetColumn = 0;
    QString name; // empty: a plain block (`if`, `for`...), used by sticky scroll only
    int kind = 0; // LSP SymbolKind; 0 for plain blocks
    int parent = -1;
    QList<int> children; // in document order
};

// The scopes of a document, nested by their ranges, in document order.
struct ScopeTree {
    std::vector<Scope> scopes;
    QList<int> roots;
    bool fromServer = false;

    // Sorts the scopes and nests them by their ranges (after filling `scopes`).
    void build();
    // The scopes containing the position, outermost first.
    QList<int> chainAt(int line, int column) const;
    // A named symbol (shown in the breadcrumbs), not a plain block.
    bool isSymbol(int index) const;
    // The symbols directly under `parent` (-1: the top level), looking
    // through the plain blocks in between.
    QList<int> symbolsUnder(int parent) const;
    // Sticky scroll: the scopes whose first line is scrolled out of view
    // while the view shows some of their lines, outermost first. Row `n`
    // of the view shows document line `lineAtRow(n)` (-1 for none); the
    // first `n` rows are covered by the n sticky lines above them.
    QList<int> sticky(const std::function<int(int)> &lineAtRow, int maxLines) const;
};

struct ScopeOptions {
    enum class Style {
        Auto, // blocks in braces, or the indentation when the text has none
        Indentation, // Python, YAML...
        Markdown, // headings
    };
    Style style = Style::Auto;
    int tabWidth = 4;
    QString lineComment; // "//", "#"... (empty: none)
    QString blockCommentStart; // "/*"
    QString blockCommentEnd; // "*/"
    bool css = false; // blocks are named by their selectors
};

// The style for a highlighting mode ("Python", "C++"...) or, without one, a file name.
ScopeOptions::Style scopeStyleFor(const QString &highlightingMode, const QString &fileName);

// Scopes found without a language server: blocks in braces or brackets that
// span lines (the header is the line with the opening brace, or the line
// before it when the brace stands alone), or the lines followed by more
// indented ones; comment-only and blank lines do not count.
ScopeTree heuristicScopes(const QStringList &lines, const ScopeOptions &options);
}
