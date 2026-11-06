#include "git.h"

#include "prettier.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>

namespace Lieve
{
namespace
{
QProcessEnvironment gitEnvironment()
{
    static const QProcessEnvironment environment = [] {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0")); // never wait on an invisible terminal
        // Hooks (husky, lint-staged...) need node even when Lieve starts from
        // the menu with a minimal PATH.
        QStringList path = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        QStringList extra;
        const QString node = findNode();
        if (!node.isEmpty()) {
            extra << QFileInfo(node).absolutePath();
        }
        extra << QDir::homePath() + QStringLiteral("/.local/share/mise/shims") << QDir::homePath() + QStringLiteral("/.local/bin");
        for (const QString &dir : std::as_const(extra)) {
            if (!path.contains(dir) && QFileInfo(dir).isDir()) {
                path << dir;
            }
        }
        env.insert(QStringLiteral("PATH"), path.join(QLatin1Char(':')));
        return env;
    }();
    return environment;
}

QString outputText(const QByteArray &err, const QByteArray &out)
{
    QString text = QString::fromUtf8(err).trimmed();
    if (text.isEmpty()) {
        text = QString::fromUtf8(out).trimmed();
    }
    return text.isEmpty() ? QCoreApplication::translate("Lieve::GitRepo", "git gave no further details.") : text;
}

// Text after the n-th space (porcelain v2 records end with the path, which
// may itself contain spaces).
QByteArray afterSpaces(const QByteArray &record, int spaces)
{
    qsizetype pos = -1;
    for (int i = 0; i < spaces; ++i) {
        pos = record.indexOf(' ', pos + 1);
        if (pos < 0) {
            return {};
        }
    }
    return record.mid(pos + 1);
}

// Separators of the formats below (never in names or subjects).
constexpr char kField = '\x1f';
constexpr char kRecord = '\x1e';
}

namespace GitParse
{
QStringList branchesArgs()
{
    return {QStringLiteral("for-each-ref"),
            QStringLiteral("--sort=-committerdate"),
            QStringLiteral("--format=%(refname)%1f%(objectname:short)%1f%(committerdate:unix)%1f"
                           "%(upstream:short)%1f%(upstream:track,nobracket)%1f%(HEAD)%1f%(symref)"),
            QStringLiteral("refs/heads"),
            QStringLiteral("refs/remotes")};
}

QStringList stashesArgs()
{
    return {QStringLiteral("stash"), QStringLiteral("list"), QStringLiteral("--format=%gd%x1f%ct%x1f%gs")};
}

QStringList commitsArgs(const QString &path)
{
    QStringList args{QStringLiteral("log"), QStringLiteral("--max-count=200"), QStringLiteral("--format=%x1e%H%x1f%h%x1f%an%x1f%ct%x1f%s")};
    if (!path.isEmpty()) {
        // Through renames; --name-only gives the file's name in each commit.
        args << QStringLiteral("--follow") << QStringLiteral("--name-only") << QStringLiteral("-z") << QStringLiteral("--") << path;
    }
    return args;
}

QList<GitBranch> branches(const QByteArray &out)
{
    QList<GitBranch> result;
    const QList<QByteArray> lines = out.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> f = line.split(kField);
        if (f.size() < 7 || !f.at(6).isEmpty()) { // symbolic refs: origin/HEAD
            continue;
        }
        GitBranch b;
        const QString ref = QString::fromUtf8(f.at(0));
        if (ref.startsWith(QLatin1String("refs/heads/"))) {
            b.name = ref.mid(11);
        } else if (ref.startsWith(QLatin1String("refs/remotes/"))) {
            b.name = ref.mid(13);
            b.remote = true;
        } else {
            continue;
        }
        b.hash = QString::fromLatin1(f.at(1));
        b.time = f.at(2).toLongLong();
        b.upstream = QString::fromUtf8(f.at(3));
        // "ahead 1, behind 2" (for-each-ref doesn't translate it)
        const QList<QByteArray> track = f.at(4).split(',');
        for (const QByteArray &part : track) {
            const QByteArray t = part.trimmed();
            if (t.startsWith("ahead ")) {
                b.ahead = t.mid(6).toInt();
            } else if (t.startsWith("behind ")) {
                b.behind = t.mid(7).toInt();
            }
        }
        b.current = f.at(5) == "*";
        result << b;
    }
    return result;
}

QList<GitStash> stashes(const QByteArray &out)
{
    QList<GitStash> result;
    const QList<QByteArray> lines = out.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> f = line.split(kField);
        if (f.size() < 3 || !f.at(0).startsWith("stash@{")) {
            continue;
        }
        GitStash s;
        s.index = f.at(0).mid(7, f.at(0).size() - 8).toInt();
        s.time = f.at(1).toLongLong();
        // "On main: message" or "WIP on main: 1234abc subject" (git writes them in English)
        const QString subject = QString::fromUtf8(f.mid(2).join(kField));
        const qsizetype colon = subject.indexOf(QLatin1String(": "));
        s.message = subject;
        if (subject.startsWith(QLatin1String("On ")) && colon > 3) {
            s.branch = subject.mid(3, colon - 3);
            s.message = subject.mid(colon + 2);
        } else if (subject.startsWith(QLatin1String("WIP on ")) && colon > 7) {
            s.branch = subject.mid(7, colon - 7);
        }
        result << s;
    }
    return result;
}

QList<GitCommit> commits(const QByteArray &out)
{
    QList<GitCommit> result;
    const QList<QByteArray> records = out.split(kRecord);
    for (const QByteArray &record : records) {
        // "<fields>" or, with -z --name-only, "<fields>\0\n<path>\0"
        const qsizetype nul = record.indexOf('\0');
        const QList<QByteArray> f = (nul < 0 ? record : record.left(nul)).trimmed().split(kField);
        if (f.size() < 5) {
            continue;
        }
        GitCommit c;
        c.hash = QString::fromLatin1(f.at(0));
        c.shortHash = QString::fromLatin1(f.at(1));
        c.author = QString::fromUtf8(f.at(2));
        c.time = f.at(3).toLongLong();
        c.subject = QString::fromUtf8(f.mid(4).join(kField));
        if (nul >= 0) {
            QByteArray path = record.mid(nul + 1);
            while (path.startsWith('\n')) {
                path.remove(0, 1);
            }
            c.path = QString::fromUtf8(path.left(path.indexOf('\0')));
        }
        result << c;
    }
    return result;
}

bool validBranchName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String("@") || name == QLatin1String("HEAD") || name.startsWith(QLatin1Char('-'))
        || name.endsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('.')) || name.contains(QLatin1String(".."))
        || name.contains(QLatin1String("//")) || name.contains(QLatin1String("@{"))) {
        return false;
    }
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || QStringView(u" ~^:?*[\\").contains(c)) {
            return false;
        }
    }
    const QStringList parts = name.split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return part.startsWith(QLatin1Char('.')) || part.endsWith(QLatin1String(".lock"));
    });
}
}

GitRepo::GitRepo(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &GitRepo::refresh);
    connect(&m_autofetch, &QTimer::timeout, this, [this] {
        fetch(true);
    });
}

GitRepo::~GitRepo()
{
    m_queue.clear();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(1000);
        delete m_proc;
    }
}

bool GitRepo::gitInstalled()
{
    static const bool installed = !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
    return installed;
}

void GitRepo::resetState()
{
    m_root.clear();
    m_branch.clear();
    m_upstream.clear();
    m_ahead = 0;
    m_behind = 0;
    m_unborn = false;
    m_entries.clear();
    m_indexIds.clear();
}

void GitRepo::setDirectory(const QString &dir)
{
    ++m_generation;
    m_directory = dir;
    m_queue.clear();
    m_statusQueued = false;
    m_debounce.stop();
    m_autofetch.stop();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    setBusy(false);
    resetState();
    if (!dir.isEmpty() && gitInstalled()) {
        // Quick and needed before anything else.
        QProcess probe;
        probe.setWorkingDirectory(dir);
        probe.setProcessEnvironment(gitEnvironment());
        probe.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
        if (probe.waitForFinished(3000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) {
            m_root = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
        }
    }
    Q_EMIT changed();
    if (!m_root.isEmpty()) {
        refresh();
        setupAutofetch();
    }
}

void GitRepo::setupAutofetch()
{
    if (!configOrVscode(QStringLiteral("git/autofetch"), m_directory, QStringLiteral("git.autofetch"), false)) {
        return;
    }
    int minutes = config().value(QStringLiteral("git/autofetchMinutes"), 0).toInt();
    if (minutes <= 0) {
        minutes = std::max(1, vscodeSetting(m_directory, QStringLiteral("git.autofetchPeriod")).toInt(180) / 60);
    }
    m_autofetch.start(minutes * 60 * 1000);
    QTimer::singleShot(10000, this, [this, generation = m_generation] {
        if (generation == m_generation) {
            fetch(true);
        }
    });
}

bool GitRepo::hasGroup(GitEntry::Group group) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [group](const GitEntry &e) {
        return e.group == group;
    });
}

QString GitRepo::absolutePath(const QString &relative) const
{
    return QDir(m_root).filePath(relative);
}

QString GitRepo::indexState(const QString &relative) const
{
    const auto it = m_indexIds.constFind(relative);
    if (it != m_indexIds.constEnd()) {
        return it.value();
    }
    return m_root.isEmpty() || m_head.isEmpty() ? QString() : QStringLiteral("HEAD ") + m_head;
}

void GitRepo::setBusy(bool busy)
{
    if (busy != m_busy) {
        m_busy = busy;
        Q_EMIT busyChanged(busy);
    }
}

void GitRepo::enqueue(Job job)
{
    m_queue.append(std::move(job));
    if (!m_proc) {
        runNext();
    }
}

void GitRepo::runNext()
{
    if (m_proc) {
        return;
    }
    if (m_queue.isEmpty()) {
        setBusy(false);
        return;
    }
    const Job job = m_queue.takeFirst();
    const bool statusJob = job.args.value(0) == QLatin1String("status");
    if (statusJob) {
        m_statusQueued = false; // later changes need a new status
    }
    setBusy(job.busy || std::any_of(m_queue.cbegin(), m_queue.cend(), [](const Job &j) {
                return j.busy;
            }));

    auto *proc = new QProcess(this);
    m_proc = proc;
    proc->setWorkingDirectory(m_root.isEmpty() ? m_directory : m_root);
    QProcessEnvironment env = gitEnvironment();
    if (!job.busy) {
        // Background work (status, autofetch): never pop up password dialogs.
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("never"));
        env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    }
    proc->setProcessEnvironment(env);

    const quint64 generation = m_generation;
    const auto finish = [this, proc, job, generation](int exitCode) {
        const QByteArray out = proc->readAllStandardOutput();
        const QByteArray err = proc->readAllStandardError();
        proc->deleteLater();
        if (m_proc == proc) {
            m_proc = nullptr;
        }
        if (generation != m_generation) {
            return;
        }
        if (exitCode != 0 && !job.title.isEmpty()) {
            Q_EMIT failed(job.title, outputText(err, out));
        }
        if (job.done) {
            job.done(exitCode, out, err);
        }
        if (job.busy) {
            refresh();
        }
        runNext();
    };
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    connect(proc, &QProcess::errorOccurred, this, [finish, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            proc->disconnect();
            finish(-1);
        }
    });

    QStringList args{QStringLiteral("-c"), QStringLiteral("core.quotepath=false"), QStringLiteral("-c"), QStringLiteral("color.ui=never")};
    if (statusJob) {
        args << QStringLiteral("--no-optional-locks"); // don't fight with git in the terminal
    }
    args << job.args;
    proc->start(QStringLiteral("git"), args);
    if (!job.input.isEmpty()) {
        proc->write(job.input);
    }
    proc->closeWriteChannel();
}

void GitRepo::refresh()
{
    if (m_root.isEmpty() || m_statusQueued) {
        return;
    }
    m_statusQueued = true;
    Job job;
    job.args = {QStringLiteral("status"),
                QStringLiteral("--porcelain=v2"),
                QStringLiteral("-z"),
                QStringLiteral("--branch"),
                QStringLiteral("--untracked-files=all")};
    job.busy = false;
    job.done = [this](int code, const QByteArray &out, const QByteArray &) {
        if (code == 0) {
            parseStatus(out);
            Q_EMIT changed();
        }
    };
    enqueue(std::move(job));
}

void GitRepo::scheduleRefresh()
{
    if (!m_root.isEmpty()) {
        m_debounce.start();
    }
}

void GitRepo::parseStatus(const QByteArray &out)
{
    m_entries.clear();
    m_indexIds.clear();
    m_branch.clear();
    m_upstream.clear();
    m_ahead = 0;
    m_behind = 0;
    m_unborn = false;

    const QList<QByteArray> records = out.split('\0');
    for (qsizetype i = 0; i < records.size(); ++i) {
        const QByteArray &r = records.at(i);
        if (r.isEmpty()) {
            continue;
        }
        if (r.startsWith("# ")) {
            if (r.startsWith("# branch.head ")) {
                m_branch = QString::fromUtf8(r.mid(14));
            } else if (r.startsWith("# branch.oid ")) {
                m_unborn = r.mid(13) == "(initial)";
                m_head = QString::fromLatin1(r.mid(13));
            } else if (r.startsWith("# branch.upstream ")) {
                m_upstream = QString::fromUtf8(r.mid(18));
            } else if (r.startsWith("# branch.ab ")) {
                const QList<QByteArray> ab = r.mid(12).split(' ');
                if (ab.size() == 2) {
                    m_ahead = ab.at(0).mid(1).toInt();
                    m_behind = ab.at(1).mid(1).toInt();
                }
            }
            continue;
        }
        const char kind = r.at(0);
        if (kind == '?') {
            m_entries.append({GitEntry::Changes, QLatin1Char('U'), QString::fromUtf8(r.mid(2)), {}});
            m_indexIds.insert(m_entries.last().path, QString());
            continue;
        }
        if (r.size() < 4 || (kind != '1' && kind != '2' && kind != 'u')) {
            continue;
        }
        const char x = r.at(2);
        const char y = r.at(3);
        if (kind == 'u') {
            m_entries.append({GitEntry::Conflicts, QLatin1Char('!'), QString::fromUtf8(afterSpaces(r, 10)), {}});
            m_indexIds.insert(m_entries.last().path, QString());
            continue;
        }
        const QString path = QString::fromUtf8(afterSpaces(r, kind == '1' ? 8 : 9));
        // "<kind> <XY> <sub> <mH> <mI> <mW> <hH> <hI> ...": the index blob is the 8th field.
        QByteArray indexId = afterSpaces(r, 7);
        indexId = indexId.left(indexId.indexOf(' '));
        m_indexIds.insert(path, indexId.count('0') == indexId.size() ? QString() : QString::fromLatin1(indexId));
        QString origPath;
        if (kind == '2' && i + 1 < records.size()) {
            origPath = QString::fromUtf8(records.at(++i));
        }
        if (x != '.') {
            m_entries.append({GitEntry::Staged, QLatin1Char(x), path, origPath});
        }
        if (y != '.') {
            m_entries.append({GitEntry::Changes, QLatin1Char(y), path, {}});
        }
    }
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const GitEntry &a, const GitEntry &b) {
        if (a.group != b.group) {
            return a.group < b.group;
        }
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });
}

void GitRepo::stage(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return;
    }
    Job job;
    job.args = QStringList{QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--")} + paths;
    job.title = tr("Couldn't stage the files");
    enqueue(std::move(job));
}

void GitRepo::unstage(const QList<GitEntry> &entries)
{
    QStringList paths;
    for (const GitEntry &e : entries) {
        if (e.group == GitEntry::Staged) {
            paths << e.path;
            if (!e.origPath.isEmpty()) {
                paths << e.origPath;
            }
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    Job job;
    job.args = m_unborn ? QStringList{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("--")}
                        : QStringList{QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--")};
    job.args += paths;
    job.title = tr("Couldn't unstage the files");
    enqueue(std::move(job));
}

void GitRepo::discard(const QList<GitEntry> &entries)
{
    QStringList tracked;
    QStringList notTrashed;
    for (const GitEntry &e : entries) {
        if (e.group != GitEntry::Changes) {
            continue;
        }
        if (e.status == QLatin1Char('U')) {
            if (!QFile::moveToTrash(absolutePath(e.path))) {
                notTrashed << e.path;
            }
        } else {
            tracked << e.path;
        }
    }
    if (!notTrashed.isEmpty()) {
        Q_EMIT failed(tr("Couldn't move to the trash"), notTrashed.join(QLatin1Char('\n')));
    }
    if (tracked.isEmpty()) {
        refresh();
        return;
    }
    Job job;
    job.args = QStringList{QStringLiteral("restore"), QStringLiteral("--worktree"), QStringLiteral("--")} + tracked;
    job.title = tr("Couldn't discard the changes");
    enqueue(std::move(job));
}

void GitRepo::commit(const QString &message, bool amend, bool stageAll, After after)
{
    Job commitJob;
    commitJob.args = {QStringLiteral("commit")};
    if (amend) {
        commitJob.args << QStringLiteral("--amend");
    }
    if (amend && message.isEmpty()) {
        commitJob.args << QStringLiteral("--no-edit");
    } else {
        commitJob.args << QStringLiteral("-F") << QStringLiteral("-");
        commitJob.input = message.toUtf8();
    }
    commitJob.title = tr("Commit failed");
    commitJob.done = [this, after](int code, const QByteArray &, const QByteArray &) {
        if (code != 0) {
            return;
        }
        Q_EMIT committed();
        if (after == After::Push) {
            push();
        } else if (after == After::Sync) {
            sync();
        }
    };
    if (!stageAll) {
        enqueue(std::move(commitJob));
        return;
    }
    Job add;
    add.args = {QStringLiteral("add"), QStringLiteral("-A")};
    add.title = tr("Couldn't stage the changes");
    add.done = [this, commitJob](int code, const QByteArray &, const QByteArray &) {
        if (code == 0) {
            enqueue(commitJob);
        }
    };
    enqueue(std::move(add));
}

void GitRepo::push()
{
    Job job;
    if (m_upstream.isEmpty()) { // publish the branch
        job.args = {QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("HEAD")};
        job.title = tr("Publishing the branch failed");
    } else {
        job.args = {QStringLiteral("push")};
        job.title = tr("Push failed");
    }
    enqueue(std::move(job));
}

void GitRepo::pull()
{
    Job job;
    job.args = {QStringLiteral("pull")};
    job.title = tr("Pull failed");
    enqueue(std::move(job));
}

void GitRepo::sync()
{
    if (m_upstream.isEmpty()) {
        push(); // nothing to pull from yet: publish
        return;
    }
    Job job;
    job.args = {QStringLiteral("pull")};
    job.title = tr("Sync failed");
    job.done = [this](int code, const QByteArray &, const QByteArray &) {
        if (code == 0) {
            push();
        }
    };
    enqueue(std::move(job));
}

void GitRepo::fetch(bool quiet)
{
    if (m_root.isEmpty()) {
        return;
    }
    Job job;
    job.args = {QStringLiteral("fetch"), QStringLiteral("--prune")};
    job.title = quiet ? QString() : tr("Fetch failed");
    job.busy = !quiet;
    job.done = [this, quiet](int, const QByteArray &, const QByteArray &) {
        if (quiet) {
            refresh(); // ahead/behind may have changed
        }
    };
    enqueue(std::move(job));
}

void GitRepo::undoLastCommit()
{
    Job log;
    log.args = {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")};
    log.busy = false;
    log.done = [this](int code, const QByteArray &out, const QByteArray &) {
        if (code != 0) {
            Q_EMIT failed(tr("Undo Last Commit"), tr("There is no commit to undo."));
            return;
        }
        const QString message = QString::fromUtf8(out).trimmed();
        Job reset;
        reset.args = {QStringLiteral("reset"), QStringLiteral("--soft"), QStringLiteral("HEAD~1")};
        reset.title = tr("Couldn't undo the last commit");
        reset.done = [this, message](int c, const QByteArray &, const QByteArray &) {
            if (c == 0) {
                Q_EMIT messageRestored(message);
            }
        };
        enqueue(std::move(reset));
    };
    enqueue(std::move(log));
}

void GitRepo::init()
{
    if (m_directory.isEmpty()) {
        return;
    }
    Job job;
    job.args = {QStringLiteral("init")};
    job.title = tr("Couldn't initialize the repository");
    job.done = [this](int code, const QByteArray &, const QByteArray &) {
        if (code == 0) {
            const QString dir = m_directory;
            QTimer::singleShot(0, this, [this, dir] {
                setDirectory(dir);
            });
        }
    };
    enqueue(std::move(job));
}

void GitRepo::diff(const GitEntry &entry, const std::function<void(const QString &)> &done)
{
    Job job;
    job.busy = false;
    const QStringList base{QStringLiteral("diff"), QStringLiteral("--no-ext-diff")};
    switch (entry.group) {
    case GitEntry::Staged:
        job.args = base + QStringList{QStringLiteral("--cached"), QStringLiteral("-M"), QStringLiteral("--")};
        if (!entry.origPath.isEmpty()) {
            job.args << entry.origPath;
        }
        job.args << entry.path;
        break;
    case GitEntry::Changes:
        if (entry.status == QLatin1Char('U')) {
            job.args = base + QStringList{QStringLiteral("--no-index"), QStringLiteral("--"), QStringLiteral("/dev/null"), entry.path};
        } else {
            job.args = base + QStringList{QStringLiteral("--"), entry.path};
        }
        break;
    case GitEntry::Conflicts:
        job.args = base + QStringList{QStringLiteral("--"), entry.path};
        break;
    }
    job.done = [done](int, const QByteArray &out, const QByteArray &) {
        done(QString::fromUtf8(out));
    };
    enqueue(std::move(job));
}

void GitRepo::query(const QStringList &args, const std::function<void(int, const QByteArray &)> &done)
{
    if (m_root.isEmpty()) {
        done(-1, {});
        return;
    }
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(m_root);
    QProcessEnvironment env = gitEnvironment();
    env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("never"));
    env.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    proc->setProcessEnvironment(env);
    proc->setStandardInputFile(QProcess::nullDevice());
    const quint64 generation = m_generation;
    const auto finish = [this, proc, done, generation](int exitCode) {
        const QByteArray out = proc->readAllStandardOutput();
        proc->deleteLater();
        if (generation == m_generation) {
            done(exitCode, out);
        }
    };
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? code : -1);
    });
    connect(proc, &QProcess::errorOccurred, this, [finish, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            proc->disconnect();
            finish(-1);
        }
    });
    proc->start(QStringLiteral("git"),
                QStringList{QStringLiteral("-c"), QStringLiteral("core.quotepath=false"), QStringLiteral("-c"), QStringLiteral("color.ui=never"),
                            QStringLiteral("--no-optional-locks")}
                    + args);
}

void GitRepo::branches(const std::function<void(const QList<GitBranch> &)> &done)
{
    query(GitParse::branchesArgs(), [done](int code, const QByteArray &out) {
        done(code == 0 ? GitParse::branches(out) : QList<GitBranch>());
    });
}

void GitRepo::checkout(const QString &branch)
{
    Job job;
    job.args = {QStringLiteral("switch"), branch};
    job.title = tr("Couldn't check out “%1”").arg(branch);
    enqueue(std::move(job));
}

void GitRepo::checkoutTracking(const QString &remoteBranch)
{
    Job job;
    job.args = {QStringLiteral("switch"), QStringLiteral("--track"), remoteBranch};
    job.title = tr("Couldn't check out “%1”").arg(remoteBranch);
    enqueue(std::move(job));
}

void GitRepo::createBranch(const QString &name, const QString &from)
{
    Job job;
    // Not tracking the branch it starts from: Sync would push to that one.
    job.args = {QStringLiteral("switch"), QStringLiteral("--no-track"), QStringLiteral("-c"), name};
    if (!from.isEmpty()) {
        job.args << from;
    }
    job.title = tr("Couldn't create the branch “%1”").arg(name);
    enqueue(std::move(job));
}

bool GitRepo::canStash(bool includeUntracked) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [includeUntracked](const GitEntry &e) {
        return includeUntracked || e.status != QLatin1Char('U');
    });
}

void GitRepo::stash(const QString &message, bool includeUntracked)
{
    if (!canStash(includeUntracked)) {
        Q_EMIT failed(tr("Stash"), tr("There are no changes to stash."));
        return;
    }
    Job job;
    job.args = {QStringLiteral("stash"), QStringLiteral("push")};
    if (includeUntracked) {
        job.args << QStringLiteral("--include-untracked");
    }
    if (!message.isEmpty()) {
        job.args << QStringLiteral("--message") << message;
    }
    job.title = tr("Couldn't stash the changes");
    enqueue(std::move(job));
}

void GitRepo::stashes(const std::function<void(const QList<GitStash> &)> &done)
{
    query(GitParse::stashesArgs(), [done](int code, const QByteArray &out) {
        done(code == 0 ? GitParse::stashes(out) : QList<GitStash>());
    });
}

void GitRepo::popLatestStash()
{
    // In the queue: a stash being made just before counts.
    Job list;
    list.args = {QStringLiteral("stash"), QStringLiteral("list"), QStringLiteral("--format=%gd")};
    list.busy = false;
    list.done = [this](int code, const QByteArray &out, const QByteArray &) {
        if (code != 0 || out.trimmed().isEmpty()) {
            Q_EMIT failed(tr("Pop Latest Stash"), tr("There are no stashes in the repository."));
            return;
        }
        applyStash(0, true);
    };
    enqueue(std::move(list));
}

void GitRepo::applyStash(int index, bool pop)
{
    Job job;
    job.args = {QStringLiteral("stash"), pop ? QStringLiteral("pop") : QStringLiteral("apply"), QStringLiteral("stash@{%1}").arg(index)};
    job.title = pop ? tr("Couldn't pop the stash") : tr("Couldn't apply the stash");
    enqueue(std::move(job));
}

void GitRepo::dropStash(int index)
{
    Job job;
    job.args = {QStringLiteral("stash"), QStringLiteral("drop"), QStringLiteral("stash@{%1}").arg(index)};
    job.title = tr("Couldn't drop the stash");
    enqueue(std::move(job));
}

void GitRepo::log(const QString &path, const std::function<void(const QList<GitCommit> &)> &done)
{
    query(GitParse::commitsArgs(path), [done](int code, const QByteArray &out) {
        done(code == 0 ? GitParse::commits(out) : QList<GitCommit>());
    });
}

void GitRepo::show(const QString &hash, const QString &path, const std::function<void(const QString &)> &done)
{
    QStringList args{QStringLiteral("show"), QStringLiteral("--no-ext-diff"), QStringLiteral("--diff-merges=first-parent"), QStringLiteral("--format=medium")};
    if (path.isEmpty()) {
        args << QStringLiteral("--stat") << QStringLiteral("--patch");
    }
    args << hash;
    if (!path.isEmpty()) {
        args << QStringLiteral("--") << path;
    }
    query(args, [done](int, const QByteArray &out) {
        done(QString::fromUtf8(out));
    });
}
}
