#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

class QProcess;

namespace Lieve
{
struct GitEntry {
    enum Group {
        Conflicts,
        Staged,
        Changes,
    };
    Group group = Changes;
    QChar status; // M A D R C T, U = untracked, ! = conflict
    QString path; // relative to the repository root
    QString origPath; // renamed from (staged renames)
};

// A branch, for the checkout picker.
struct GitBranch {
    QString name; // "main", or "origin/main" for remote ones
    bool remote = false;
    bool current = false; // checked out
    QString hash; // short
    qint64 time = 0; // of its last commit, seconds since the epoch
    QString upstream; // local branches: "origin/main" ("" = none)
    int ahead = 0;
    int behind = 0;
};

struct GitStash {
    int index = 0; // stash@{index}
    QString message;
    QString branch; // made on
    qint64 time = 0;
};

struct GitCommit {
    QString hash;
    QString shortHash;
    QString author;
    qint64 time = 0;
    QString subject;
    QString path; // file history: the file's path in this commit (it may have been renamed since)
};

// A file as the index holds it, read to stage or unstage part of it.
struct GitIndexFile {
    bool inIndex = false;
    QString mode; // "100644", "100755"; "" when not in the index
    QByteArray data;
    bool inHead = false;
    QByteArray head; // the file in HEAD, when asked for
};

// What goes back into the index: the new content, or the file out of it.
struct GitIndexWrite {
    QByteArray data;
    QString mode;
    bool remove = false;
};

// The git commands that GitRepo reads lists with, and their parsers (public
// for the tests).
namespace GitParse
{
QStringList branchesArgs();
QList<GitBranch> branches(const QByteArray &out); // remote HEADs are left out
QStringList stashesArgs();
QList<GitStash> stashes(const QByteArray &out);
QStringList commitsArgs(const QString &path); // "" = the whole project
QList<GitCommit> commits(const QByteArray &out);
// Whether git takes `name` for a new branch (the rules of check-ref-format).
bool validBranchName(const QString &name);
// The index entry of one file (ls-files --stage): its mode and blob; false
// when it is not in the index, `conflict` when it is there in several stages.
QStringList indexEntryArgs(const QString &path);
bool indexEntry(const QByteArray &out, QString *mode, QString *blob, bool *conflict);
// New index content for a file: hash-object -w (the content on stdin, git's
// filters for that path applied: end of line conversion...) gives the blob,
// update-index puts it in the index; or the file leaves the index.
QStringList hashObjectArgs(const QString &path);
QStringList updateIndexArgs(const QString &mode, const QString &blob, const QString &path);
QStringList removeFromIndexArgs(const QString &path);
}

// One git repository, driven through the git command line. Commands run one
// at a time in the background; the status is re-read after every change.
class GitRepo : public QObject
{
    Q_OBJECT
public:
    enum class After {
        Nothing,
        Push,
        Sync,
    };

    explicit GitRepo(QObject *parent = nullptr);
    ~GitRepo() override;

    // Finds the repository containing `dir` ("" = none).
    void setDirectory(const QString &dir);
    QString directory() const
    {
        return m_directory;
    }
    QString root() const
    {
        return m_root;
    }
    bool isRepository() const
    {
        return !m_root.isEmpty();
    }
    static bool gitInstalled();

    QString branch() const
    {
        return m_branch;
    }
    QString upstream() const
    {
        return m_upstream;
    }
    int ahead() const
    {
        return m_ahead;
    }
    int behind() const
    {
        return m_behind;
    }
    bool unborn() const
    {
        return m_unborn;
    }
    // The commit HEAD points to ("(initial)" before the first commit).
    QString head() const
    {
        return m_head;
    }
    const QList<GitEntry> &entries() const
    {
        return m_entries;
    }
    // What the index holds for a file (path relative to the root), to notice
    // when it changes: its blob id, "HEAD <commit>" for files without changes
    // (the same blob as HEAD), "" for untracked files and conflicts.
    QString indexState(const QString &relative) const;
    bool hasGroup(GitEntry::Group group) const;
    bool isBusy() const
    {
        return m_busy;
    }
    QString absolutePath(const QString &relative) const;

    void refresh();
    void scheduleRefresh(); // coalesces bursts of events
    void stage(const QStringList &paths);
    void unstage(const QList<GitEntry> &entries);
    void discard(const QList<GitEntry> &entries); // the caller asks for confirmation
    void commit(const QString &message, bool amend, bool stageAll, After after);
    void push();
    void pull();
    void sync();
    void fetch(bool quiet);
    void undoLastCommit();
    void init();
    void diff(const GitEntry &entry, const std::function<void(const QString &)> &done);
    // A file's content as git stores it, in the queue: `spec` ":path" for the
    // index, "HEAD:path"; found = false when there is none.
    void readFile(const QString &spec, const std::function<void(bool found, const QByteArray &data)> &done);
    // Part of a file staged or unstaged, like VS Code: reads the file's index
    // entry and content (and its HEAD content at `headPath`, unless empty),
    // lets `change` compute the new index content and writes it, in one run
    // of the queue that nothing else interrupts. `change` returns nothing to
    // leave the index as it is.
    void rewriteIndex(const QString &path,
                      const QString &headPath,
                      const std::function<std::optional<GitIndexWrite>(const GitIndexFile &)> &change,
                      const QString &failTitle);

    // Branches, stashes and history. The lists are read by their own git
    // process, beside the queue (a slow fetch doesn't hold up a picker).
    void branches(const std::function<void(const QList<GitBranch> &)> &done);
    void checkout(const QString &branch);
    void checkoutTracking(const QString &remoteBranch); // creates the local branch that tracks it
    void createBranch(const QString &name, const QString &from); // and switches to it ("" = from HEAD)
    bool canStash(bool includeUntracked) const; // there are changes to stash
    void stash(const QString &message, bool includeUntracked);
    void stashes(const std::function<void(const QList<GitStash> &)> &done);
    void popLatestStash();
    void applyStash(int index, bool pop);
    void dropStash(int index);
    // The last 200 commits of the project, or of one file ("" = all).
    void log(const QString &path, const std::function<void(const QList<GitCommit> &)> &done);
    // A commit as `git show` prints it: the whole commit or only `path`.
    void show(const QString &hash, const QString &path, const std::function<void(const QString &)> &done);

Q_SIGNALS:
    void changed();
    void busyChanged(bool busy);
    void failed(const QString &title, const QString &details);
    void committed();
    void messageRestored(const QString &message);

private:
    struct Job {
        QStringList args;
        QString title; // shown when it fails; empty = fail silently
        QByteArray input;
        bool busy = true; // shows the progress line
        std::function<void(int exitCode, const QByteArray &out, const QByteArray &err)> done;
    };

    void enqueue(Job job);
    void enqueueNext(Job job); // before the rest of the queue: the next step of a job
    void runNext();
    // A read-only git command in a process of its own; `done` is dropped
    // when the folder changes meanwhile.
    void query(const QStringList &args, const std::function<void(int exitCode, const QByteArray &out)> &done);
    void parseStatus(const QByteArray &out);
    void setBusy(bool busy);
    void resetState();
    void setupAutofetch();

    QString m_directory;
    QString m_root;
    QString m_branch;
    QString m_upstream;
    int m_ahead = 0;
    int m_behind = 0;
    bool m_unborn = false;
    QString m_head;
    QList<GitEntry> m_entries;
    QHash<QString, QString> m_indexIds; // files in the status -> index blob ("" = none)

    QList<Job> m_queue;
    QProcess *m_proc = nullptr;
    bool m_statusQueued = false;
    bool m_busy = false;
    quint64 m_generation = 0; // bumped on setDirectory: stale answers are ignored
    QTimer m_debounce;
    QTimer m_autofetch;
};
}
