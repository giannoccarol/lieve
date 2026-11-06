// The scopes found without a language server (braces, indentation, Markdown
// headings), as used by the breadcrumbs and sticky scroll.
#include "scopes.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using Lieve::ScopeOptions;
using Lieve::ScopeTree;

namespace
{
int failures = 0;

void expect(bool ok, const char *what)
{
    failures += ok ? 0 : 1;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

ScopeOptions cLike()
{
    ScopeOptions o;
    o.lineComment = QStringLiteral("//");
    o.blockCommentStart = QStringLiteral("/*");
    o.blockCommentEnd = QStringLiteral("*/");
    return o;
}

// "start-end name" for each scope, 1-based lines, in document order.
QString dump(const ScopeTree &tree)
{
    QStringList out;
    for (const Lieve::Scope &s : tree.scopes) {
        out << QStringLiteral("%1-%2 %3").arg(s.startLine + 1).arg(s.endLine + 1).arg(s.name.isEmpty() ? QStringLiteral("-") : s.name);
    }
    return out.join(QStringLiteral("; "));
}

QString names(const ScopeTree &tree, const QList<int> &indexes)
{
    QStringList out;
    for (const int i : indexes) {
        out << (tree.scopes[i].name.isEmpty() ? QStringLiteral("-") : tree.scopes[i].name);
    }
    return out.join(QStringLiteral(" > "));
}

void check(const QString &got, const QString &wanted, const char *what)
{
    expect(got == wanted, what);
    if (got != wanted) {
        std::printf("     got:    %s\n     wanted: %s\n", qPrintable(got), qPrintable(wanted));
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // C++ in KDE style: braces of functions on their own line.
    const QStringList cpp = QString::fromUtf8(R"(namespace Lieve
{
class Foo : public QObject
{
public:
    void run(int a,
             int b) const;
};

Foo::Foo(QObject *parent)
    : QObject(parent)
    , m_x(1)
{
    connect(this, &Foo::done, this, [this] {
        if (m_x) { // a { in a comment
            work("}");
        }
    });
}

void Foo::run(int a,
              int b) const
{
    const auto text = QStringLiteral(R"x(
    { not a block
    )x");
    for (int i = 0; i < a; ++i) {
        /* } */
        work(i);
    }
}
}
)").split(QLatin1Char('\n'));
    const ScopeTree t = Lieve::heuristicScopes(cpp, cLike());
    check(dump(t),
          QStringLiteral("1-32 Lieve; 3-8 Foo; 10-19 Foo::Foo; 14-18 connect(); 15-17 -; 21-31 Foo::run; 27-30 -"),
          "C++ blocks, headers and names");
    check(names(t, t.chainAt(15, 4)), QStringLiteral("Lieve > Foo::Foo > connect() > -"), "chain at a line");
    check(names(t, t.symbolsUnder(-1)), QStringLiteral("Lieve"), "top-level symbols");
    check(names(t, t.symbolsUnder(0)), QStringLiteral("Foo > Foo::Foo > Foo::run"), "symbols under a namespace");
    check(names(t, t.symbolsUnder(2)), QStringLiteral("connect()"), "symbols under a function");

    // Sticky lines: the view's first row shows line 16 (0-based 15).
    const auto rows = [](int top) {
        return [top](int row) {
            return top + row;
        };
    };
    check(names(t, t.sticky(rows(15), 5)), QStringLiteral("Lieve > Foo::Foo > connect()"), "sticky headers, nested");
    check(names(t, t.sticky(rows(11), 5)), QStringLiteral("Lieve > Foo::Foo"), "a header shown on its own row does not stick");
    check(names(t, t.sticky(rows(15), 2)), QStringLiteral("Lieve > Foo::Foo"), "at most maxLines");
    check(names(t, t.sticky(rows(0), 5)), QString(), "nothing sticks at the top");
    check(names(t, t.sticky(rows(29), 5)), QStringLiteral("Lieve > Foo::run"), "until the last line of the block");
    check(names(t, t.sticky(rows(30), 5)), QStringLiteral("Lieve"), "then it goes");

    // K&R style, else chains and arrays.
    const QStringList js = QString::fromUtf8(R"(export class Shop extends Base {
  private items: Item[] = [
    1,
  ];
  async load(id: number): Promise<void> {
    if (id) {
      this.items.forEach((item) => {
        print(`${item} {`);
      });
    } else {
      done();
    }
  }
}
const handler = async (e) => {
  return e;
};
describe('shop', () => {
  it('works', () => {
    expect(1);
  });
});
)").split(QLatin1Char('\n'));
    const ScopeTree j = Lieve::heuristicScopes(js, cLike());
    check(dump(j),
          QStringLiteral("1-14 Shop; 2-4 items; 5-13 load; 6-10 -; 7-9 this.items.forEach(); 10-12 -; 15-17 handler; 18-22 describe(); 19-21 it()"),
          "TypeScript classes, methods, arrays and callbacks");
    check(names(j, j.chainAt(10, 6)), QStringLiteral("Shop > load > -"), "inside else: the else block");

    // Indentation: Python with comments and docstrings, YAML.
    ScopeOptions py;
    py.style = ScopeOptions::Style::Indentation;
    py.lineComment = QStringLiteral("#");
    const QStringList python = QString::fromUtf8(R"(class Greeter:
    """Says hello.
Not indented.
    """

    def hello(self, name):
# a comment at the start of the line
        if name:
            print(name)
        return 1

def main():
    pass
)").split(QLatin1Char('\n'));
    const ScopeTree p = Lieve::heuristicScopes(python, py);
    check(dump(p), QStringLiteral("1-10 Greeter; 6-10 hello; 8-9 -; 12-13 main"), "Python by indentation");

    const QStringList yaml = QString::fromUtf8(R"(jobs:
  build:
    steps:
      - name: Checkout
        uses: actions/checkout@v4
      - run: make
)").split(QLatin1Char('\n'));
    const ScopeTree y = Lieve::heuristicScopes(yaml, py);
    check(dump(y), QStringLiteral("1-6 jobs; 2-6 build; 3-6 steps; 4-5 Checkout"), "YAML keys and list items");

    // JSON: braces, keys as names, array items without one.
    ScopeOptions json;
    const QStringList jsonText = QString::fromUtf8(R"({
  "compilerOptions": {
    "strict": true,
    "paths": [
      "a"
    ]
  },
  "files": [
    {
      "x": "{"
    }
  ]
})").split(QLatin1Char('\n'));
    check(dump(Lieve::heuristicScopes(jsonText, json)),
          QStringLiteral("1-13 -; 2-7 compilerOptions; 4-6 paths; 8-12 files; 9-11 -"),
          "JSON");

    // Plain text without braces falls back to the indentation.
    const QStringList text = QString::fromUtf8("Title\n  detail one\n  detail two\nOther\n").split(QLatin1Char('\n'));
    check(dump(Lieve::heuristicScopes(text, ScopeOptions())), QStringLiteral("1-3 Title"), "indentation when there are no braces");

    // Markdown headings, not those in code blocks.
    ScopeOptions md;
    md.style = ScopeOptions::Style::Markdown;
    const QStringList markdown = QString::fromUtf8("# Title\nintro\n## Install\nsteps\n```\n# not a heading\n```\n\n## Use\ntext\n# Next\nmore\n").split(QLatin1Char('\n'));
    check(dump(Lieve::heuristicScopes(markdown, md)), QStringLiteral("1-10 Title; 3-7 Install; 9-10 Use; 11-12 Next"), "Markdown headings");

    // CSS: selectors name the blocks.
    ScopeOptions css = cLike();
    css.lineComment.clear();
    css.css = true;
    const QStringList style = QString::fromUtf8(".a .b:hover,\n.c {\n  color: red;\n}\n@media (max-width: 600px) {\n  .d {\n    margin: 0;\n  }\n}\n").split(QLatin1Char('\n'));
    check(dump(Lieve::heuristicScopes(style, css)), QStringLiteral("1-4 .a .b:hover, .c; 5-9 @media (max-width: 600px); 6-8 .d"), "CSS selectors");

    check(QString::number(int(Lieve::scopeStyleFor(QStringLiteral("Python"), QString()))), QString::number(int(ScopeOptions::Style::Indentation)), "style for Python");
    check(QString::number(int(Lieve::scopeStyleFor(QStringLiteral("Normal"), QStringLiteral("README.md")))),
          QString::number(int(ScopeOptions::Style::Markdown)),
          "style for a .md file without a mode");

    std::printf("%s\n", failures ? "scopes: FAILED" : "scopes: all passed");
    return failures ? 1 : 0;
}
