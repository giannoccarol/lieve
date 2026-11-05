#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QProcess;

namespace Lieve
{
// How to start one language server.
struct LspServerSpec {
    QString id; // "typescript", "angular": one server per window and id
    QString name; // shown to the user
    QString program;
    QStringList arguments;
    QString root;
    QJsonObject initializationOptions;
    QProcessEnvironment environment;
};

// The command that starts a language server: `configured` ([lsp] key) when
// set, else `binary` from the project's node_modules/.bin, PATH or the
// global folders of npm and the node version managers. Node scripts run with
// the node Lieve finds, so a minimal PATH (started from the menu) works too.
struct LspCommand {
    QString program; // node, for node scripts
    QString script; // the resolved node script, if it is one
    QStringList arguments; // the server's own
    QStringList processArguments() const
    {
        return script.isEmpty() ? arguments : QStringList{script} + arguments;
    }
};
std::optional<LspCommand> findLspCommand(const QString &configured, const QString &binary, const QStringList &defaultArgs, const QString &root);

// One language server process, spoken to with JSON-RPC 2.0 over stdio
// (Content-Length framing). Everything is asynchronous: requests take a
// callback and time out. A crashed server is started again once; after a
// second crash it stays off.
class LspClient : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Starting, // process started, initialize sent
        Running,
        Failed, // could not start, or crashed twice
    };
    // `result` is null on errors and timeouts (`ok` false).
    using Callback = std::function<void(const QJsonValue &result, bool ok)>;

    LspClient(const LspServerSpec &spec, QObject *parent);
    ~LspClient() override;

    void start();
    // Polite stop (shutdown, exit), then the whole process group is killed;
    // in the background unless `wait` (closing the window: at most ~1 s).
    void stop(bool wait = false);
    State state() const
    {
        return m_state;
    }
    const LspServerSpec &spec() const
    {
        return m_spec;
    }
    QJsonObject capabilities() const
    {
        return m_capabilities;
    }
    bool hasCapability(const QString &name) const;
    QString lastError() const
    {
        return m_error;
    }
    QList<qint64> processIds() const;

    // Requests sent before the server is ready wait for it. `context` going
    // away drops the answer.
    int request(const QString &method, const QJsonObject &params, QObject *context, Callback callback, int timeoutMs = 10000);
    void cancel(int id);
    // Only while running (documents are opened again on ready()).
    void notify(const QString &method, const QJsonValue &params);

Q_SIGNALS:
    // Initialized (also after a restart): open the documents.
    void ready();
    void notificationReceived(const QString &method, const QJsonObject &params);
    void stateChanged();

private:
    struct Pending {
        QPointer<QObject> context;
        bool hasContext = false;
        Callback callback;
    };
    void launch();
    void send(const QJsonObject &message);
    void readOutput();
    void handleMessage(const QJsonObject &message);
    void handleServerRequest(const QJsonObject &message);
    void finish(int id, const QJsonValue &result, bool ok);
    void onFinished();
    void failAll();
    void setState(State state);
    void killGroup();

    LspServerSpec m_spec;
    QProcess *m_process = nullptr;
    State m_state = State::Idle;
    QByteArray m_buffer;
    QJsonObject m_capabilities;
    QHash<int, Pending> m_pending;
    QList<QJsonObject> m_queue; // requests waiting for initialize
    int m_nextId = 1;
    int m_initializeId = -1;
    int m_restarts = 0;
    bool m_stopping = false;
    QString m_error;
};
}
