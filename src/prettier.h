#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <optional>

class QProcess;

namespace Lieve
{
// Path of "node": [format] node=, PATH, then mise / volta / fnm / nvm.
QString findNode();

struct FormatResult {
    enum Status {
        Skipped, // no Prettier for this file, ignored, unsupported, or disabled
        Unchanged,
        Formatted,
        Failed, // syntax error, Node missing, timeout...
    };
    Status status = Skipped;
    QString text;
    int cursor = -1; // new cursor offset in `text` (UTF-16, like QString)
    QString message;
};

// A long-running "node" process that keeps the project's own Prettier (and
// its plugins) loaded, so formatting takes tens of milliseconds instead of a
// second. Started on demand, stopped after some idle minutes.
class PrettierDaemon : public QObject
{
public:
    PrettierDaemon(const QString &root, const QString &node, QObject *parent);
    ~PrettierDaemon() override;

    int request(QJsonObject payload);
    // Waits (without running the event loop) for the answer to `id`.
    std::optional<QJsonObject> wait(int id, int timeoutMs);
    void discard(int id);
    QString fatalError() const
    {
        return m_fatal;
    }

private:
    void start();
    void stop();
    void readLines();

    QString m_root;
    QString m_node;
    QProcess *m_proc = nullptr;
    QByteArray m_buffer;
    QHash<int, QJsonObject> m_answers;
    QSet<int> m_discarded;
    int m_nextId = 1;
    QTimer m_idle;
    QString m_fatal;
};

// Format on save with Prettier, following the same rules VS Code uses:
// "editor.formatOnSave" and "prettier.requireConfig" from the project's
// .vscode/settings.json or the user's VS Code settings, overridable with
// [format] onSave= in lieve.conf.
class Formatter
{
public:
    static Formatter &instance();

    bool formatOnSave(const QString &projectRoot) const;
    // Has a local Prettier (node_modules/prettier above the file)?
    bool available(const QString &file) const;
    // Starts the daemon and loads the plugins for this kind of file, so the
    // first save is fast. Asynchronous.
    void warmUp(const QString &file, const QString &text, const QString &projectRoot);
    FormatResult format(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot, int timeoutMs);

private:
    Formatter() = default;
    QString prettierRoot(const QString &file) const;
    QString nodeExecutable() const;
    PrettierDaemon *daemonFor(const QString &file, QString *error);
    QJsonObject requestFor(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot) const;

    QHash<QString, PrettierDaemon *> m_daemons;
};
}
