// DocumentBlame follows edits in memory: after each edit its answer must match
// a fresh `git blame` of the edited text.
#include "blame.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

#include <QApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace
{
QString g_root;

bool git(const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(g_root);
    p.start(QStringLiteral("git"), QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Test"), QStringLiteral("-c"),
                                               QStringLiteral("user.email=test@example.com")} + args);
    return p.waitForFinished(10000) && p.exitCode() == 0;
}

void writeFile(const QByteArray &text)
{
    QFile f(g_root + QStringLiteral("/f.txt"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text);
    }
}

// "U" for uncommitted lines, else the short commit hash, per line.
QStringList truth(KTextEditor::Document *doc)
{
    QProcess p;
    p.setWorkingDirectory(g_root);
    p.start(QStringLiteral("git"), {QStringLiteral("blame"), QStringLiteral("--porcelain"), QStringLiteral("--contents"), QStringLiteral("-"),
                                    QStringLiteral("--"), QStringLiteral("f.txt")});
    p.write(doc->text().toUtf8());
    p.closeWriteChannel();
    p.waitForFinished(10000);
    QStringList out;
    const QList<QByteArray> lines = p.readAllStandardOutput().split('\n');
    for (const QByteArray &l : lines) {
        const QList<QByteArray> parts = l.split(' ');
        if ((parts.size() == 3 || parts.size() == 4) && parts.at(0).size() == 40) {
            out << (parts.at(0).count('0') == 40 ? QStringLiteral("U") : QString::fromLatin1(parts.at(0).left(7)));
        }
    }
    return out;
}

QStringList ours(DocumentBlame *blame, KTextEditor::Document *doc)
{
    QStringList out;
    const int lines = doc->lines() - (doc->line(doc->lines() - 1).isEmpty() ? 1 : 0); // git has no line after the last newline
    for (int i = 0; i < lines; ++i) {
        const int e = blame->entry(i);
        out << (e == DocumentBlame::Uncommitted ? QStringLiteral("U") : e == DocumentBlame::Unknown ? QStringLiteral("?") : blame->commit(e).hash.left(7));
    }
    return out;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir dir;
    g_root = dir.path();
    writeFile("alpha\nbeta\ngamma\n\ndelta\nepsilon\nzeta\neta\ntheta\niota\n");
    if (!git({QStringLiteral("init"), QStringLiteral("-q")}) || !git({QStringLiteral("add"), QStringLiteral("f.txt")})
        || !git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")})) {
        std::puts("cannot create the test repository");
        return 1;
    }
    writeFile("alpha\nbeta2\ngamma\n\ndelta\nepsilon\nzeta\neta\ntheta\niota\nkappa\n");
    git({QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("two")});

    auto *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    doc->openUrl(QUrl::fromLocalFile(g_root + QStringLiteral("/f.txt")));
    auto *blame = new DocumentBlame(doc, g_root, doc);
    blame->refresh();
    wait(2000);

    int failures = 0;
    const auto check = [&](const char *what) {
        const QStringList a = ours(blame, doc);
        const QStringList t = truth(doc);
        const bool ok = a == t;
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) {
            std::printf("     ours %s\n     git  %s\n", qPrintable(a.join(QLatin1Char(','))), qPrintable(t.join(QLatin1Char(','))));
        }
    };
    check("after blame");
    doc->insertText(KTextEditor::Cursor(2, 0), QStringLiteral("new line\n"));
    check("line inserted above another");
    doc->removeLine(5);
    check("line removed");
    doc->insertText(KTextEditor::Cursor(0, 5), QStringLiteral("!"));
    check("line edited");
    doc->removeText(KTextEditor::Range(0, 5, 0, 6));
    check("edit undone by hand");
    doc->insertText(KTextEditor::Cursor(6, doc->lineLength(6)), QStringLiteral("\n"));
    check("Enter at the end of a line");
    doc->insertText(KTextEditor::Cursor(8, 0), QStringLiteral("\n"));
    check("Enter at the start of a line");
    doc->insertText(KTextEditor::Cursor(9, 2), QStringLiteral("\n"));
    check("line split");
    doc->removeText(KTextEditor::Range(9, doc->lineLength(9), 10, 0));
    check("split lines joined back");
    doc->insertText(KTextEditor::Cursor(3, 0), QStringLiteral("p1\np2\np3\n"));
    check("three lines pasted");
    doc->removeText(KTextEditor::Range(3, 0, 6, 0));
    check("pasted lines deleted");
    blame->refresh();
    doc->insertText(KTextEditor::Cursor(1, 0), QStringLiteral("during\n"));
    doc->removeLine(4);
    wait(2000);
    check("edits while git runs");

    delete doc;
    std::printf("%s\n", failures ? "blame: FAILED" : "blame: all passed");
    return failures ? 1 : 0;
}
