#include "prettier.h"

#include "jsonc.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>

namespace Lieve
{
namespace
{
// Runs inside node: loads the project's own Prettier once, then answers
// format requests (JSON lines on stdin -> JSON lines on stdout).
const char kDaemonScript[] = R"JS(
const root = process.argv[1];
for (const k of ['log', 'info', 'warn', 'debug', 'trace']) {
  console[k] = (...args) => process.stderr.write(args.map(String).join(' ') + '\n');
}
const send = (o) => process.stdout.write(JSON.stringify(o) + '\n');
const { createRequire } = await import('node:module');
const path = await import('node:path');
const { pathToFileURL } = await import('node:url');
const readline = await import('node:readline');
let api;
try {
  const require = createRequire(path.join(root, 'package.json'));
  const mod = await import(pathToFileURL(require.resolve('prettier')).href);
  api = mod.default ?? mod;
} catch (e) {
  send({ id: 0, status: 'fatal', message: String(e?.message ?? e) });
  process.exit(1);
}
const ignorePath = path.join(root, '.prettierignore');
async function handle(req) {
  const info = await api.getFileInfo(req.file, { ignorePath, resolveConfig: true });
  if (info.ignored) return { status: 'skipped', message: 'ignored' };
  if (!info.inferredParser) return { status: 'skipped', message: 'unsupported' };
  if (req.requireConfig && !(await api.resolveConfigFile(req.file))) return { status: 'skipped', message: 'no-config' };
  const options = (await api.resolveConfig(req.file, { editorconfig: true })) ?? {};
  const cursor = Number.isInteger(req.cursor) && req.cursor >= 0 ? req.cursor : 0;
  const result = await api.formatWithCursor(req.text, { ...options, filepath: req.file, cursorOffset: cursor });
  if (result.formatted === req.text) return { status: 'unchanged' };
  return { status: 'formatted', text: result.formatted, cursor: req.cursor >= 0 ? result.cursorOffset : -1 };
}
readline.createInterface({ input: process.stdin, crlfDelay: Infinity })
  .on('line', async (line) => {
    let req;
    try { req = JSON.parse(line); } catch { return; }
    let answer;
    try { answer = await handle(req); } catch (e) { answer = { status: 'error', message: String(e?.message ?? e) }; }
    send({ id: req.id, ...answer });
    // Formatting leaves a lot of garbage: collect it once the saves stop, so
    // the idle daemon keeps about half the memory.
    clearTimeout(globalThis.lieveGc);
    globalThis.lieveGc = setTimeout(() => globalThis.gc?.(), 300);
  })
  .on('close', () => process.exit(0));
)JS";

QString firstLine(const QString &text)
{
    const QString line = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    return line.size() > 200 ? line.left(199) + QStringLiteral("…") : line;
}
}

// ---------------------------------------------------------------------------

PrettierDaemon::PrettierDaemon(const QString &root, const QString &node, QObject *parent)
    : QObject(parent)
    , m_root(root)
    , m_node(node)
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(qMax(1, config().value(QStringLiteral("format/keepAliveMinutes"), 10).toInt()) * 60 * 1000);
    connect(&m_idle, &QTimer::timeout, this, &PrettierDaemon::stop);
}

PrettierDaemon::~PrettierDaemon()
{
    stop();
}

void PrettierDaemon::start()
{
    if (m_proc || !m_fatal.isEmpty()) {
        return;
    }
    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_root);
    m_proc->setStandardErrorFile(QProcess::nullDevice()); // plugin chatter
    QProcess *proc = m_proc;
    connect(proc, &QProcess::readyReadStandardOutput, this, &PrettierDaemon::readLines);
    connect(proc, &QProcess::finished, this, [this, proc] {
        if (m_proc == proc) {
            readLines();
            m_proc = nullptr;
            m_buffer.clear();
        }
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_proc == proc) {
            m_fatal = QCoreApplication::translate("Prettier", "cannot run %1").arg(m_node);
            m_proc = nullptr;
            proc->deleteLater();
        }
    });
    // A small young generation and a collection after each burst of saves
    // halve the memory of the daemon (about 150 instead of 300 MB with a large
    // project), formatting just as fast.
    m_proc->start(m_node,
                  {QStringLiteral("--max-semi-space-size=4"),
                   QStringLiteral("--expose-gc"),
                   QStringLiteral("--input-type=module"),
                   QStringLiteral("-e"),
                   QString::fromUtf8(kDaemonScript),
                   m_root});
}

void PrettierDaemon::stop()
{
    m_idle.stop();
    QProcess *proc = m_proc;
    m_proc = nullptr;
    m_buffer.clear();
    m_answers.clear();
    m_discarded.clear();
    if (!proc) {
        return;
    }
    proc->disconnect(this);
    proc->closeWriteChannel(); // the daemon exits when its input ends
    if (!proc->waitForFinished(500)) {
        proc->kill();
        proc->waitForFinished(500);
    }
    delete proc;
}

int PrettierDaemon::request(QJsonObject payload)
{
    start();
    if (!m_proc) {
        return -1;
    }
    const int id = m_nextId++;
    payload.insert(QStringLiteral("id"), id);
    m_proc->write(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
    m_idle.start();
    return id;
}

void PrettierDaemon::discard(int id)
{
    if (!m_answers.remove(id)) {
        m_discarded.insert(id);
    }
}

void PrettierDaemon::readLines()
{
    if (!m_proc) {
        return;
    }
    m_buffer += m_proc->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        const QJsonObject answer = QJsonDocument::fromJson(line).object();
        if (answer.isEmpty()) {
            continue;
        }
        const int id = answer.value(QLatin1String("id")).toInt(-1);
        if (id == 0) {
            if (answer.value(QLatin1String("status")).toString() == QLatin1String("fatal")) {
                m_fatal = firstLine(answer.value(QLatin1String("message")).toString());
            }
            continue;
        }
        if (m_discarded.remove(id)) {
            continue;
        }
        m_answers.insert(id, answer);
    }
}

std::optional<QJsonObject> PrettierDaemon::wait(int id, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (const auto it = m_answers.find(id); it != m_answers.end()) {
            const QJsonObject answer = *it;
            m_answers.erase(it);
            return answer;
        }
        QProcess *proc = m_proc;
        const qint64 left = timeoutMs - timer.elapsed();
        if (!proc || left <= 0) {
            discard(id);
            return std::nullopt;
        }
        // Blocks without running the event loop: no re-entrancy while saving.
        if (proc->waitForReadyRead(int(left))) {
            readLines();
        } else if (m_proc != proc || proc->state() == QProcess::NotRunning) {
            readLines();
            if (!m_answers.contains(id)) {
                return std::nullopt;
            }
        }
    }
}

// ---------------------------------------------------------------------------

Formatter &Formatter::instance()
{
    static Formatter formatter;
    return formatter;
}

bool Formatter::formatOnSave(const QString &projectRoot) const
{
    return configOrVscode(QStringLiteral("format/onSave"), projectRoot, QStringLiteral("editor.formatOnSave"), false);
}

QString Formatter::prettierRoot(const QString &file) const
{
    QDir dir = QFileInfo(file).absoluteDir();
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("node_modules/prettier/package.json")))) {
            return dir.absolutePath();
        }
    } while (dir.cdUp());
    return {};
}

bool Formatter::available(const QString &file) const
{
    return !prettierRoot(file).isEmpty();
}

QString Formatter::nodeExecutable() const
{
    return findNode();
}

QString findNode()
{
    QString configured = config().value(QStringLiteral("format/node")).toString().trimmed();
    if (configured.startsWith(QLatin1String("~/"))) {
        configured = QDir::homePath() + configured.mid(1);
    }
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) {
        return configured;
    }
    QString found = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (!found.isEmpty()) {
        return found;
    }
    // Started from the application menu the PATH is often minimal: look
    // where version managers keep node.
    const QString home = QDir::homePath();
    found = QStandardPaths::findExecutable(QStringLiteral("node"),
                                           {home + QStringLiteral("/.local/share/mise/shims"),
                                            home + QStringLiteral("/.volta/bin"),
                                            home + QStringLiteral("/.local/share/fnm/aliases/default/bin"),
                                            home + QStringLiteral("/.local/bin"),
                                            QStringLiteral("/usr/local/bin"),
                                            QStringLiteral("/usr/bin")});
    if (!found.isEmpty()) {
        return found;
    }
    QStringList nvm = QDir(home + QStringLiteral("/.nvm/versions/node")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(nvm.begin(), nvm.end(), [](const QString &a, const QString &b) {
        return QVersionNumber::fromString(a.mid(1)) < QVersionNumber::fromString(b.mid(1));
    });
    if (!nvm.isEmpty()) {
        const QString candidate = home + QStringLiteral("/.nvm/versions/node/") + nvm.last() + QStringLiteral("/bin/node");
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return {};
}

PrettierDaemon *Formatter::daemonFor(const QString &file, QString *error)
{
    const QString root = prettierRoot(file);
    if (root.isEmpty()) {
        return nullptr;
    }
    if (PrettierDaemon *daemon = m_daemons.value(root)) {
        return daemon;
    }
    const QString node = nodeExecutable();
    if (node.isEmpty()) {
        *error = QCoreApplication::translate("Prettier", "Node.js not found: set its path in Settings (Ctrl+,), section [format] node=");
        return nullptr;
    }
    auto *daemon = new PrettierDaemon(root, node, qApp);
    m_daemons.insert(root, daemon);
    return daemon;
}

QJsonObject Formatter::requestFor(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot) const
{
    return QJsonObject{
        {QStringLiteral("file"), file},
        {QStringLiteral("text"), text},
        {QStringLiteral("cursor"), cursorOffset},
        {QStringLiteral("requireConfig"), vscodeSetting(projectRoot, QStringLiteral("prettier.requireConfig")).toBool(false)},
    };
}

void Formatter::warmUp(const QString &file, const QString &text, const QString &projectRoot)
{
    if (!formatOnSave(projectRoot)) {
        return;
    }
    QString error;
    PrettierDaemon *daemon = daemonFor(file, &error);
    if (!daemon || !daemon->fatalError().isEmpty()) {
        return;
    }
    const int id = daemon->request(requestFor(file, text, -1, projectRoot));
    if (id > 0) {
        daemon->discard(id);
    }
}

FormatResult Formatter::format(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot, int timeoutMs)
{
    QString error;
    PrettierDaemon *daemon = daemonFor(file, &error);
    if (!daemon) {
        return error.isEmpty() ? FormatResult{} : FormatResult{FormatResult::Failed, {}, -1, error};
    }
    const int id = daemon->request(requestFor(file, text, cursorOffset, projectRoot));
    const std::optional<QJsonObject> answer = id > 0 ? daemon->wait(id, timeoutMs) : std::nullopt;
    if (!answer) {
        if (!daemon->fatalError().isEmpty()) {
            return {FormatResult::Failed, {}, -1, QCoreApplication::translate("Prettier", "Prettier failed to start: %1").arg(daemon->fatalError())};
        }
        return {FormatResult::Failed, {}, -1, QCoreApplication::translate("Prettier", "Prettier didn't respond in time: file saved without formatting.")};
    }
    const QString status = answer->value(QLatin1String("status")).toString();
    if (status == QLatin1String("formatted")) {
        return {FormatResult::Formatted,
                answer->value(QLatin1String("text")).toString(),
                answer->value(QLatin1String("cursor")).toInt(-1),
                {}};
    }
    if (status == QLatin1String("unchanged")) {
        return {FormatResult::Unchanged, {}, -1, {}};
    }
    if (status == QLatin1String("skipped")) {
        return {FormatResult::Skipped, {}, -1, answer->value(QLatin1String("message")).toString()};
    }
    return {FormatResult::Failed, {}, -1, firstLine(answer->value(QLatin1String("message")).toString())};
}
}
