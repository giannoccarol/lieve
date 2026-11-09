#include "prettier.h"

#include "platform.h"

#include "jsonc.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>
#include <utility>

namespace Lieve
{
namespace
{
// Runs inside node: loads the project's own Prettier (and ESLint) once, then
// answers format requests (JSON lines on stdin -> JSON lines on stdout).
// A request runs ESLint's fixes, then Prettier on the result, like VS Code.
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
const { Worker } = await import('node:worker_threads');
const errorText = (e) => String(e?.message ?? e);

let prettier;
const ignorePath = path.join(root, '.prettierignore');
async function format(req, text, cursor) {
  try {
    prettier ??= import(pathToFileURL(createRequire(path.join(root, 'package.json')).resolve('prettier')).href).then((m) => m.default ?? m);
    const api = await prettier;
    const info = await api.getFileInfo(req.file, { ignorePath, resolveConfig: true });
    if (info.ignored) return { status: 'skipped', message: 'ignored' };
    if (!info.inferredParser) return { status: 'skipped', message: 'unsupported' };
    if (req.requireConfig && !(await api.resolveConfigFile(req.file))) return { status: 'skipped', message: 'no-config' };
    const options = (await api.resolveConfig(req.file, { editorconfig: true })) ?? {};
    const result = await api.formatWithCursor(text, { ...options, filepath: req.file, cursorOffset: Math.max(cursor, 0) });
    if (result.formatted === text) return { status: 'unchanged' };
    return { status: 'formatted', text: result.formatted, cursor: cursor >= 0 ? result.cursorOffset : -1 };
  } catch (e) {
    return { status: 'error', message: errorText(e) };
  }
}

// ESLint runs in a worker thread: its first lint can take seconds (config,
// plugins, typed rules), and meanwhile Prettier keeps answering.
async function eslintWorker() {
  const { parentPort } = await import('node:worker_threads');
  const fs = await import('node:fs');
  const path = await import('node:path');
  const { createRequire } = await import('node:module');
  const { pathToFileURL } = await import('node:url');
  const names = ['js', 'mjs', 'cjs', 'ts', 'mts', 'cts'].map((ext) => 'eslint.config.' + ext);
  const engines = new Map(); // working directory -> { stamp, engine }
  function configFile(file) {
    for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
      const found = names.map((name) => path.join(dir, name)).find((f) => fs.existsSync(f));
      if (found || path.dirname(dir) === dir) return found;
    }
  }
  function engine(req) {
    // ESLint looks for the flat config in its working directory: the folder
    // of the file's nearest one (else of node_modules/eslint).
    const config = configFile(req.file);
    const cwd = config ? path.dirname(config) : req.eslintRoot;
    const stamp = config ? fs.statSync(config).mtimeMs : 0; // config edited: load it again
    let cached = engines.get(cwd);
    if (!cached || cached.stamp !== stamp) {
      cached = { stamp, engine: (async () => {
        const mod = await import(pathToFileURL(createRequire(path.join(req.eslintRoot, 'package.json')).resolve('eslint')).href);
        const api = mod.default ?? mod;
        const ESLint = api.loadESLint ? await api.loadESLint({ useFlatConfig: req.useFlatConfig, cwd }) : api.ESLint;
        return new ESLint({ cwd, fix: true });
      })() };
      engines.set(cwd, cached);
    }
    return cached.engine;
  }
  parentPort.on('message', async (req) => {
    let answer;
    try {
      const eslint = await engine(req);
      if (await eslint.isPathIgnored(req.file)) {
        answer = { status: 'skipped', message: 'ignored' };
      } else {
        const [result] = await eslint.lintText(req.text, { filePath: req.file });
        const output = result?.output;
        answer = output !== undefined && output !== req.text ? { status: 'fixed', text: output } : { status: 'unchanged' };
      }
    } catch (e) {
      // No configuration at all (ESLint only installed as a dependency): nothing to fix.
      answer = ['config-file-missing', 'no-config-found'].includes(e?.messageTemplate)
        ? { status: 'skipped', message: 'no-config' }
        : { status: 'error', message: String(e?.message ?? e) };
    }
    parentPort.postMessage({ id: req.id, ...answer });
    clearTimeout(globalThis.lieveGc); // as in the main thread
    globalThis.lieveGc = setTimeout(() => globalThis.gc?.(), 300);
  });
}
let worker;
let lintId = 0;
const linting = new Map(); // id -> finish(answer)
function lint(req) {
  if (!worker) {
    const w = new Worker('(' + eslintWorker + ')()', { eval: true, stdout: true });
    let failure = 'ESLint stopped';
    w.stdout.resume(); // plugin chatter: never on our stdout
    w.on('message', ({ id, ...answer }) => linting.get(id)?.(answer));
    w.on('error', (e) => { failure = errorText(e); });
    w.on('exit', () => {
      if (worker === w) worker = undefined;
      for (const finish of [...linting.values()]) finish({ status: 'error', message: failure });
    });
    worker = w;
  }
  const id = ++lintId;
  return new Promise((resolve) => {
    let timer;
    const finish = (answer) => { clearTimeout(timer); linting.delete(id); resolve(answer); };
    timer = setTimeout(() => finish({ status: 'timeout' }), req.eslintTimeout);
    linting.set(id, finish);
    worker.postMessage({ id, file: req.file, text: req.text, eslintRoot: req.eslintRoot, useFlatConfig: req.useFlatConfig });
  });
}

// An offset in `a` moved to `b`: the same before the first change, shifted
// after the last one; -1 in between.
function around(a, b, offset) {
  let p = 0;
  while (p < a.length && p < b.length && a[p] === b[p]) p++;
  if (offset <= p) return offset;
  let s = 0;
  while (s < a.length - p && s < b.length - p && a[a.length - 1 - s] === b[b.length - 1 - s]) s++;
  return offset >= a.length - s ? offset + b.length - a.length : -1;
}
// Where the cursor goes after ESLint's fixes: the same place in the lines
// they left alone (found with a quick line diff), else on the changed line.
function mapCursor(a, b, offset) {
  const moved = around(a, b, offset);
  if (moved >= 0) return moved;
  const A = a.split('\n');
  const B = b.split('\n');
  const line = a.slice(0, offset).split('\n').length - 1;
  const column = offset - (a.lastIndexOf('\n', offset - 1) + 1);
  let i = 0;
  let j = 0;
  let to = -1;
  while (to < 0 && i < A.length && j < B.length) {
    if (A[i] === B[j]) {
      if (i === line) to = j;
      i++;
      j++;
      continue;
    }
    // Changed lines: go on from the nearest pair of equal lines.
    let x = -1;
    let y = -1;
    for (let k = 1; x < 0 && k < 400; k++) {
      for (let d = 0; d <= k; d++) {
        if (i + d < A.length && A[i + d] === B[j + k - d]) {
          x = d;
          y = k - d;
          break;
        }
      }
    }
    if (x < 0) break;
    if (line < i + x) to = j + Math.max(0, Math.min(line - i, y - 1));
    i += x;
    j += y;
  }
  if (to < 0) to = Math.min(j + Math.max(0, line - i), B.length - 1);
  let start = 0;
  for (let n = 0; n < to; n++) start += B[n].length + 1;
  const inLine = around(A[line], B[to], column);
  return start + Math.min(inLine >= 0 ? inLine : column, B[to].length);
}

async function handle(req) {
  const fixes = req.eslint ? lint(req) : undefined;
  // Prettier formats the text meanwhile: that is the result when ESLint has
  // nothing to fix, as usual.
  const formatted = req.prettier ? format(req, req.text, req.cursor) : undefined;
  const answer = { cursor: req.cursor };
  let text = req.text;
  if (fixes) {
    const { text: fixed, ...status } = await fixes;
    answer.eslint = status;
    if (fixed !== undefined) {
      answer.cursor = req.cursor >= 0 ? mapCursor(text, fixed, req.cursor) : -1;
      text = fixed;
    }
  }
  if (formatted) {
    const { text: pretty, cursor, ...status } = text === req.text ? await formatted : await format(req, text, answer.cursor);
    answer.prettier = status;
    if (pretty !== undefined) {
      text = pretty;
      answer.cursor = cursor;
    }
  }
  if (text !== req.text) answer.text = text;
  return answer;
}
readline.createInterface({ input: process.stdin, crlfDelay: Infinity })
  .on('line', async (line) => {
    let req;
    try { req = JSON.parse(line); } catch { return; }
    let answer;
    try { answer = await handle(req); } catch (e) { answer = { prettier: { status: 'error', message: errorText(e) } }; }
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

// The VS Code language id of a file ESLint may check (for eslint.validate).
QString eslintLanguage(const QString &file)
{
    constexpr std::pair<const char *, const char *> kLanguages[] = {
        {"js", "javascript"},  {"mjs", "javascript"},   {"cjs", "javascript"},    {"jsx", "javascriptreact"},
        {"ts", "typescript"},  {"mts", "typescript"},   {"cts", "typescript"},    {"tsx", "typescriptreact"},
        {"html", "html"},      {"htm", "html"},         {"vue", "vue"},           {"svelte", "svelte"},
        {"astro", "astro"},    {"md", "markdown"},      {"mdx", "mdx"},           {"json", "json"},
        {"jsonc", "jsonc"},    {"json5", "json5"},      {"css", "css"},           {"gjs", "glimmer-js"},
        {"gts", "glimmer-ts"},
    };
    const QString suffix = QFileInfo(file).suffix().toLower();
    for (const auto &[extension, language] : kLanguages) {
        if (suffix == QLatin1String(extension)) {
            return QLatin1String(language);
        }
    }
    return {};
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
    // project; ESLint's worker adds about 200), formatting just as fast.
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

bool Formatter::fixOnSave(const QString &projectRoot) const
{
    const QString mode = config().value(QStringLiteral("format/eslint"), QStringLiteral("auto")).toString().trimmed().toLower();
    if (mode == QLatin1String("true") || mode == QLatin1String("on")) {
        return true;
    }
    if (mode == QLatin1String("false") || mode == QLatin1String("off")) {
        return false;
    }
    if (vscodeSetting(projectRoot, QStringLiteral("eslint.enable")) == QJsonValue(false)) {
        return false;
    }
    // {"source.fixAll.eslint": "explicit"} (or true, "always"), or the older
    // list form; "source.fixAll" includes ESLint's. VS Code merges the
    // project's object with yours: the project's entries come first.
    const QString key = QStringLiteral("editor.codeActionsOnSave");
    const QJsonValue project = projectRoot.isEmpty() ? QJsonValue() : readJsonc(projectRoot + QStringLiteral("/.vscode/settings.json")).value(key);
    for (const QJsonValue &actions : {project, vscodeUserSettings().value(key)}) {
        for (const QString &kind : {QStringLiteral("source.fixAll.eslint"), QStringLiteral("source.fixAll")}) {
            if (actions.isArray() && actions.toArray().contains(kind)) {
                return true;
            }
            const QJsonValue value = actions.toObject().value(kind);
            if (value.isBool()) {
                return value.toBool();
            }
            if (value.isString()) {
                return value.toString() == QLatin1String("explicit") || value.toString() == QLatin1String("always");
            }
        }
    }
    return false;
}

QString Formatter::packageRoot(const QString &file, const QString &package) const
{
    const QString manifest = QStringLiteral("node_modules/%1/package.json").arg(package);
    QDir dir = QFileInfo(file).absoluteDir();
    do {
        if (QFileInfo::exists(dir.filePath(manifest))) {
            return dir.absolutePath();
        }
    } while (dir.cdUp());
    return {};
}

bool Formatter::available(const QString &file) const
{
    return !packageRoot(file, QStringLiteral("prettier")).isEmpty();
}

bool Formatter::eslintAvailable(const QString &file, const QString &projectRoot) const
{
    const QString language = eslintLanguage(file);
    if (language.isEmpty() || packageRoot(file, QStringLiteral("eslint")).isEmpty()) {
        return false;
    }
    // "eslint.validate" lists the languages to check; without it ESLint's
    // own configuration decides.
    const QJsonValue validate = vscodeSetting(projectRoot, QStringLiteral("eslint.validate"));
    if (!validate.isArray()) {
        return true;
    }
    const QJsonArray languages = validate.toArray();
    return std::any_of(languages.begin(), languages.end(), [&language](const QJsonValue &entry) {
        return (entry.isObject() ? entry.toObject().value(QLatin1String("language")) : entry).toString() == language;
    });
}

FormatSteps Formatter::steps(const QString &file, const QString &projectRoot, bool onSave) const
{
    FormatSteps steps;
    steps.eslint = fixOnSave(projectRoot) && eslintAvailable(file, projectRoot);
    steps.prettier = (!onSave || formatOnSave(projectRoot)) && available(file);
    return steps;
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
                                           QStringList{home + QStringLiteral("/.local/share/mise/shims"),
                                                       home + QStringLiteral("/.volta/bin"),
                                                       home + QStringLiteral("/.local/share/fnm/aliases/default/bin"),
                                                       home + QStringLiteral("/.local/bin"),
                                                       QStringLiteral("/usr/local/bin"),
                                                       QStringLiteral("/usr/bin")}
                                               + Platform::nodeBinDirs());
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
    QString root = packageRoot(file, QStringLiteral("prettier"));
    if (root.isEmpty()) {
        root = packageRoot(file, QStringLiteral("eslint"));
    }
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

QJsonObject Formatter::requestFor(const QString &file,
                                  const QString &text,
                                  int cursorOffset,
                                  const QString &projectRoot,
                                  FormatSteps steps,
                                  int eslintTimeoutMs) const
{
    QJsonObject request{
        {QStringLiteral("file"), file},
        {QStringLiteral("text"), text},
        {QStringLiteral("cursor"), cursorOffset},
        {QStringLiteral("prettier"), steps.prettier},
        {QStringLiteral("requireConfig"), vscodeSetting(projectRoot, QStringLiteral("prettier.requireConfig")).toBool(false)},
        {QStringLiteral("eslint"), steps.eslint},
    };
    if (steps.eslint) {
        request.insert(QStringLiteral("eslintRoot"), packageRoot(file, QStringLiteral("eslint")));
        request.insert(QStringLiteral("eslintTimeout"), eslintTimeoutMs);
        // Flat config (true) or .eslintrc (false); unset: ESLint's default.
        const QJsonValue flat = vscodeSetting(projectRoot, QStringLiteral("eslint.useFlatConfig"));
        if (flat.isBool()) {
            request.insert(QStringLiteral("useFlatConfig"), flat);
        }
    }
    return request;
}

void Formatter::warmUp(const QString &file, const QString &text, const QString &projectRoot)
{
    const FormatSteps onSave = steps(file, projectRoot, true);
    if (!onSave.eslint && !onSave.prettier) {
        return;
    }
    QString error;
    PrettierDaemon *daemon = daemonFor(file, &error);
    if (!daemon || !daemon->fatalError().isEmpty()) {
        return;
    }
    const int id = daemon->request(requestFor(file, text, -1, projectRoot, onSave, 60000));
    if (id > 0) {
        daemon->discard(id);
    }
}

FormatResult Formatter::format(const QString &file, const QString &text, int cursorOffset, const QString &projectRoot, FormatSteps steps, int timeoutMs)
{
    FormatResult result;
    if (!steps.eslint && !steps.prettier) {
        return result;
    }
    // A failure before any answer is Prettier's, or ESLint's when it runs alone.
    const auto failed = [&result, steps](const QString &message) {
        if (steps.prettier) {
            result.status = FormatResult::Failed;
            result.message = message;
        } else {
            result.eslint = FormatResult::Failed;
            result.eslintMessage = message;
        }
        return result;
    };
    QString error;
    PrettierDaemon *daemon = daemonFor(file, &error);
    if (!daemon) {
        return error.isEmpty() ? result : failed(error);
    }
    // ESLint gets most of the time: what is left is enough for Prettier.
    const int id = daemon->request(requestFor(file, text, cursorOffset, projectRoot, steps, std::max(500, timeoutMs - 1500)));
    const std::optional<QJsonObject> answer = id > 0 ? daemon->wait(id, timeoutMs) : std::nullopt;
    if (!answer) {
        if (!daemon->fatalError().isEmpty()) {
            return failed(steps.prettier ? QCoreApplication::translate("Prettier", "Prettier failed to start: %1").arg(daemon->fatalError())
                                         : QCoreApplication::translate("Prettier", "ESLint failed to start: %1").arg(daemon->fatalError()));
        }
        return failed(steps.prettier ? QCoreApplication::translate("Prettier", "Prettier didn't respond in time: file saved without formatting.")
                                     : QCoreApplication::translate("Prettier", "ESLint didn't respond in time: its fixes were not applied."));
    }
    if (answer->contains(QLatin1String("text"))) {
        result.text = answer->value(QLatin1String("text")).toString();
        result.cursor = answer->value(QLatin1String("cursor")).toInt(-1);
    }
    const QJsonObject prettier = answer->value(QLatin1String("prettier")).toObject();
    const QString status = prettier.value(QLatin1String("status")).toString();
    if (status == QLatin1String("formatted")) {
        result.status = FormatResult::Formatted;
    } else if (status == QLatin1String("unchanged")) {
        result.status = FormatResult::Unchanged;
    } else if (status == QLatin1String("skipped")) {
        result.message = prettier.value(QLatin1String("message")).toString();
    } else if (status == QLatin1String("error")) {
        result.status = FormatResult::Failed;
        result.message = firstLine(prettier.value(QLatin1String("message")).toString());
    }
    const QJsonObject eslint = answer->value(QLatin1String("eslint")).toObject();
    const QString fixes = eslint.value(QLatin1String("status")).toString();
    if (fixes == QLatin1String("fixed")) {
        result.eslint = FormatResult::Formatted;
    } else if (fixes == QLatin1String("unchanged")) {
        result.eslint = FormatResult::Unchanged;
    } else if (fixes == QLatin1String("timeout")) {
        result.eslint = FormatResult::Failed;
        result.eslintMessage = QCoreApplication::translate("Prettier", "ESLint didn't respond in time: its fixes were not applied.");
    } else if (fixes == QLatin1String("error")) {
        result.eslint = FormatResult::Failed;
        result.eslintMessage = QStringLiteral("ESLint: %1").arg(firstLine(eslint.value(QLatin1String("message")).toString()));
    }
    return result;
}
}
