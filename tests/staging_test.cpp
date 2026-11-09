// Staging, unstaging and reverting part of a file on scratch repositories,
// the way GitRepo::rewriteIndex() and GitStaging do it: the index entry from
// ls-files, the new content from the hunk maths, then hash-object and
// update-index (GitParse's commands), checked against what git then reports.
#include "git.h"
#include "linediff.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
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

QByteArray git(const QString &dir, const QStringList &args, int *exitCode = nullptr, const QByteArray &input = QByteArray())
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"),
            QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Test Author"), QStringLiteral("-c"), QStringLiteral("user.email=test@example.com"),
                        QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false"), QStringLiteral("-c"), QStringLiteral("core.quotepath=false")}
                + args);
    p.write(input);
    p.closeWriteChannel();
    p.waitForFinished(10000);
    if (exitCode) {
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    }
    return p.readAllStandardOutput();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
    }
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// What a KTextEditor document holds for a file: lines split on "\n" and
// "\r\n", and whether its line breaks are CRLF.
QStringList documentLines(const QByteArray &data, bool *crlf = nullptr)
{
    if (crlf) {
        *crlf = data.contains("\r\n");
    }
    QString text = QString::fromUtf8(data);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return text.split(QLatin1Char('\n'));
}

struct Entry {
    bool inIndex = false;
    bool conflict = false;
    QString mode;
    QString blob;
};

Entry indexEntry(const QString &repo, const QString &path)
{
    Entry e;
    e.inIndex = GitParse::indexEntry(git(repo, GitParse::indexEntryArgs(path)), &e.mode, &e.blob, &e.conflict);
    return e;
}

// GitRepo::rewriteIndex()'s write step.
bool writeIndex(const QString &repo, const QString &path, const QString &mode, const QByteArray &data)
{
    int code = 0;
    const QString blob = QString::fromLatin1(git(repo, GitParse::hashObjectArgs(path), &code, data)).trimmed();
    if (code != 0 || blob.isEmpty()) {
        return false;
    }
    git(repo, GitParse::updateIndexArgs(mode, blob, path), &code);
    return code == 0;
}

// GitStaging::stage(): the document is the file on disk.
bool stage(const QString &repo, const QString &path, const LineSelection &selection)
{
    const Entry e = indexEntry(repo, path);
    if (e.conflict) {
        return false;
    }
    const QByteArray index = e.inIndex ? git(repo, {QStringLiteral("cat-file"), QStringLiteral("blob"), e.blob}) : QByteArray();
    bool crlf = false;
    const QStringList lines = documentLines(readFile(QDir(repo).filePath(path)), &crlf);
    const std::optional<QByteArray> content = stagedContent(index, lines, "UTF-8", crlf, selection);
    return content && writeIndex(repo, path, e.inIndex ? e.mode : QStringLiteral("100644"), *content);
}

// GitStaging::unstage()
bool unstage(const QString &repo, const QString &path, const LineSelection &selection)
{
    const Entry e = indexEntry(repo, path);
    int code = 0;
    const QByteArray head = git(repo, {QStringLiteral("cat-file"), QStringLiteral("blob"), QStringLiteral("HEAD:") + path}, &code);
    const bool inHead = code == 0;
    const QByteArray index = git(repo, {QStringLiteral("cat-file"), QStringLiteral("blob"), e.blob});
    const std::optional<QByteArray> content = unstagedContent(inHead ? head : QByteArray(), index, selection);
    if (!content) {
        return false;
    }
    if (!inHead && content->isEmpty()) {
        git(repo, GitParse::removeFromIndexArgs(path), &code);
        return code == 0;
    }
    return writeIndex(repo, path, e.mode, *content);
}

QByteArray indexText(const QString &repo, const QString &path)
{
    return git(repo, {QStringLiteral("show"), QLatin1Char(':') + path});
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

QString newRepo(const QTemporaryDir &tmp, const QString &name)
{
    const QString repo = tmp.filePath(name);
    QDir().mkpath(repo);
    git(repo, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")});
    git(repo, {QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("false")}); // whatever the user's own setting
    return repo;
}

void commitAll(const QString &repo, const QString &message)
{
    git(repo, {QStringLiteral("add"), QStringLiteral("-A")});
    git(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), message});
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    expect(tmp.isValid(), "scratch directory");

    // A committed file with three changes in the working tree
    const QString repo = newRepo(tmp, QStringLiteral("repo"));
    writeFile(repo + QStringLiteral("/f.txt"), "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    commitAll(repo, QStringLiteral("First"));
    writeFile(repo + QStringLiteral("/f.txt"), "1\ntwo\n3\n4\n5\n5a\n6\n7\n9\n10\n");

    Entry e = indexEntry(repo, QStringLiteral("f.txt"));
    expect(e.inIndex && !e.conflict && e.mode == QLatin1String("100644") && e.blob.size() == 40, "ls-files: mode and blob of the index entry");
    expect(!indexEntry(repo, QStringLiteral("missing.txt")).inIndex, "ls-files: a file not in the index");

    expect(stage(repo, QStringLiteral("f.txt"), lines(1, 1, true)), "stage the change at a line");
    expect(indexText(repo, QStringLiteral("f.txt")) == "1\ntwo\n3\n4\n5\n6\n7\n8\n9\n10\n", "the index has that change only");
    expect(git(repo, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--numstat")}).startsWith("1\t1\tf.txt"), "git diff --cached: 1 line changed");
    expect(git(repo, {QStringLiteral("diff"), QStringLiteral("--numstat")}).startsWith("1\t1\tf.txt"), "git diff: the other two changes left");

    expect(stage(repo, QStringLiteral("f.txt"), lines(7, 7, true)), "stage the deletion, from the line above it");
    expect(indexText(repo, QStringLiteral("f.txt")) == "1\ntwo\n3\n4\n5\n6\n7\n9\n10\n", "the deletion is staged");

    expect(unstage(repo, QStringLiteral("f.txt"), lines(1, 1, true)), "unstage the first change");
    expect(indexText(repo, QStringLiteral("f.txt")) == "1\n2\n3\n4\n5\n6\n7\n9\n10\n", "the index without it");
    expect(unstage(repo, QStringLiteral("f.txt"), lines(7, 7, true, DiffSide::Original)), "unstage the deletion from the HEAD side");
    int code = 0;
    git(repo, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet")}, &code);
    expect(code == 0, "nothing staged any more");
    expect(readFile(repo + QStringLiteral("/f.txt")) == "1\ntwo\n3\n4\n5\n5a\n6\n7\n9\n10\n", "the working file is untouched");

    // Revert: the document's text with a change taken back to the index
    {
        const std::optional<QString> text =
            revertedText(indexText(repo, QStringLiteral("f.txt")), documentLines(readFile(repo + QStringLiteral("/f.txt"))), "UTF-8", lines(5, 5, true));
        expect(text && *text == QLatin1String("1\ntwo\n3\n4\n5\n6\n7\n9\n10\n"), "revert the added line");
        expect(!revertedText(indexText(repo, QStringLiteral("f.txt")), documentLines(readFile(repo + QStringLiteral("/f.txt"))), "UTF-8", lines(2, 3, true)),
               "revert on unchanged lines: nothing");
    }

    // An executable file keeps its mode
    writeFile(repo + QStringLiteral("/run.sh"), "#!/bin/sh\necho a\n");
    QFile::setPermissions(repo + QStringLiteral("/run.sh"), QFile::permissions(repo + QStringLiteral("/run.sh")) | QFile::ExeOwner);
    commitAll(repo, QStringLiteral("Script"));
    writeFile(repo + QStringLiteral("/run.sh"), "#!/bin/sh\necho b\necho c\n");
#ifdef Q_OS_WIN // no executable bit on Windows (git's core.filemode is false): staging still works
    expect(stage(repo, QStringLiteral("run.sh"), lines(2, 2)), "stage part of an executable");
#else
    expect(indexEntry(repo, QStringLiteral("run.sh")).mode == QLatin1String("100755"), "an executable's mode");
    expect(stage(repo, QStringLiteral("run.sh"), lines(2, 2)), "stage part of an executable");
    expect(indexEntry(repo, QStringLiteral("run.sh")).mode == QLatin1String("100755"), "it is still executable in the index");
#endif
    expect(indexText(repo, QStringLiteral("run.sh")) == "#!/bin/sh\necho c\n", "the selected line replaced the old one (different sizes)");

    // A new file: part of it staged, then unstaged again
    writeFile(repo + QStringLiteral("/new.txt"), "x\ny\n");
    expect(stage(repo, QStringLiteral("new.txt"), lines(0, 0)), "stage the first line of an untracked file");
    expect(indexText(repo, QStringLiteral("new.txt")) == "x\n", "the index has a new file with that line");
    expect(git(repo, {QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("--"), QStringLiteral("new.txt")}) == "AM new.txt\n",
           "status: added, and modified in the working tree");
    expect(unstage(repo, QStringLiteral("new.txt"), lines(0, 0, true)), "unstage all of the new file");
    expect(!indexEntry(repo, QStringLiteral("new.txt")).inIndex && readFile(repo + QStringLiteral("/new.txt")) == "x\ny\n",
           "out of the index again, still on disk");

    // Names that look like patterns
    writeFile(repo + QStringLiteral("/a*b.txt"), "one\n");
    writeFile(repo + QStringLiteral("/aXb.txt"), "other\n");
    commitAll(repo, QStringLiteral("Patterns"));
    const Entry star = indexEntry(repo, QStringLiteral("a*b.txt"));
    expect(star.inIndex && !star.conflict, "a name with * is taken literally");

    // No final newline
    writeFile(repo + QStringLiteral("/end.txt"), "a\nb");
    commitAll(repo, QStringLiteral("No newline"));
    writeFile(repo + QStringLiteral("/end.txt"), "a\nb\nc\n");
    expect(stage(repo, QStringLiteral("end.txt"), lines(2, 3, true)), "stage lines added after a last line without newline");
    expect(indexText(repo, QStringLiteral("end.txt")) == "a\nb\nc\n", "the index ends with a newline now");

    // CRLF files: committed as they are, and converted by core.autocrlf
    writeFile(repo + QStringLiteral("/dos.txt"), "a\r\nb\r\nc\r\n");
    commitAll(repo, QStringLiteral("CRLF"));
    writeFile(repo + QStringLiteral("/dos.txt"), "a\r\nB\r\nc\r\nd\r\n");
    expect(stage(repo, QStringLiteral("dos.txt"), lines(1, 1, true)), "stage a change of a CRLF file");
    expect(indexText(repo, QStringLiteral("dos.txt")) == "a\r\nB\r\nc\r\n", "its lines keep CRLF in the index");
    {
        const QString auto_ = newRepo(tmp, QStringLiteral("autocrlf"));
        git(auto_, {QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("true")});
        writeFile(auto_ + QStringLiteral("/w.txt"), "a\r\nb\r\n");
        commitAll(auto_, QStringLiteral("First"));
        expect(indexText(auto_, QStringLiteral("w.txt")) == "a\nb\n", "autocrlf: LF in the index");
        writeFile(auto_ + QStringLiteral("/w.txt"), "a\r\nb\r\nc\r\n");
        expect(stage(auto_, QStringLiteral("w.txt"), lines(2, 2, true)), "autocrlf: stage the added line");
        expect(indexText(auto_, QStringLiteral("w.txt")) == "a\nb\nc\n", "autocrlf: still LF only");
        // A new file written with CRLF lines: hash-object --path converts them.
        writeFile(auto_ + QStringLiteral("/n.txt"), "x\r\ny\r\n");
        expect(stage(auto_, QStringLiteral("n.txt"), lines(0, 1, true)), "autocrlf: stage a new CRLF file");
        expect(indexText(auto_, QStringLiteral("n.txt")) == "x\ny\n", "autocrlf: git's filters applied on the way in");
    }

    // A merge conflict: refused
    {
        const QString merge = newRepo(tmp, QStringLiteral("merge"));
        writeFile(merge + QStringLiteral("/c.txt"), "base\n");
        commitAll(merge, QStringLiteral("Base"));
        git(merge, {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("other")});
        writeFile(merge + QStringLiteral("/c.txt"), "other\n");
        commitAll(merge, QStringLiteral("Other"));
        git(merge, {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("main")});
        writeFile(merge + QStringLiteral("/c.txt"), "mine\n");
        commitAll(merge, QStringLiteral("Mine"));
        git(merge, {QStringLiteral("merge"), QStringLiteral("-q"), QStringLiteral("other")});
        const Entry c = indexEntry(merge, QStringLiteral("c.txt"));
        expect(!c.inIndex && c.conflict, "ls-files: a file in conflict");
        expect(!stage(merge, QStringLiteral("c.txt"), lines(0, 0, true)), "staging part of it is refused");
    }

    std::printf("%s\n", failures ? "staging: FAILED" : "staging: all passed");
    return failures ? 1 : 0;
}
