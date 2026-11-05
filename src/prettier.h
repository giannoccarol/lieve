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
    Status status = Skipped; // Prettier's
    QString text; // the new text, when ESLint or Prettier changed it
    int cursor = -1; // new cursor offset in `text` (UTF-16, like QString)
    QString message; // Prettier's: why it skipped the file, or its error
    Status eslint = Skipped; // ESLint's fixes (Formatted: it fixed something)
    QString eslintMessage; // a sentence to show: ESLint's error or timeout

    bool changed() const
    {
        return !text.isNull();
    }
};

// What runs on a file: ESLint's fixes first, then Prettier (VS Code's order:
// code actions on save, then format on save).
struct FormatSteps {
    bool eslint = false;
    bool prettier = false;
};

// A long-running "node" process that keeps the project's own Prettier (and
// its plugins) loaded, so formatting takes tens of milliseconds instead of a
// second; the project's ESLint too, in a worker thread, when its fixes are
// wanted. Started on demand, stopped after some idle minutes.
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
// [format] onSave= in lieve.conf. Before it, ESLint's "fix all" when
// "editor.codeActionsOnSave" asks for it ([format] eslint=).
class Formatter
{
public:
    static Formatter &instance();

    bool formatOnSave(const QString &projectRoot) const;
    bool fixOnSave(const QString &projectRoot) const;
    // Has a local Prettier (node_modules/prettier above the file)?
    bool available(const QString &file) const;
    // Has a local ESLint, and is the file in a language it checks?
    bool eslintAvailable(const QString &file, const QString &projectRoot) const;
    // On save, or for the Format Document command: Prettier even without
    // format on save, ESLint only when its fixes on save are on.
    FormatSteps steps(const QString &file, const QString &projectRoot, bool onSave) const;
    // Starts the daemon and loads the plugins for this kind of file, so the
    // first save is fast. Asynchronous.
    void warmUp(const QString &file, const QString &text, const QString &projectRoot);
    FormatResult format(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot, FormatSteps steps, int timeoutMs);

private:
    Formatter() = default;
    QString packageRoot(const QString &file, const QString &package) const;
    QString nodeExecutable() const;
    PrettierDaemon *daemonFor(const QString &file, QString *error);
    QJsonObject requestFor(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot, FormatSteps steps, int eslintTimeoutMs) const;

    QHash<QString, PrettierDaemon *> m_daemons;
};
}
