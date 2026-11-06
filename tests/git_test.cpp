// The lists of the branch, stash and history pickers: GitRepo's git commands
// run on a scratch repository (with a clone as its server) and their parsed
// output must describe it.
#include "git.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

namespace
{
QByteArray run(const QString &dir, const QStringList &args, int *exitCode = nullptr)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"),
            QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Test Author"), QStringLiteral("-c"), QStringLiteral("user.email=test@example.com"),
                        QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false"), QStringLiteral("-c"), QStringLiteral("core.quotepath=false")}
                + args);
    p.waitForFinished(10000);
    if (exitCode) {
        *exitCode = p.exitCode();
    }
    return p.readAllStandardOutput();
}

void writeFile(const QString &path, const QByteArray &text)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };
    using namespace Lieve;

    QTemporaryDir tmp;
    const QString server = tmp.filePath(QStringLiteral("server.git"));
    const QString repo = tmp.filePath(QStringLiteral("repo"));
    run(tmp.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main"), server});
    run(tmp.path(), {QStringLiteral("clone"), QStringLiteral("-q"), server, repo});
    run(repo, {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("main")});
    writeFile(repo + QStringLiteral("/a.txt"), "one\n");
    run(repo, {QStringLiteral("add"), QStringLiteral("a.txt")});
    run(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("First: add a/b")});
    run(repo, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("main")});
    // A rename, then a change: the file's history follows it.
    run(repo, {QStringLiteral("mv"), QStringLiteral("a.txt"), QStringLiteral("b c.txt")});
    run(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("Rename")});
    writeFile(repo + QStringLiteral("/b c.txt"), "one\ntwo\n");
    run(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Second line")});
    run(repo, {QStringLiteral("branch"), QStringLiteral("feature/x"), QStringLiteral("main~2")});
    run(repo, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("feature/x:remote-only")});
    run(repo, {QStringLiteral("fetch"), QStringLiteral("-q")});

    // Branches
    int code = 0;
    const QList<GitBranch> branches = GitParse::branches(run(repo, GitParse::branchesArgs(), &code));
    expect(code == 0, "for-each-ref runs");
    const auto find = [&branches](const QString &name, bool remote) -> const GitBranch * {
        for (const GitBranch &b : branches) {
            if (b.name == name && b.remote == remote) {
                return &b;
            }
        }
        return nullptr;
    };
    const GitBranch *main = find(QStringLiteral("main"), false);
    expect(main && main->current && main->upstream == QLatin1String("origin/main"), "main: current, tracks origin/main");
    expect(main && main->ahead == 2 && main->behind == 0, "main: 2 commits ahead");
    expect(main && main->hash.size() >= 7 && main->time > 0, "main: short hash and date");
    const GitBranch *feature = find(QStringLiteral("feature/x"), false);
    expect(feature && !feature->current && feature->upstream.isEmpty(), "feature/x: local, no upstream");
    expect(find(QStringLiteral("origin/remote-only"), true) && find(QStringLiteral("origin/main"), true), "remote branches listed");
    expect(!find(QStringLiteral("origin/HEAD"), true), "origin/HEAD left out");

    // Stashes: a message, then untracked files without one
    writeFile(repo + QStringLiteral("/b c.txt"), "changed\n");
    run(repo, {QStringLiteral("stash"), QStringLiteral("push"), QStringLiteral("--message"), QStringLiteral("Half done: really")});
    writeFile(repo + QStringLiteral("/new.txt"), "new\n");
    run(repo, {QStringLiteral("stash"), QStringLiteral("push"), QStringLiteral("--include-untracked")});
    const QList<GitStash> stashes = GitParse::stashes(run(repo, GitParse::stashesArgs()));
    expect(stashes.size() == 2, "two stashes");
    const GitStash wip = stashes.value(0);
    expect(wip.index == 0 && wip.message.startsWith(QLatin1String("WIP on main: ")) && wip.branch == QLatin1String("main"), "stash@{0}: WIP message, branch");
    expect(stashes.value(1).index == 1 && stashes.value(1).message == QLatin1String("Half done: really") && stashes.value(1).branch == QLatin1String("main"),
           "stash@{1}: own message, branch");
    expect(stashes.value(1).time > 0, "stash date");

    // History: of the project, and of the renamed file
    const QList<GitCommit> all = GitParse::commits(run(repo, GitParse::commitsArgs(QString())));
    expect(all.size() == 3 && all.value(0).subject == QLatin1String("Second line") && all.value(2).subject == QLatin1String("First: add a/b"),
           "project history, newest first");
    expect(all.value(0).author == QLatin1String("Test Author") && all.value(0).hash.size() == 40 && all.value(0).shortHash.size() >= 7,
           "author and hashes");
    const QList<GitCommit> file = GitParse::commits(run(repo, GitParse::commitsArgs(QStringLiteral("b c.txt"))));
    expect(file.size() == 3, "file history follows the rename");
    expect(file.value(0).path == QLatin1String("b c.txt") && file.value(2).path == QLatin1String("a.txt"), "the file's name in each commit");
    expect(GitParse::commits(run(repo, GitParse::commitsArgs(QStringLiteral("new.txt")))).isEmpty(), "no history for a file never committed");

    // Branch names
    expect(GitParse::validBranchName(QStringLiteral("feature/new-thing")), "valid name");
    for (const char *bad : {"", "-x", "a..b", "a b", "a~b", "a:b", "x/", "x.", ".x", "a/.b", "x.lock", "a//b", "@", "HEAD", "a@{b"}) {
        if (GitParse::validBranchName(QString::fromLatin1(bad))) {
            std::printf("FAIL invalid name accepted: '%s'\n", bad);
            ++failures;
        }
    }

    std::printf("%s\n", failures ? "git: FAILED" : "git: all passed");
    return failures ? 1 : 0;
}
