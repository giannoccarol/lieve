#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

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
    void runNext();
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
