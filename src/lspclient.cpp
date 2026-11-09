#include "lspclient.h"

#include "platform.h"

#include "prettier.h"
#include "version.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <algorithm>


namespace Lieve
{
namespace
{
bool traceEnabled()
{
    static const bool trace = qEnvironmentVariableIsSet("LIEVE_LSP_TRACE");
    return trace;
}

QString expandHome(const QString &path)
{
    return path.startsWith(QLatin1String("~/")) ? QDir::homePath() + path.mid(1) : path;
}

// The "<dir>/<version>/bin" folders of a version manager, newest first
// (a global package lives with the node version it was installed with).
QStringList versionBins(const QString &dir)
{
    QStringList versions = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
        const auto version = [](const QString &s) {
            return QVersionNumber::fromString(s.startsWith(QLatin1Char('v')) ? s.mid(1) : s);
        };
        return version(b) < version(a);
    });
    QStringList bins;
    for (const QString &v : std::as_const(versions)) {
        bins << dir + QLatin1Char('/') + v + QStringLiteral("/bin");
    }
    return bins;
}

// Where "npm install -g" puts programs, besides PATH: started from the
// application menu, PATH often lacks them.
QStringList globalBinDirs()
{
    const QString home = QDir::homePath();
    QStringList dirs;
    const QString node = findNode();
    if (!node.isEmpty()) {
        dirs << QFileInfo(node).absolutePath(); // nvm, fnm, system node: global bins sit next to node
    }
    dirs << versionBins(home + QStringLiteral("/.local/share/mise/installs/node")) << versionBins(home + QStringLiteral("/.nvm/versions/node"))
         << home + QStringLiteral("/.local/share/mise/shims") << home + QStringLiteral("/.volta/bin")
         << home + QStringLiteral("/.local/share/fnm/aliases/default/bin") << home + QStringLiteral("/.npm-global/bin")
         << home + QStringLiteral("/.npm-packages/bin") << home + QStringLiteral("/.local/share/pnpm") << home + QStringLiteral("/.bun/bin")
         << home + QStringLiteral("/.yarn/bin") << home + QStringLiteral("/.local/bin") << QStringLiteral("/usr/local/bin") << QStringLiteral("/usr/bin")
         << Platform::nodeBinDirs();
    dirs.removeAll(QString());
    dirs.removeDuplicates();
    return dirs;
}

QString findProgram(const QString &name, const QString &root)
{
    // The project's own copy first (node_modules/.bin above the project root).
    if (!root.isEmpty()) {
        QDir dir(root);
        do {
            const QString local = dir.filePath(QStringLiteral("node_modules/.bin/") + name);
            if (QFileInfo(local).isExecutable()) {
                return local;
            }
#ifdef Q_OS_WIN
            // npm's launchers on Windows: name.cmd (and name.ps1)
            if (QFileInfo::exists(local + QStringLiteral(".cmd"))) {
                return local + QStringLiteral(".cmd");
            }
#endif
        } while (dir.cdUp() && dir.absolutePath() != QDir::homePath() && !dir.isRoot());
    }
    QString found = QStandardPaths::findExecutable(name);
    if (found.isEmpty()) {
        found = QStandardPaths::findExecutable(name, globalBinDirs());
    }
    return found;
}

bool isNodeScript(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (suffix == QLatin1String("js") || suffix == QLatin1String("mjs") || suffix == QLatin1String("cjs")) {
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray first = file.readLine(256);
    return first.startsWith("#!") && first.contains("node");
}

QByteArray frame(const QJsonObject &message)
{
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (traceEnabled()) {
        qInfo().noquote() << "lsp >" << QString::fromUtf8(body.left(600));
    }
    return "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

QJsonArray numbers(int from, int to)
{
    QJsonArray values;
    for (int i = from; i <= to; ++i) {
        values.append(i);
    }
    return values;
}

QJsonObject clientCapabilities()
{
    const QJsonObject markup{{QStringLiteral("contentFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}}};
    return QJsonObject{
        {QStringLiteral("general"), QJsonObject{{QStringLiteral("positionEncodings"), QJsonArray{QStringLiteral("utf-16")}}}},
        {QStringLiteral("window"), QJsonObject{{QStringLiteral("workDoneProgress"), true}}},
        {QStringLiteral("workspace"),
         QJsonObject{{QStringLiteral("configuration"), true},
                     {QStringLiteral("workspaceFolders"), true},
                     {QStringLiteral("applyEdit"), false},
                     {QStringLiteral("workspaceEdit"), QJsonObject{{QStringLiteral("documentChanges"), true}}}}},
        {QStringLiteral("textDocument"),
         QJsonObject{
             {QStringLiteral("synchronization"),
              QJsonObject{{QStringLiteral("didSave"), true}, {QStringLiteral("willSave"), false}, {QStringLiteral("dynamicRegistration"), false}}},
             {QStringLiteral("publishDiagnostics"),
              QJsonObject{{QStringLiteral("versionSupport"), true},
                          {QStringLiteral("relatedInformation"), false},
                          {QStringLiteral("tagSupport"), QJsonObject{{QStringLiteral("valueSet"), numbers(1, 2)}}}}},
             {QStringLiteral("diagnostic"), QJsonObject{{QStringLiteral("dynamicRegistration"), false}, {QStringLiteral("relatedDocumentSupport"), false}}},
             {QStringLiteral("hover"), markup},
             {QStringLiteral("definition"), QJsonObject{{QStringLiteral("linkSupport"), true}}},
             {QStringLiteral("completion"),
              QJsonObject{
                  {QStringLiteral("contextSupport"), true},
                  {QStringLiteral("completionItem"),
                   QJsonObject{{QStringLiteral("snippetSupport"), false},
                               {QStringLiteral("labelDetailsSupport"), true},
                               {QStringLiteral("insertReplaceSupport"), false},
                               {QStringLiteral("documentationFormat"), QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")}},
                               {QStringLiteral("resolveSupport"),
                                QJsonObject{{QStringLiteral("properties"),
                                             QJsonArray{QStringLiteral("detail"), QStringLiteral("documentation"), QStringLiteral("additionalTextEdits")}}}}}},
                  {QStringLiteral("completionItemKind"), QJsonObject{{QStringLiteral("valueSet"), numbers(1, 25)}}},
              }},
             {QStringLiteral("documentSymbol"),
              QJsonObject{{QStringLiteral("hierarchicalDocumentSymbolSupport"), true},
                          {QStringLiteral("symbolKind"), QJsonObject{{QStringLiteral("valueSet"), numbers(1, 26)}}}}},
             // Go to References, Rename Symbol and the reference counts (symbolactions.h)
             {QStringLiteral("references"), QJsonObject()},
             {QStringLiteral("rename"), QJsonObject{{QStringLiteral("prepareSupport"), true}}},
             {QStringLiteral("codeLens"), QJsonObject()},
         }},
    };
}
}

std::optional<LspCommand> findLspCommand(const QString &configured, const QString &binary, const QStringList &defaultArgs, const QString &root)
{
    QString program;
    QStringList arguments = defaultArgs;
    const QString custom = configured.trimmed();
    if (!custom.isEmpty()) {
        QStringList parts = QProcess::splitCommand(custom);
        if (parts.isEmpty()) {
            return std::nullopt;
        }
        program = expandHome(parts.takeFirst());
        if (!parts.isEmpty()) { // just a path: the usual arguments
            arguments = parts;
        }
        if (!program.contains(QLatin1Char('/'))) {
            program = findProgram(program, root);
        } else if (QFileInfo(program).isRelative() && !root.isEmpty()) {
            program = QDir(root).absoluteFilePath(program);
        }
    } else {
        program = findProgram(binary, root);
    }
    const QFileInfo info(program);
    if (program.isEmpty() || !info.exists()) {
        return std::nullopt;
    }
    const QString real = info.canonicalFilePath();
    LspCommand command;
    if (isNodeScript(real)) {
        const QString node = findNode();
        if (node.isEmpty()) {
            return std::nullopt;
        }
        command.program = node;
        command.arguments = arguments;
        command.script = real;
    } else if (info.isExecutable()) {
        command.program = program;
        command.arguments = arguments;
    } else {
        return std::nullopt;
    }
    return command;
}

// ---------------------------------------------------------------------------

LspClient::LspClient(const LspServerSpec &spec, QObject *parent)
    : QObject(parent)
    , m_spec(spec)
{
}

LspClient::~LspClient()
{
    stop(true);
}

bool LspClient::hasCapability(const QString &name) const
{
    const QJsonValue value = m_capabilities.value(name);
    return value.isObject() || (value.isBool() && value.toBool());
}

QList<qint64> LspClient::processIds() const
{
    if (!m_process || m_process->processId() <= 0) {
        return {};
    }
    return {m_process->processId()};
}

void LspClient::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged();
    }
}

void LspClient::start()
{
    if (m_process || m_state == State::Failed) {
        return;
    }
    m_stopping = false;
    launch();
}

void LspClient::launch()
{
    m_buffer.clear();
    m_capabilities = {};
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(m_spec.environment);
    m_process->setWorkingDirectory(m_spec.root);
    // Its own process group, so stopping it also ends tsserver and the
    // other helpers it starts.
    Platform::ownGroup(m_process);
    if (traceEnabled()) {
        m_process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    } else {
        m_process->setStandardErrorFile(QProcess::nullDevice());
    }
    QProcess *process = m_process;
    connect(process, &QProcess::readyReadStandardOutput, this, &LspClient::readOutput);
    connect(process, &QProcess::finished, this, [this, process] {
        if (process == m_process) {
            onFinished();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_process) {
            m_error = QCoreApplication::translate("Lsp", "cannot run %1").arg(m_spec.program);
            m_process = nullptr;
            process->deleteLater();
            failAll();
            setState(State::Failed);
        }
    });
    setState(State::Starting);
    m_process->start(m_spec.program, m_spec.arguments);

    const QString rootUri = QUrl::fromLocalFile(m_spec.root).toString(QUrl::FullyEncoded);
    QJsonObject params{
        {QStringLiteral("processId"), qint64(QCoreApplication::applicationPid())},
        {QStringLiteral("clientInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("Lieve")}, {QStringLiteral("version"), QStringLiteral(LIEVE_VERSION)}}},
        {QStringLiteral("rootPath"), m_spec.root},
        {QStringLiteral("rootUri"), rootUri},
        {QStringLiteral("workspaceFolders"),
         QJsonArray{QJsonObject{{QStringLiteral("uri"), rootUri}, {QStringLiteral("name"), QFileInfo(m_spec.root).fileName()}}}},
        {QStringLiteral("capabilities"), clientCapabilities()},
        {QStringLiteral("trace"), QStringLiteral("off")},
    };
    if (!m_spec.initializationOptions.isEmpty()) {
        params.insert(QStringLiteral("initializationOptions"), m_spec.initializationOptions);
    }
    const int id = m_nextId++;
    m_initializeId = id;
    m_pending.insert(id, Pending{{}, false, [this](const QJsonValue &result, bool ok) {
                                     if (!ok || !result.isObject()) {
                                         if (m_error.isEmpty()) {
                                             m_error = QCoreApplication::translate("Lsp", "the server did not start");
                                         }
                                         m_restarts = 1; // a broken setup does not get better by retrying
                                         killGroup();
                                         return;
                                     }
                                     m_capabilities = result.toObject().value(QLatin1String("capabilities")).toObject();
                                     send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                                      {QStringLiteral("method"), QStringLiteral("initialized")},
                                                      {QStringLiteral("params"), QJsonObject()}});
                                     setState(State::Running);
                                     QPointer<LspClient> self(this);
                                     Q_EMIT ready(); // documents are opened before the requests that waited
                                     if (!self) {
                                         return;
                                     }
                                     const QList<QJsonObject> queue = std::exchange(m_queue, {});
                                     for (const QJsonObject &message : queue) {
                                         send(message);
                                     }
                                 }});
    send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("method"), QStringLiteral("initialize")},
                     {QStringLiteral("params"), params}});
    QTimer::singleShot(60000, this, [this, id] {
        if (m_pending.contains(id)) {
            m_error = QCoreApplication::translate("Lsp", "the server did not answer");
            finish(id, QJsonValue(), false);
        }
    });
}

void LspClient::stop(bool wait)
{
    m_stopping = true;
    m_queue.clear();
    failAll();
    QProcess *process = std::exchange(m_process, nullptr);
    m_buffer.clear();
    m_capabilities = {};
    if (m_state != State::Failed) {
        setState(State::Idle);
    }
    if (!process) {
        return;
    }
    process->disconnect(this);
    const qint64 pid = process->processId();
    if (process->state() == QProcess::Running) {
        // shutdown, then exit: servers end by themselves when their input closes too.
        process->write(frame(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                         {QStringLiteral("id"), m_nextId++},
                                         {QStringLiteral("method"), QStringLiteral("shutdown")}}));
        process->write(frame(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("exit")}}));
        process->closeWriteChannel();
    }
    if (wait) {
        if (!process->waitForFinished(500) && pid > 0) {
            Platform::endGroup(pid, false);
            if (!process->waitForFinished(500)) {
                Platform::endGroup(pid, true);
                process->waitForFinished(200);
            }
        }
        if (pid > 0) {
            Platform::endGroup(pid, false); // helpers left in the group
        }
        delete process;
        return;
    }
    // Finished in the background: helpers left in the group are ended too.
    process->setParent(QCoreApplication::instance());
    connect(process, &QProcess::finished, process, [process, pid] {
        if (pid > 0) {
            Platform::endGroup(pid, false);
        }
        process->deleteLater();
    });
    QTimer::singleShot(1500, process, [pid] {
        if (pid > 0) {
            Platform::endGroup(pid, true);
        }
    });
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
    }
}

void LspClient::killGroup()
{
    if (m_process && m_process->processId() > 0) {
        Platform::endGroup(m_process->processId(), false);
    }
}

int LspClient::request(const QString &method, const QJsonObject &params, QObject *context, Callback callback, int timeoutMs)
{
    const int id = m_nextId++;
    if (m_state == State::Failed) {
        QTimer::singleShot(0, context ? context : this, [callback] {
            callback(QJsonValue(), false);
        });
        return id;
    }
    start();
    m_pending.insert(id, Pending{context, context != nullptr, std::move(callback)});
    const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("method"), method},
                              {QStringLiteral("params"), params}};
    if (m_state == State::Running) {
        send(message);
    } else {
        m_queue << message;
    }
    QTimer::singleShot(timeoutMs, this, [this, id] {
        if (m_pending.contains(id)) {
            cancel(id);
        }
    });
    return id;
}

void LspClient::cancel(int id)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    Pending pending = std::move(*it);
    m_pending.erase(it);
    const qsizetype queued = m_queue.size();
    m_queue.removeIf([id](const QJsonObject &message) {
        return message.value(QLatin1String("id")).toInt() == id;
    });
    if (queued == m_queue.size() && m_state == State::Running) {
        notify(QStringLiteral("$/cancelRequest"), QJsonObject{{QStringLiteral("id"), id}});
    }
    if (pending.callback && (!pending.hasContext || pending.context)) {
        pending.callback(QJsonValue(), false);
    }
}

void LspClient::notify(const QString &method, const QJsonValue &params)
{
    if (m_state != State::Running) {
        return;
    }
    send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), method}, {QStringLiteral("params"), params}});
}

void LspClient::send(const QJsonObject &message)
{
    if (m_process) {
        m_process->write(frame(message));
    }
}

void LspClient::readOutput()
{
    if (!m_process) {
        return;
    }
    m_buffer += m_process->readAllStandardOutput();
    QPointer<LspClient> self(this);
    while (self && m_process) {
        const qsizetype headerEnd = m_buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        qsizetype length = -1;
        const QList<QByteArray> headers = m_buffer.left(headerEnd).split('\n');
        for (const QByteArray &header : headers) {
            if (header.trimmed().toLower().startsWith("content-length:")) {
                length = header.mid(header.indexOf(':') + 1).trimmed().toLongLong();
            }
        }
        if (length < 0) {
            m_buffer.remove(0, headerEnd + 4); // not a header: skip it
            continue;
        }
        if (m_buffer.size() < headerEnd + 4 + length) {
            return;
        }
        const QByteArray body = m_buffer.mid(headerEnd + 4, length);
        m_buffer.remove(0, headerEnd + 4 + length);
        if (traceEnabled()) {
            qInfo().noquote() << "lsp <" << QString::fromUtf8(body.left(600));
        }
        const QJsonDocument document = QJsonDocument::fromJson(body);
        if (document.isObject()) {
            handleMessage(document.object());
        }
    }
}

void LspClient::handleMessage(const QJsonObject &message)
{
    const QJsonValue id = message.value(QLatin1String("id"));
    const QString method = message.value(QLatin1String("method")).toString();
    if (!method.isEmpty()) {
        if (id.isUndefined() || id.isNull()) {
            Q_EMIT notificationReceived(method, message.value(QLatin1String("params")).toObject());
        } else {
            handleServerRequest(message);
        }
        return;
    }
    if (message.contains(QLatin1String("error"))) {
        const QString text = message.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
        if (id.toInt() == m_initializeId && !text.isEmpty()) {
            m_error = text.section(QLatin1Char('\n'), 0, 0);
        }
        QPointer<LspClient> self(this);
        m_requestError = text;
        finish(id.toInt(), QJsonValue(), false);
        if (self) {
            m_requestError.clear();
        }
    } else {
        finish(id.toInt(), message.value(QLatin1String("result")), true);
    }
}

void LspClient::handleServerRequest(const QJsonObject &message)
{
    const QString method = message.value(QLatin1String("method")).toString();
    QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), message.value(QLatin1String("id"))}};
    if (method == QLatin1String("workspace/configuration")) {
        QJsonArray answers;
        const QJsonArray items = message.value(QLatin1String("params")).toObject().value(QLatin1String("items")).toArray();
        for (qsizetype i = 0; i < items.size(); ++i) {
            answers.append(QJsonValue());
        }
        reply.insert(QStringLiteral("result"), answers);
    } else if (method == QLatin1String("workspace/workspaceFolders")) {
        const QString rootUri = QUrl::fromLocalFile(m_spec.root).toString(QUrl::FullyEncoded);
        reply.insert(QStringLiteral("result"),
                     QJsonArray{QJsonObject{{QStringLiteral("uri"), rootUri}, {QStringLiteral("name"), QFileInfo(m_spec.root).fileName()}}});
    } else if (method == QLatin1String("workspace/applyEdit")) {
        reply.insert(QStringLiteral("result"), QJsonObject{{QStringLiteral("applied"), false}});
    } else if (method == QLatin1String("client/registerCapability") || method == QLatin1String("client/unregisterCapability")
               || method == QLatin1String("window/workDoneProgress/create") || method == QLatin1String("window/showMessageRequest")
               || method == QLatin1String("workspace/codeLens/refresh") || method == QLatin1String("workspace/semanticTokens/refresh")
               || method == QLatin1String("workspace/inlayHint/refresh")) {
        reply.insert(QStringLiteral("result"), QJsonValue());
    } else if (method == QLatin1String("workspace/diagnostic/refresh")) {
        reply.insert(QStringLiteral("result"), QJsonValue());
        send(reply);
        Q_EMIT notificationReceived(method, QJsonObject()); // problems are asked again
        return;
    } else {
        reply.insert(QStringLiteral("error"),
                     QJsonObject{{QStringLiteral("code"), -32601}, {QStringLiteral("message"), QStringLiteral("Method not found: ") + method}});
    }
    send(reply);
}

void LspClient::finish(int id, const QJsonValue &result, bool ok)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    Pending pending = std::move(*it);
    m_pending.erase(it);
    if (pending.callback && (!pending.hasContext || pending.context)) {
        pending.callback(result, ok);
    }
}

void LspClient::failAll()
{
    const QList<int> ids = m_pending.keys();
    for (const int id : ids) {
        finish(id, QJsonValue(), false);
    }
    m_queue.clear();
}

void LspClient::onFinished()
{
    QProcess *process = std::exchange(m_process, nullptr);
    const qint64 pid = process->processId();
    if (pid > 0) {
        Platform::endGroup(pid, false); // what it started goes with it
    }
    if (m_error.isEmpty()) {
        m_error = process->exitStatus() == QProcess::CrashExit ? QCoreApplication::translate("Lsp", "the server crashed")
                                                                : QCoreApplication::translate("Lsp", "the server exited (code %1)").arg(process->exitCode());
    }
    process->deleteLater();
    m_buffer.clear();
    failAll();
    if (m_stopping) {
        setState(State::Idle);
        return;
    }
    if (m_restarts < 1) {
        ++m_restarts;
        setState(State::Starting);
        QTimer::singleShot(1000, this, [this] {
            if (!m_process && !m_stopping && m_state == State::Starting) {
                m_error.clear();
                launch();
            }
        });
        return;
    }
    setState(State::Failed);
}
}
