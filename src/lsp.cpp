#include "lsp.h"

#include "lspclient.h"
#include "lspcompletion.h"
#include "mainwindow.h"
#include "prettier.h"
#include "settings.h"
#include "statusbar.h"
#include "symbolpicker.h"
#include "theme.h"

#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/Message>
#include <KTextEditor/MovingRange>
#include <KTextEditor/TextHintInterface>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTextDocument>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>

#include <algorithm>
#include <numeric>

#include <signal.h>

namespace Lieve
{
// The hover text of every view: problems at once, the server's answer when it comes.
class LspHints : public KTextEditor::TextHintProvider
{
public:
    explicit LspHints(LspManager *manager)
        : m_manager(manager)
    {
    }
    QString textHint(KTextEditor::View *view, const KTextEditor::Cursor &position) override
    {
        return m_manager->textHint(view, position);
    }

private:
    LspManager *m_manager;
};

namespace
{
constexpr int kMaxHistory = 50;
constexpr qsizetype kMaxDocumentSize = 3 * 1024 * 1024; // characters

QString localPath(const KTextEditor::Document *doc)
{
    return doc->url().isLocalFile() ? doc->url().toLocalFile() : QString();
}

QString uriFor(const QString &path)
{
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

bool isAngularProject(const QString &root)
{
    return QFileInfo::exists(root + QStringLiteral("/angular.json")) || QFileInfo::exists(root + QStringLiteral("/node_modules/@angular/core/package.json"));
}

// Language id and server for a file of the project ("" when none).
QPair<QString, QString> languageFor(const QString &path, const QString &root)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("ts") || suffix == QLatin1String("mts") || suffix == QLatin1String("cts")) {
        return {QStringLiteral("typescript"), QStringLiteral("typescript")};
    }
    if (suffix == QLatin1String("tsx")) {
        return {QStringLiteral("typescriptreact"), QStringLiteral("typescript")};
    }
    if (suffix == QLatin1String("js") || suffix == QLatin1String("mjs") || suffix == QLatin1String("cjs")) {
        return {QStringLiteral("javascript"), QStringLiteral("typescript")};
    }
    if (suffix == QLatin1String("jsx")) {
        return {QStringLiteral("javascriptreact"), QStringLiteral("typescript")};
    }
    if (suffix == QLatin1String("html") && isAngularProject(root)) {
        return {QStringLiteral("html"), QStringLiteral("angular")};
    }
    return {};
}

// typescript-language-server needs tsserver: the project's TypeScript, or
// one installed next to the server.
bool hasTsserver(const QString &root, const QString &script)
{
    QDir dir(root);
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("node_modules/typescript/lib/tsserver.js")))) {
            return true;
        }
    } while (dir.cdUp() && !dir.isRoot());
    if (script.isEmpty()) {
        return true; // a native program: let it decide
    }
    dir = QFileInfo(script).absoluteDir();
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("node_modules/typescript/lib/tsserver.js")))
            || (dir.dirName() == QLatin1String("node_modules") && QFileInfo::exists(dir.filePath(QStringLiteral("typescript/lib/tsserver.js"))))) {
            return true;
        }
    } while (dir.cdUp() && !dir.isRoot());
    return false;
}

QString packageVersion(const QString &packageJson)
{
    QFile file(packageJson);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("version")).toString();
}

// The folder of the npm package that holds `script` (has a package.json named `name`).
QString packageDir(const QString &script, const QString &name)
{
    QDir dir = QFileInfo(script).absoluteDir();
    do {
        QFile file(dir.filePath(QStringLiteral("package.json")));
        if (file.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("name")).toString() == name) {
            return dir.absolutePath();
        }
    } while (dir.cdUp() && !dir.isRoot());
    return {};
}

KTextEditor::Range lspRange(const QJsonObject &range)
{
    const QJsonObject start = range.value(QLatin1String("start")).toObject();
    const QJsonObject end = range.value(QLatin1String("end")).toObject();
    return KTextEditor::Range(start.value(QLatin1String("line")).toInt(),
                              start.value(QLatin1String("character")).toInt(),
                              end.value(QLatin1String("line")).toInt(),
                              end.value(QLatin1String("character")).toInt());
}

QJsonObject lspPosition(KTextEditor::Cursor c)
{
    return QJsonObject{{QStringLiteral("line"), c.line()}, {QStringLiteral("character"), c.column()}};
}

// Line and column of `offset` in `text` (lines joined with '\n').
KTextEditor::Cursor positionOf(QStringView text, qsizetype offset)
{
    const QStringView before = text.left(offset);
    const int line = int(before.count(QLatin1Char('\n')));
    const qsizetype lineStart = before.lastIndexOf(QLatin1Char('\n')) + 1;
    return KTextEditor::Cursor(line, int(offset - lineStart));
}

KTextEditor::Range clampedRange(KTextEditor::Document *doc, KTextEditor::Range r)
{
    const int lastLine = std::max(0, doc->lines() - 1);
    const auto clamp = [&](KTextEditor::Cursor c) {
        if (c.line() > lastLine) {
            return KTextEditor::Cursor(lastLine, doc->lineLength(lastLine));
        }
        return KTextEditor::Cursor(std::max(0, c.line()), std::clamp(c.column(), 0, doc->lineLength(std::max(0, c.line()))));
    };
    r = KTextEditor::Range(clamp(r.start()), clamp(r.end()));
    if (r.isEmpty()) {
        // An empty range (e.g. "';' expected" at the end of a line): underline a character.
        const int length = doc->lineLength(r.start().line());
        if (r.start().column() < length) {
            r.setEnd(KTextEditor::Cursor(r.start().line(), r.start().column() + 1));
        } else if (r.start().column() > 0) {
            r.setStart(KTextEditor::Cursor(r.start().line(), r.start().column() - 1));
        }
    }
    return r;
}

QColor severityColor(int severity)
{
    const bool dark = Theme::colors().dark;
    switch (severity) {
    case 1:
        return dark ? QColor(0xf1, 0x4c, 0x4c) : QColor(0xe5, 0x14, 0x00);
    case 2:
        return dark ? QColor(0xcc, 0xa7, 0x00) : QColor(0xbf, 0x88, 0x03);
    default:
        return dark ? QColor(0x37, 0x94, 0xff) : QColor(0x1a, 0x85, 0xff);
    }
}

QString severityMark(int severity)
{
    return severity == 1 ? QStringLiteral("⊗") : severity == 2 ? QStringLiteral("⚠") : QStringLiteral("ⓘ");
}

KTextEditor::Attribute::Ptr diagnosticAttribute(int severity, bool unnecessary, bool deprecated)
{
    KTextEditor::Attribute::Ptr a(new KTextEditor::Attribute());
    if (severity <= 3) {
        a->setUnderlineStyle(QTextCharFormat::WaveUnderline);
        a->setUnderlineColor(severityColor(severity));
    }
    if (unnecessary) { // unused code is faded, like VS Code
        QColor faded = Theme::colors().dim;
        if (faded.isValid()) {
            a->setForeground(faded);
        }
    }
    if (deprecated) {
        a->setFontStrikeOut(true);
    }
    return a;
}

// Hover text: markdown (or plain text, or the old MarkedString forms) as markdown.
QString hoverMarkdown(const QJsonValue &contents)
{
    if (contents.isString()) {
        return contents.toString();
    }
    if (contents.isArray()) {
        QStringList parts;
        const QJsonArray array = contents.toArray();
        for (const QJsonValue &v : array) {
            parts << hoverMarkdown(v);
        }
        parts.removeAll(QString());
        return parts.join(QStringLiteral("\n\n"));
    }
    const QJsonObject o = contents.toObject();
    const QString value = o.value(QLatin1String("value")).toString();
    if (o.contains(QLatin1String("language"))) {
        return QStringLiteral("```%1\n%2\n```").arg(o.value(QLatin1String("language")).toString(), value);
    }
    if (o.value(QLatin1String("kind")).toString() == QLatin1String("plaintext")) {
        return QStringLiteral("```\n%1\n```").arg(value);
    }
    return value;
}

QString markdownToHtml(QString markdown)
{
    if (markdown.size() > 4000) {
        markdown = markdown.left(4000) + QStringLiteral("…");
    }
    QTextDocument doc;
    doc.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    QString html = doc.toHtml();
    // Only the body: the tooltip uses its own font and colours.
    const qsizetype bodyStart = html.indexOf(QLatin1String("<body"));
    const qsizetype bodyOpenEnd = bodyStart >= 0 ? html.indexOf(QLatin1Char('>'), bodyStart) : -1;
    const qsizetype bodyEnd = html.lastIndexOf(QLatin1String("</body>"));
    if (bodyOpenEnd >= 0 && bodyEnd > bodyOpenEnd) {
        html = html.mid(bodyOpenEnd + 1, bodyEnd - bodyOpenEnd - 1);
    }
    return html.trimmed();
}

void postMessage(KTextEditor::View *view, const QString &text)
{
    auto *message = new KTextEditor::Message(QStringLiteral("<p>%1</p>").arg(text.toHtmlEscaped()), KTextEditor::Message::Information);
    message->setPosition(KTextEditor::Message::BottomInView);
    message->setAutoHide(3000);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    message->setView(view);
    view->document()->postMessage(message);
}

QString iconName(int symbolKind)
{
    switch (symbolKind) {
    case 5: // class
    case 11: // interface
    case 23: // struct
        return QStringLiteral("code-class");
    case 6: // method
    case 9: // constructor
    case 12: // function
    case 24: // event
    case 25: // operator
        return QStringLiteral("code-function");
    case 10: // enum
    case 22: // enum member
        return QStringLiteral("enum");
    case 26: // type parameter
        return QStringLiteral("code-typedef");
    case 2: // module
    case 3: // namespace
    case 4: // package
        return QStringLiteral("code-context");
    case 1: // file
        return QStringLiteral("text-x-generic");
    default: // variables, properties, constants, values
        return QStringLiteral("code-variable");
    }
}
}

QIcon symbolKindIcon(int kind)
{
    return QIcon::fromTheme(iconName(kind));
}

QIcon completionKindIcon(int kind)
{
    // CompletionItemKind -> the nearest SymbolKind
    static const int toSymbol[] = {0, 15, 6, 12, 9, 8, 13, 5, 11, 2, 7, 16, 15, 10, 0, 0, 15, 1, 1, 1, 22, 14, 23, 24, 25, 26};
    if (kind == 14 || kind == 15) { // keyword, snippet
        return QIcon::fromTheme(QStringLiteral("format-text-code"));
    }
    return symbolKindIcon(kind > 0 && kind < int(std::size(toSymbol)) ? toSymbol[kind] : 13);
}

// ---------------------------------------------------------------------------

LspManager::LspManager(MainWindow *window, StatusBar *status, QWidget *overlayParent)
    : QObject(window)
    , m_window(window)
    , m_status(status)
{
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(250);
    connect(&m_syncTimer, &QTimer::timeout, this, &LspManager::flushAll);
    m_completion = new LspCompletionModel(this);
    m_hints = std::make_unique<LspHints>(this);

    m_picker = new SymbolPicker(overlayParent);
    connect(m_picker, &SymbolPicker::highlighted, this, [this](int index) {
        if (m_pickerPreview && m_pickerView && index >= 0 && index < m_pickerTargets.size()) {
            m_pickerView->setCursorPosition(m_pickerTargets.at(index).cursor);
        }
    });
    connect(m_picker, &SymbolPicker::chosen, this, [this](int index) {
        if (index < 0 || index >= m_pickerTargets.size()) {
            return;
        }
        const Location target = m_pickerTargets.at(index);
        if (m_pickerView && m_pickerPreview) {
            m_pickerView->setCursorPosition(m_pickerOrigin); // so Alt+Left comes back here
        }
        jumpTo(target.path, target.cursor, true);
    });
    connect(m_picker, &SymbolPicker::cancelled, this, [this] {
        if (m_pickerView) {
            if (m_pickerPreview) {
                m_pickerView->setCursorPosition(m_pickerOrigin);
            }
            m_pickerView->setFocus();
        }
    });

    // In the editor's context menu; F12 itself is the window's shortcut.
    m_definitionAction = new QAction(tr("Go to Definition"), this);
    m_definitionAction->setShortcut(QKeySequence(Qt::Key_F12));
    m_definitionAction->setShortcutContext(Qt::WidgetShortcut);
    m_definitionAction->setShortcutVisibleInContextMenu(true);
    connect(m_definitionAction, &QAction::triggered, this, [this] {
        // Where the menu was opened, else at the cursor.
        if (m_menuView && m_menuCursor.isValid()) {
            goToDefinition(m_menuView, m_menuCursor);
        } else if (m_current) {
            goToDefinition(m_current, m_current->cursorPosition());
        }
    });
}

LspManager::~LspManager()
{
    m_link.reset();
    for (const QPointer<KTextEditor::View> &view : std::as_const(m_views)) {
        if (view) {
            view->unregisterTextHintProvider(m_hints.get());
            view->unregisterCompletionModel(m_completion);
            if (QWidget *inner = view->focusProxy()) {
                inner->removeEventFilter(this);
            }
        }
    }
    m_views.clear();
    for (auto &[doc, s] : m_docs) {
        doc->disconnect(this);
        clearDiagnostics(s.get());
        releaseRevisions(s.get(), INT_MAX);
    }
    m_docs.clear();
    stopAll(true);
}

QList<LspManager::Shortcut> LspManager::shortcuts()
{
    return {
        {tr("Go to Definition"),
         {QKeySequence(Qt::Key_F12)},
         [this] {
             if (m_current) {
                 goToDefinition(m_current, m_current->cursorPosition());
             }
         }},
        {tr("Go Back"),
         {QKeySequence(Qt::ALT | Qt::Key_Left)},
         [this] {
             navigate(-1);
         }},
        {tr("Go Forward"),
         {QKeySequence(Qt::ALT | Qt::Key_Right)},
         [this] {
             navigate(1);
         }},
        {tr("Go to Symbol in File"),
         {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
         [this] {
             showDocumentSymbols(m_current);
         }},
        {tr("Next Problem"),
         {QKeySequence(Qt::Key_F8)},
         [this] {
             nextProblem(m_current, true);
         }},
        {tr("Previous Problem"),
         {QKeySequence(Qt::SHIFT | Qt::Key_F8)},
         [this] {
             nextProblem(m_current, false);
         }},
    };
}

// ---------------------------------------------------------------------------
// Servers

void LspManager::setRoot(const QString &root)
{
    if (root == m_root) {
        return;
    }
    stopAll();
    m_root = root;
    m_back.clear();
    m_forward.clear();
    for (auto &[doc, s] : m_docs) {
        detach(s.get());
        attach(doc);
    }
    updateStatus();
}

void LspManager::stopAll(bool wait)
{
    const QHash<QString, LspClient *> clients = std::exchange(m_clients, {});
    for (LspClient *client : clients) {
        client->disconnect(this);
        client->stop(wait);
        client->deleteLater();
    }
    qDeleteAll(m_idle);
    m_idle.clear();
    m_progress.clear();
    for (auto &[doc, s] : m_docs) {
        s->open = false;
        clearDiagnostics(s.get());
        releaseRevisions(s.get(), INT_MAX);
    }
}

LspClient *LspManager::clientFor(const QString &serverId)
{
    if (LspClient *client = m_clients.value(serverId)) {
        return client;
    }
    QSettings &c = config();
    const QString mode = c.value(QStringLiteral("lsp/enabled"), QStringLiteral("auto")).toString().trimmed().toLower();
    if (m_root.isEmpty() || mode == QLatin1String("false") || mode == QLatin1String("off") || mode == QLatin1String("no")) {
        return nullptr;
    }
    const QString setting = c.value(QStringLiteral("lsp/") + serverId).toString().trimmed();
    if (setting == QLatin1String("false") || setting == QLatin1String("off")) {
        return nullptr;
    }

    LspServerSpec spec;
    spec.id = serverId;
    spec.root = m_root;
    std::optional<LspCommand> command;
    if (serverId == QLatin1String("typescript")) {
        command = findLspCommand(setting, QStringLiteral("typescript-language-server"), {QStringLiteral("--stdio")}, m_root);
        if (!command || (setting.isEmpty() && !hasTsserver(m_root, command->script))) {
            return nullptr;
        }
        spec.name = QStringLiteral("TypeScript");
        // Lighter tsserver: one process instead of two (no separate syntax
        // server), no typings installer, and a memory cap.
        spec.initializationOptions = QJsonObject{
            {QStringLiteral("hostInfo"), QStringLiteral("lieve")},
            {QStringLiteral("disableAutomaticTypingAcquisition"), true},
            {QStringLiteral("maxTsServerMemory"), std::max(512, c.value(QStringLiteral("lsp/maxTsServerMemory"), 3072).toInt())},
            {QStringLiteral("tsserver"), QJsonObject{{QStringLiteral("useSyntaxServer"), QStringLiteral("never")}}},
            {QStringLiteral("preferences"),
             QJsonObject{{QStringLiteral("includeCompletionsForModuleExports"), true}, {QStringLiteral("includeCompletionsWithInsertText"), true}}},
        };
    } else if (serverId == QLatin1String("angular")) {
        command = findLspCommand(setting, QStringLiteral("ngserver"), {}, m_root);
        if (!command) {
            return nullptr;
        }
        if (command->arguments.isEmpty()) {
            // TypeScript and the Angular language service: the project's
            // first, else the ones that came with the server.
            QStringList probes{m_root};
            const QString own = command->script.isEmpty() ? QString() : packageDir(command->script, QStringLiteral("@angular/language-server"));
            if (!own.isEmpty()) {
                probes << own;
            }
            command->arguments << QStringLiteral("--stdio") << QStringLiteral("--tsProbeLocations") << probes.join(QLatin1Char(','))
                               << QStringLiteral("--ngProbeLocations") << probes.join(QLatin1Char(','));
            const QString core = packageVersion(m_root + QStringLiteral("/node_modules/@angular/core/package.json"));
            if (!core.isEmpty()) {
                command->arguments << QStringLiteral("--angularCoreVersion") << core;
            }
        }
        spec.name = QStringLiteral("Angular");
    } else {
        return nullptr;
    }
    spec.program = command->program;
    spec.arguments = command->processArguments();
    spec.environment = QProcessEnvironment::systemEnvironment();
    const QString node = findNode();
    if (!node.isEmpty()) { // so "#!/usr/bin/env node" helpers work with a minimal PATH too
        spec.environment.insert(QStringLiteral("PATH"), QFileInfo(node).absolutePath() + QLatin1Char(':') + spec.environment.value(QStringLiteral("PATH")));
    }

    auto *client = new LspClient(spec, this);
    m_clients.insert(serverId, client);
    connect(client, &LspClient::ready, this, [this, client] {
        serverReady(client);
    });
    connect(client, &LspClient::notificationReceived, this, [this, client](const QString &method, const QJsonObject &params) {
        serverNotification(client, method, params);
    });
    connect(client, &LspClient::stateChanged, this, [this, client] {
        if (client->state() != LspClient::State::Running) {
            // Restarting or stopped: the documents are opened again when it is back.
            for (auto &[doc, s] : m_docs) {
                if (s->serverId == client->spec().id) {
                    s->open = false;
                    s->diagnosticsReceived = false;
                    clearDiagnostics(s.get());
                    releaseRevisions(s.get(), INT_MAX);
                }
            }
            m_progress.remove(client);
        }
        updateStatus();
    });
    client->start();
    return client;
}

LspClient *LspManager::readyClient(KTextEditor::Document *doc) const
{
    DocState *s = state(doc);
    if (!s || s->serverId.isEmpty() || !s->open) {
        return nullptr;
    }
    LspClient *client = m_clients.value(s->serverId);
    return client && client->state() == LspClient::State::Running ? client : nullptr;
}

void LspManager::serverReady(LspClient *client)
{
    for (auto &[doc, s] : m_docs) {
        if (s->serverId == client->spec().id && !s->open) {
            sendOpen(s.get(), client);
        }
    }
    updateStatus();
}

void LspManager::serverNotification(LspClient *client, const QString &method, const QJsonObject &params)
{
    if (method == QLatin1String("textDocument/publishDiagnostics")) {
        DocState *s = stateForUri(params.value(QLatin1String("uri")).toString());
        if (s && s->open && s->serverId == client->spec().id) {
            const QJsonValue version = params.value(QLatin1String("version"));
            setDiagnostics(s, params.value(QLatin1String("diagnostics")).toArray(), version.isDouble() ? version.toInt() : -1);
        }
    } else if (method == QLatin1String("workspace/diagnostic/refresh")) {
        for (auto &[doc, s] : m_docs) {
            if (s->open && s->serverId == client->spec().id) {
                pullDiagnostics(s.get(), client);
            }
        }
    } else if (method == QLatin1String("$/progress")) {
        const QJsonObject value = params.value(QLatin1String("value")).toObject();
        const QString kind = value.value(QLatin1String("kind")).toString();
        if (kind == QLatin1String("begin")) {
            m_progress.insert(client, value.value(QLatin1String("title")).toString());
        } else if (kind == QLatin1String("end")) {
            m_progress.remove(client);
        }
        updateStatus();
    }
}

void LspManager::updateIdle()
{
    const int minutes = std::max(0, config().value(QStringLiteral("lsp/idleMinutes"), 5).toInt());
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        const QString id = it.key();
        const bool used = std::any_of(m_docs.begin(), m_docs.end(), [&id](const auto &entry) {
            return entry.second->serverId == id;
        });
        QTimer *timer = m_idle.value(id);
        if (used) {
            if (timer) {
                timer->stop();
            }
            continue;
        }
        if (!timer) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            m_idle.insert(id, timer);
            connect(timer, &QTimer::timeout, this, [this, id] {
                // No file of its language for a while: free its memory.
                if (LspClient *client = m_clients.take(id)) {
                    client->disconnect(this);
                    m_progress.remove(client);
                    client->stop();
                    client->deleteLater();
                }
                delete m_idle.take(id);
                updateStatus();
            });
        }
        if (!timer->isActive()) {
            timer->start(minutes * 60 * 1000);
        }
    }
}

// ---------------------------------------------------------------------------
// Documents

LspManager::DocState *LspManager::state(KTextEditor::Document *doc) const
{
    const auto it = m_docs.find(doc);
    return it == m_docs.end() ? nullptr : it->second.get();
}

LspManager::DocState *LspManager::stateForUri(const QString &uri) const
{
    for (const auto &[doc, s] : m_docs) {
        if (s->uri == uri) {
            return s.get();
        }
    }
    const QString path = QFileInfo(QUrl(uri).toLocalFile()).canonicalFilePath();
    for (const auto &[doc, s] : m_docs) {
        if (!path.isEmpty() && s->path == path) {
            return s.get();
        }
    }
    return nullptr;
}

void LspManager::addDocument(KTextEditor::Document *doc)
{
    if (m_docs.count(doc)) {
        return;
    }
    auto s = std::make_unique<DocState>();
    s->doc = doc;
    m_docs.emplace(doc, std::move(s));
    connect(doc, &KTextEditor::Document::textChanged, this, [this](KTextEditor::Document *d) {
        if (DocState *s = state(d); s && s->open) {
            s->dirty = true;
            m_syncTimer.start();
        }
    });
    connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, [this](KTextEditor::Document *d) {
        DocState *s = state(d);
        LspClient *client = readyClient(d);
        if (!s || !client) {
            return;
        }
        flush(d);
        const QJsonValue sync = client->capabilities().value(QLatin1String("textDocumentSync"));
        const bool includeText = sync.toObject().value(QLatin1String("save")).toObject().value(QLatin1String("includeText")).toBool();
        QJsonObject params{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s->uri}}}};
        if (includeText) {
            params.insert(QStringLiteral("text"), s->synced);
        }
        client->notify(QStringLiteral("textDocument/didSave"), params);
    });
    connect(doc, &KTextEditor::Document::documentUrlChanged, this, [this](KTextEditor::Document *d) {
        // Saved under another name, or renamed: maybe another language now.
        if (DocState *s = state(d); s && localPath(d) != s->path) {
            detach(s);
            attach(d);
            updateIdle();
            updateStatus();
        }
    });
    connect(doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, [this](KTextEditor::Document *d) {
        // Reloaded: the revisions are gone and the underlines with them.
        if (DocState *s = state(d)) {
            clearDiagnostics(s);
            s->revisions.clear();
            if (m_link && m_linkView && m_linkView->document() == d) {
                m_link.reset();
            }
            updateStatus();
        }
    });
    connect(doc, &QObject::destroyed, this, [this, doc] {
        // Normally removeDocument() came first; never touch the dead document's ranges.
        if (const auto it = m_docs.find(doc); it != m_docs.end()) {
            for (Diagnostic &d : it->second->diagnostics) {
                (void)d.range.release();
            }
            it->second->revisions.clear();
            m_docs.erase(it);
        }
    });
    attach(doc);
    updateIdle();
}

void LspManager::attach(KTextEditor::Document *doc)
{
    DocState *s = state(doc);
    const QString path = localPath(doc);
    if (!s || path.isEmpty() || m_root.isEmpty() || !path.startsWith(m_root + QLatin1Char('/'))) {
        return;
    }
    if (doc->totalCharacters() > kMaxDocumentSize) {
        return; // a minified bundle or generated file: not worth a server's time
    }
    const auto [languageId, serverId] = languageFor(path, m_root);
    if (serverId.isEmpty()) {
        return;
    }
    s->path = path;
    s->uri = uriFor(path);
    s->languageId = languageId;
    LspClient *client = clientFor(serverId);
    if (!client || client->state() == LspClient::State::Failed) {
        return;
    }
    s->serverId = serverId;
    if (QTimer *timer = m_idle.value(serverId)) {
        timer->stop();
    }
    if (client->state() == LspClient::State::Running) {
        sendOpen(s, client);
    }
    updateStatus();
}

void LspManager::detach(DocState *s)
{
    if (s->open) {
        if (LspClient *client = m_clients.value(s->serverId); client && client->state() == LspClient::State::Running) {
            client->notify(QStringLiteral("textDocument/didClose"),
                           QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s->uri}}}});
        }
    }
    clearDiagnostics(s);
    releaseRevisions(s, INT_MAX);
    s->open = false;
    s->dirty = false;
    s->synced.clear();
    s->serverId.clear();
    s->languageId.clear();
    s->uri.clear();
    s->path.clear();
}

void LspManager::removeDocument(KTextEditor::Document *doc)
{
    if (m_link && m_linkView && m_linkView->document() == doc) {
        m_link.reset();
    }
    DocState *s = state(doc);
    if (!s) {
        return;
    }
    detach(s);
    doc->disconnect(this);
    m_docs.erase(doc);
    updateIdle();
    updateStatus();
}

void LspManager::sendOpen(DocState *s, LspClient *client)
{
    s->synced = s->doc->text();
    s->dirty = false;
    ++s->version;
    client->notify(QStringLiteral("textDocument/didOpen"),
                   QJsonObject{{QStringLiteral("textDocument"),
                                QJsonObject{{QStringLiteral("uri"), s->uri},
                                            {QStringLiteral("languageId"), s->languageId},
                                            {QStringLiteral("version"), s->version},
                                            {QStringLiteral("text"), s->synced}}}});
    s->open = true;
    lockRevision(s);
    pullDiagnostics(s, client);
}

void LspManager::flushAll()
{
    for (auto &[doc, s] : m_docs) {
        if (s->dirty) {
            flush(doc);
        }
    }
}

void LspManager::flush(KTextEditor::Document *doc)
{
    DocState *s = state(doc);
    LspClient *client = readyClient(doc);
    if (!s || !client || !s->dirty) {
        return;
    }
    s->dirty = false;
    const QString text = doc->text();
    if (text == s->synced) {
        return;
    }
    const QJsonValue sync = client->capabilities().value(QLatin1String("textDocumentSync"));
    const int kind = sync.isObject() ? sync.toObject().value(QLatin1String("change")).toInt(1) : sync.toInt(1);
    QJsonObject change;
    if (kind == 2) {
        // Only the part that changed: same beginning and end are skipped.
        const QString &old = s->synced;
        const qsizetype common = std::min(old.size(), text.size());
        qsizetype prefix = 0;
        while (prefix < common && old.at(prefix) == text.at(prefix)) {
            ++prefix;
        }
        if (prefix > 0 && old.at(prefix - 1).isHighSurrogate()) {
            --prefix;
        }
        qsizetype suffix = 0;
        while (suffix < common - prefix && old.at(old.size() - 1 - suffix) == text.at(text.size() - 1 - suffix)) {
            ++suffix;
        }
        if (suffix > 0 && old.at(old.size() - suffix).isLowSurrogate()) {
            --suffix;
        }
        const KTextEditor::Cursor start = positionOf(old, prefix);
        const KTextEditor::Cursor end = positionOf(old, old.size() - suffix);
        change = QJsonObject{
            {QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), lspPosition(start)}, {QStringLiteral("end"), lspPosition(end)}}},
            {QStringLiteral("text"), text.mid(prefix, text.size() - prefix - suffix)},
        };
    } else {
        change = QJsonObject{{QStringLiteral("text"), text}};
    }
    ++s->version;
    client->notify(QStringLiteral("textDocument/didChange"),
                   QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s->uri}, {QStringLiteral("version"), s->version}}},
                               {QStringLiteral("contentChanges"), QJsonArray{change}}});
    s->synced = text;
    lockRevision(s);
    pullDiagnostics(s, client);
    // Other files may depend on this one: ask for theirs again too.
    if (client->capabilities().value(QLatin1String("diagnosticProvider")).toObject().value(QLatin1String("interFileDependencies")).toBool()) {
        for (auto &[other, os] : m_docs) {
            if (os.get() != s && os->open && os->serverId == s->serverId && !os->dirty) {
                pullDiagnostics(os.get(), client);
            }
        }
    }
}

void LspManager::pullDiagnostics(DocState *s, LspClient *client)
{
    // Servers that wait to be asked (pull diagnostics, e.g. TypeScript 7).
    if (!client->hasCapability(QStringLiteral("diagnosticProvider"))) {
        return;
    }
    if (s->pullRequest >= 0) {
        client->cancel(s->pullRequest);
    }
    KTextEditor::Document *doc = s->doc;
    const int version = s->version;
    s->pullRequest = client->request(
        QStringLiteral("textDocument/diagnostic"),
        QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s->uri}}}},
        doc,
        [this, doc, version](const QJsonValue &result, bool ok) {
            DocState *s = state(doc);
            if (!s) {
                return;
            }
            s->pullRequest = -1;
            const QJsonObject report = result.toObject();
            if (ok && s->open && report.value(QLatin1String("kind")).toString() == QLatin1String("full")) {
                setDiagnostics(s, report.value(QLatin1String("items")).toArray(), version);
            }
        },
        30000);
}

QJsonObject LspManager::positionParams(KTextEditor::Document *doc, KTextEditor::Cursor position) const
{
    DocState *s = state(doc);
    return QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s ? s->uri : uriFor(localPath(doc))}}},
                       {QStringLiteral("position"), lspPosition(position)}};
}

void LspManager::lockRevision(DocState *s)
{
    // Answers refer to the text of a version: keep its revision so their
    // positions can be moved through the edits made since.
    const qint64 revision = s->doc->revision();
    s->doc->lockRevision(revision);
    s->revisions[s->version] = revision;
    while (s->revisions.size() > 32) {
        s->doc->unlockRevision(s->revisions.begin()->second);
        s->revisions.erase(s->revisions.begin());
    }
}

void LspManager::releaseRevisions(DocState *s, int upToVersion)
{
    while (!s->revisions.empty() && s->revisions.begin()->first < upToVersion) {
        s->doc->unlockRevision(s->revisions.begin()->second);
        s->revisions.erase(s->revisions.begin());
    }
}

void LspManager::applyEdits(KTextEditor::Document *doc, const QJsonArray &edits, qint64 revision)
{
    QList<QPair<KTextEditor::Range, QString>> changes;
    for (const QJsonValue &v : edits) {
        const QJsonObject edit = v.toObject();
        changes.append({lspRange(edit.value(QLatin1String("range")).toObject()), edit.value(QLatin1String("newText")).toString()});
    }
    // From the bottom up, each moved through what changed since `revision`.
    std::sort(changes.begin(), changes.end(), [](const auto &a, const auto &b) {
        return a.first.start() > b.first.start();
    });
    if (!changes.isEmpty()) {
        KTextEditor::Document::EditingTransaction transaction(doc);
        for (auto &[range, text] : changes) {
            doc->transformRange(range, KTextEditor::MovingRange::DoNotExpand, KTextEditor::MovingRange::AllowEmpty, revision);
            if (range.isValid()) {
                doc->replaceText(range, text);
            }
        }
    }
    doc->unlockRevision(revision);
}

// ---------------------------------------------------------------------------
// Problems

void LspManager::clearDiagnostics(DocState *s)
{
    s->diagnostics.clear(); // deletes the moving ranges
}

void LspManager::setDiagnostics(DocState *s, const QJsonArray &diagnostics, int version)
{
    KTextEditor::Document *doc = s->doc;
    clearDiagnostics(s);
    s->diagnosticsReceived = true;
    qint64 from = -1;
    if (const auto it = s->revisions.find(version); version >= 0 && it != s->revisions.end()) {
        from = it->second;
    } else if (!s->revisions.empty()) {
        version = s->revisions.rbegin()->first;
        from = s->revisions.rbegin()->second;
    }
    for (const QJsonValue &value : diagnostics) {
        const QJsonObject o = value.toObject();
        const int severity = std::clamp(o.value(QLatin1String("severity")).toInt(1), 1, 4);
        bool unnecessary = false;
        bool deprecated = false;
        const QJsonArray tags = o.value(QLatin1String("tags")).toArray();
        for (const QJsonValue &tag : tags) {
            unnecessary |= tag.toInt() == 1;
            deprecated |= tag.toInt() == 2;
        }
        if (severity == 4 && !unnecessary && !deprecated) {
            continue; // hints (e.g. "could be converted to async") stay hidden
        }
        KTextEditor::Range range = lspRange(o.value(QLatin1String("range")).toObject());
        if (from >= 0) {
            doc->transformRange(range, KTextEditor::MovingRange::DoNotExpand, KTextEditor::MovingRange::AllowEmpty, from);
        }
        range = clampedRange(doc, range);
        Diagnostic d;
        d.range.reset(doc->newMovingRange(range));
        d.range->setAttribute(diagnosticAttribute(severity, unnecessary, deprecated));
        d.severity = severity;
        d.message = o.value(QLatin1String("message")).toString();
        d.source = o.value(QLatin1String("source")).toString();
        const QJsonValue code = o.value(QLatin1String("code"));
        d.code = code.isDouble() ? QString::number(code.toInt()) : code.toString();
        s->diagnostics.push_back(std::move(d));
    }
    if (version >= 0) {
        releaseRevisions(s, version);
    }
    updateStatus();
}

QString LspManager::diagnosticsHtml(DocState *s, KTextEditor::Cursor position) const
{
    QStringList parts;
    for (const Diagnostic &d : s->diagnostics) {
        const KTextEditor::Range r = d.range->toRange();
        if (d.severity > 3 || position < r.start() || position > r.end()) {
            continue;
        }
        QString message = d.message.toHtmlEscaped();
        message.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        QString origin = d.source;
        if (!d.code.isEmpty()) {
            origin += QStringLiteral("(%1)").arg(d.code);
        }
        parts << QStringLiteral("<p><span style=\"color:%1\">%2</span> %3%4</p>")
                     .arg(severityColor(d.severity).name(),
                          severityMark(d.severity),
                          message,
                          origin.isEmpty() ? QString() : QStringLiteral(" <span style=\"color:gray\">%1</span>").arg(origin.toHtmlEscaped()));
    }
    return parts.join(QString());
}

void LspManager::nextProblem(KTextEditor::View *view, bool forward)
{
    DocState *s = view ? state(view->document()) : nullptr;
    if (!s) {
        return;
    }
    QList<KTextEditor::Range> ranges;
    for (const Diagnostic &d : s->diagnostics) {
        if (d.severity <= 3) {
            ranges << d.range->toRange();
        }
    }
    if (ranges.isEmpty()) {
        return;
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
        return a.start() < b.start();
    });
    const KTextEditor::Cursor cursor = view->cursorPosition();
    KTextEditor::Range target = forward ? ranges.first() : ranges.last();
    if (forward) {
        for (const KTextEditor::Range &r : std::as_const(ranges)) {
            if (r.start() > cursor) {
                target = r;
                break;
            }
        }
    } else {
        for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
            if (it->start() < cursor) {
                target = *it;
                break;
            }
        }
    }
    view->setCursorPosition(target.start());
    view->setFocus();
    QPointer<KTextEditor::View> v(view);
    const KTextEditor::Cursor at = target.start();
    QTimer::singleShot(0, this, [this, v, at] {
        DocState *s = v ? state(v->document()) : nullptr;
        if (!s) {
            return;
        }
        const QString html = diagnosticsHtml(s, at);
        const QPoint point = v->cursorToCoordinate(at);
        if (!html.isEmpty() && point.x() >= 0) {
            QToolTip::showText(v->mapToGlobal(point + QPoint(0, v->fontMetrics().height() + 4)), html, v->focusProxy() ? v->focusProxy() : v.data());
        }
    });
}

void LspManager::statusClicked()
{
    if (m_current) {
        if (DocState *s = state(m_current->document()); s && !s->diagnostics.empty()) {
            nextProblem(m_current, true);
            return;
        }
    }
    // Problems are in another open file: go there.
    for (const auto &[doc, s] : m_docs) {
        const bool has = std::any_of(s->diagnostics.begin(), s->diagnostics.end(), [](const Diagnostic &d) {
            return d.severity <= 2;
        });
        if (has) {
            if (KTextEditor::View *view = m_window->openFile(s->path)) {
                view->setCursorPosition(KTextEditor::Cursor(0, 0));
                nextProblem(view, true);
            }
            return;
        }
    }
}

void LspManager::updateStatus()
{
    int errors = 0;
    int warnings = 0;
    for (const auto &[doc, s] : m_docs) {
        for (const Diagnostic &d : s->diagnostics) {
            errors += d.severity == 1;
            warnings += d.severity == 2;
        }
    }
    QStringList servers;
    bool busy = false;
    for (LspClient *client : std::as_const(m_clients)) {
        const QString name = client->spec().name;
        switch (client->state()) {
        case LspClient::State::Running:
            if (m_progress.contains(client)) {
                busy = true;
                servers << QStringLiteral("%1: %2").arg(name, m_progress.value(client));
            } else {
                servers << tr("%1 language server: running").arg(name);
            }
            break;
        case LspClient::State::Starting:
            busy = true;
            servers << tr("%1 language server: starting…").arg(name);
            break;
        case LspClient::State::Failed:
            servers << tr("%1 language server stopped: %2").arg(name, client->lastError());
            break;
        case LspClient::State::Idle:
            break;
        }
    }
    const bool active = std::any_of(m_clients.cbegin(), m_clients.cend(), [](LspClient *c) {
        return c->state() == LspClient::State::Running || c->state() == LspClient::State::Starting;
    });
    if (!active && errors == 0 && warnings == 0) {
        m_status->setItem(QStringLiteral("problems"), QString(), QIcon(), QStringLiteral(""));
        return;
    }
    const QString errorText = errors == 1 ? tr("1 error") : tr("%1 errors").arg(errors);
    const QString warningText = warnings == 1 ? tr("1 warning") : tr("%1 warnings").arg(warnings);
    QStringList tip{tr("Problems in the open files: %1, %2").arg(errorText, warningText)};
    tip << servers;
    tip << tr("Click or F8: next problem");
    m_status->setItem(QStringLiteral("problems"),
                      QStringLiteral("%1⊗ %2  ⚠ %3").arg(busy ? QStringLiteral("⟳ ") : QString()).arg(errors).arg(warnings),
                      QIcon(),
                      tip.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------
// Views: hover, completion, Ctrl+click

void LspManager::addView(KTextEditor::View *view)
{
    m_views.removeAll(nullptr);
    m_views << view;
    view->registerTextHintProvider(m_hints.get());
    view->registerCompletionModel(m_completion);
    if (QWidget *inner = view->focusProxy()) {
        inner->installEventFilter(this);
        inner->setProperty("lieveLspView", QVariant::fromValue<QObject *>(view));
    }
    connect(view, &KTextEditor::View::contextMenuAboutToShow, this, [this](KTextEditor::View *v, QMenu *menu) {
        if (!menu) {
            return;
        }
        if (!menu->actions().contains(m_definitionAction)) {
            QAction *first = menu->actions().value(0);
            menu->insertAction(first, m_definitionAction);
            menu->insertSeparator(first);
        }
        m_definitionAction->setVisible(readyClient(v->document()) != nullptr);
        if (m_menuView != v || !m_menuClock.isValid() || m_menuClock.elapsed() > 1500) {
            m_menuView = nullptr; // opened from the keyboard: at the cursor
        }
    });
}

void LspManager::setCurrentView(KTextEditor::View *view)
{
    m_current = view;
    updateStatus();
}

QString LspManager::textHint(KTextEditor::View *view, KTextEditor::Cursor position)
{
    DocState *s = state(view->document());
    if (!s) {
        return {};
    }
    const QString problems = diagnosticsHtml(s, position);
    LspClient *client = readyClient(view->document());
    if (client && client->hasCapability(QStringLiteral("hoverProvider"))) {
        flush(view->document());
        if (m_hoverRequest >= 0 && m_hoverClient) {
            m_hoverClient->cancel(m_hoverRequest);
        }
        QPointer<KTextEditor::View> v(view);
        m_hoverClient = client;
        m_hoverRequest = client->request(
            QStringLiteral("textDocument/hover"),
            positionParams(view->document(), position),
            view,
            [this, v, position, problems](const QJsonValue &result, bool ok) {
                m_hoverRequest = -1;
                if (ok && v) {
                    showHover(v, position, problems, result);
                }
            },
            4000);
    }
    return problems;
}

void LspManager::showHover(KTextEditor::View *view, KTextEditor::Cursor position, const QString &problems, const QJsonValue &result)
{
    const QString markdown = hoverMarkdown(result.toObject().value(QLatin1String("contents"))).trimmed();
    if (markdown.isEmpty()) {
        return; // the problems (if any) are already shown
    }
    // Only while the mouse is still on that line of this view.
    QWidget *inner = view->focusProxy() ? view->focusProxy() : view;
    const QPoint global = QCursor::pos();
    if (!inner->isVisible() || !inner->rect().contains(inner->mapFromGlobal(global))) {
        return;
    }
    const KTextEditor::Cursor under = view->coordinatesToCursor(view->mapFromGlobal(global));
    if (under.isValid() && under.line() != position.line()) {
        return;
    }
    QString html = markdownToHtml(markdown);
    if (!problems.isEmpty()) {
        html = problems + QStringLiteral("<hr>") + html;
    }
    QToolTip::showText(global, html, inner);
}

void LspManager::setLink(KTextEditor::View *view, KTextEditor::Range range)
{
    if (m_link && m_linkView == view && m_link->toRange() == range) {
        return;
    }
    m_link.reset();
    m_linkView = view;
    if (!view || !range.isValid() || range.isEmpty()) {
        return;
    }
    static KTextEditor::Attribute::Ptr attribute = [] {
        KTextEditor::Attribute::Ptr a(new KTextEditor::Attribute());
        a->setUnderlineStyle(QTextCharFormat::SingleUnderline);
        return a;
    }();
    m_link.reset(view->document()->newMovingRange(range));
    m_link->setAttribute(attribute);
    m_link->setView(view);
}

bool LspManager::eventFilter(QObject *watched, QEvent *event)
{
    auto *view = qobject_cast<KTextEditor::View *>(watched->property("lieveLspView").value<QObject *>());
    auto *inner = qobject_cast<QWidget *>(watched);
    if (!view || !inner) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::RightButton) {
            m_menuView = view;
            m_menuCursor = view->coordinatesToCursor(inner->mapTo(view, mouse->position().toPoint()));
            m_menuClock.start();
        }
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::ControlModifier && readyClient(view->document())) {
            m_pressPos = mouse->position().toPoint();
            return true; // Ctrl+click is go to definition, not a selection
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::ControlModifier && readyClient(view->document())
            && (mouse->position().toPoint() - m_pressPos).manhattanLength() < 6) {
            const KTextEditor::Cursor cursor = view->coordinatesToCursor(inner->mapTo(view, mouse->position().toPoint()));
            setLink(nullptr, KTextEditor::Range::invalid());
            if (cursor.isValid()) {
                QPointer<KTextEditor::View> v(view);
                QTimer::singleShot(0, this, [this, v, cursor] {
                    if (v) {
                        goToDefinition(v, cursor);
                    }
                });
            }
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->modifiers() == Qt::ControlModifier && mouse->buttons() == Qt::NoButton && readyClient(view->document())) {
            const KTextEditor::Cursor cursor = view->coordinatesToCursor(inner->mapTo(view, mouse->position().toPoint()));
            setLink(view, cursor.isValid() ? view->document()->wordRangeAt(cursor) : KTextEditor::Range::invalid());
        } else if (m_link) {
            setLink(nullptr, KTextEditor::Range::invalid());
        }
        break;
    }
    case QEvent::KeyRelease:
    case QEvent::Leave:
    case QEvent::FocusOut:
        if (m_link) {
            setLink(nullptr, KTextEditor::Range::invalid());
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Go to definition, symbols

void LspManager::showMessage(KTextEditor::View *view, const QString &text)
{
    if (view) {
        postMessage(view, text);
    }
}

void LspManager::goToDefinition(KTextEditor::View *view, KTextEditor::Cursor position)
{
    if (!view) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    DocState *s = state(doc);
    LspClient *client = s && !s->serverId.isEmpty() ? m_clients.value(s->serverId) : nullptr;
    if (!client) {
        showMessage(view, tr("Go to Definition needs a language server for this file (see [lsp] in Settings, Ctrl+,)."));
        return;
    }
    if (client->state() == LspClient::State::Failed) {
        showMessage(view, tr("The %1 language server stopped: %2").arg(client->spec().name, client->lastError()));
        return;
    }
    if (client->state() == LspClient::State::Running && !client->hasCapability(QStringLiteral("definitionProvider"))) {
        return;
    }
    flush(doc);
    if (m_definitionRequest >= 0 && m_definitionClient) {
        m_definitionClient->cancel(m_definitionRequest);
    }
    const QString word = doc->wordAt(position);
    QPointer<KTextEditor::View> v(view);
    m_definitionClient = client;
    // Asked even while the server starts: it waits for the project to load.
    m_definitionRequest = client->request(
        QStringLiteral("textDocument/definition"),
        positionParams(doc, position),
        view,
        [this, v, word](const QJsonValue &result, bool ok) {
            m_definitionRequest = -1;
            if (!v) {
                return;
            }
            if (!ok) {
                return; // cancelled or timed out
            }
            QList<Location> locations;
            const QJsonArray list = result.isArray() ? result.toArray() : result.isObject() ? QJsonArray{result} : QJsonArray();
            for (const QJsonValue &value : list) {
                const QJsonObject o = value.toObject();
                const bool link = o.contains(QLatin1String("targetUri"));
                const QString path = QUrl(o.value(link ? QLatin1String("targetUri") : QLatin1String("uri")).toString()).toLocalFile();
                const KTextEditor::Range range = lspRange(o.value(link ? QLatin1String("targetSelectionRange") : QLatin1String("range")).toObject());
                if (path.isEmpty()) {
                    continue;
                }
                const bool seen = std::any_of(locations.cbegin(), locations.cend(), [&](const Location &l) {
                    return l.path == path && l.cursor == range.start();
                });
                if (!seen) {
                    locations.append({path, range.start()});
                }
            }
            if (qEnvironmentVariableIsSet("LIEVE_TEST_LSP")) {
                for (const Location &l : std::as_const(locations)) {
                    qInfo().noquote() << "lsp-test definition" << QDir(m_root).relativeFilePath(l.path) << l.cursor.line() + 1 << l.cursor.column() + 1;
                }
            }
            if (locations.isEmpty()) {
                showMessage(v, word.isEmpty() ? tr("No definition found.") : tr("No definition found for “%1”.").arg(word));
            } else if (locations.size() == 1) {
                jumpTo(locations.first().path, locations.first().cursor, true);
            } else {
                QList<SymbolPicker::Entry> entries;
                for (int i = 0; i < locations.size(); ++i) {
                    const Location &l = locations.at(i);
                    const QString relative = m_root.isEmpty() ? l.path : QDir(m_root).relativeFilePath(l.path);
                    entries.append({QStringLiteral("%1:%2").arg(QFileInfo(l.path).fileName()).arg(l.cursor.line() + 1),
                                    QFileInfo(relative).path() == QLatin1String(".") ? QString() : QFileInfo(relative).path(),
                                    symbolKindIcon(1),
                                    i});
                }
                m_pickerTargets = locations;
                m_pickerView = v;
                m_pickerPreview = false;
                m_picker->popup(tr("%1 definitions of “%2”").arg(locations.size()).arg(word), entries);
            }
        },
        20000);
}

void LspManager::jumpTo(const QString &path, KTextEditor::Cursor position, bool record)
{
    if (record && m_current) {
        const QString from = localPath(m_current->document());
        if (!from.isEmpty()) {
            m_back.append({from, m_current->cursorPosition()});
            while (m_back.size() > kMaxHistory) {
                m_back.removeFirst();
            }
            m_forward.clear();
        }
    }
    m_window->openFile(path, position.line(), position.column());
}

void LspManager::navigate(int direction)
{
    QList<Location> &from = direction < 0 ? m_back : m_forward;
    QList<Location> &to = direction < 0 ? m_forward : m_back;
    while (!from.isEmpty()) {
        const Location target = from.takeLast();
        if (!QFileInfo::exists(target.path)) {
            continue;
        }
        if (m_current && !localPath(m_current->document()).isEmpty()) {
            to.append({localPath(m_current->document()), m_current->cursorPosition()});
        }
        m_window->openFile(target.path, target.cursor.line(), target.cursor.column());
        return;
    }
}

void LspManager::showDocumentSymbols(KTextEditor::View *view)
{
    if (!view) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    DocState *s = state(doc);
    LspClient *client = s && !s->serverId.isEmpty() ? m_clients.value(s->serverId) : nullptr;
    if (!client || client->state() == LspClient::State::Failed) {
        showMessage(view, tr("Go to Symbol needs a language server for this file (see [lsp] in Settings, Ctrl+,)."));
        return;
    }
    if (client->state() == LspClient::State::Running && !client->hasCapability(QStringLiteral("documentSymbolProvider"))) {
        return;
    }
    flush(doc);
    QPointer<KTextEditor::View> v(view);
    const QString path = s->path;
    client->request(
        QStringLiteral("textDocument/documentSymbol"),
        QJsonObject{{QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), s->uri}}}},
        view,
        [this, v, path](const QJsonValue &result, bool ok) {
            if (!ok || !v) {
                return;
            }
            QList<SymbolPicker::Entry> entries;
            QList<Location> targets;
            // DocumentSymbol trees (or flat SymbolInformation), in file order.
            std::function<void(const QJsonArray &, const QString &)> add = [&](const QJsonArray &symbols, const QString &container) {
                for (const QJsonValue &value : symbols) {
                    const QJsonObject o = value.toObject();
                    const QString name = o.value(QLatin1String("name")).toString();
                    KTextEditor::Range range;
                    if (o.contains(QLatin1String("selectionRange"))) {
                        range = lspRange(o.value(QLatin1String("selectionRange")).toObject());
                    } else {
                        range = lspRange(o.value(QLatin1String("location")).toObject().value(QLatin1String("range")).toObject());
                    }
                    QString detail = o.contains(QLatin1String("containerName")) ? o.value(QLatin1String("containerName")).toString() : container;
                    entries.append({name, detail, symbolKindIcon(o.value(QLatin1String("kind")).toInt()), int(targets.size())});
                    targets.append({path, range.start()});
                    const QJsonArray children = o.value(QLatin1String("children")).toArray();
                    if (!children.isEmpty()) {
                        add(children, container.isEmpty() ? name : container + QStringLiteral(" › ") + name);
                    }
                }
            };
            add(result.toArray(), QString());
            // Flat lists come in any order.
            QList<int> order(entries.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&targets](int a, int b) {
                return targets.at(a).cursor < targets.at(b).cursor;
            });
            QList<SymbolPicker::Entry> sortedEntries;
            for (const int i : std::as_const(order)) {
                sortedEntries << entries.at(i);
            }
            if (qEnvironmentVariableIsSet("LIEVE_TEST_LSP")) {
                for (const SymbolPicker::Entry &e : std::as_const(sortedEntries)) {
                    qInfo().noquote() << "lsp-test symbol" << e.name << "|" << e.detail << "|" << targets.at(e.index).cursor.line() + 1;
                }
            }
            if (entries.isEmpty()) {
                showMessage(v, tr("No symbols in this file."));
                return;
            }
            m_pickerTargets = targets;
            m_pickerView = v;
            m_pickerPreview = true;
            m_pickerOrigin = v->cursorPosition();
            m_picker->popup(tr("Go to symbol in file…"), sortedEntries);
        },
        10000);
}

// ---------------------------------------------------------------------------
// Automated checks

namespace
{
// Resident memory (kB) of a process and everything it started.
QList<QPair<QString, qint64>> processTree(qint64 root)
{
    QHash<qint64, qint64> parents;
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &p : pids) {
        bool ok = false;
        const qint64 pid = p.toLongLong(&ok);
        if (!ok) {
            continue;
        }
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (stat.open(QIODevice::ReadOnly)) {
            const QByteArray line = stat.readAll();
            const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
            parents.insert(pid, fields.value(1).toLongLong());
        }
    }
    QList<qint64> tree{root};
    for (qsizetype i = 0; i < tree.size(); ++i) {
        for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
            if (it.value() == tree.at(i)) {
                tree << it.key();
            }
        }
    }
    QList<QPair<QString, qint64>> out;
    for (const qint64 pid : std::as_const(tree)) {
        QFile status(QStringLiteral("/proc/%1/status").arg(pid));
        QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!status.open(QIODevice::ReadOnly) || !cmdline.open(QIODevice::ReadOnly)) {
            continue;
        }
        qint64 rss = 0;
        const QList<QByteArray> lines = status.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("VmRSS:")) {
                rss = line.mid(6).trimmed().split(' ').value(0).toLongLong();
            }
        }
        const QList<QByteArray> args = cmdline.readAll().split('\0');
        QStringList shown;
        for (const QByteArray &a : args.mid(0, 3)) {
            shown << QFileInfo(QString::fromLocal8Bit(a)).fileName();
        }
        out.append({shown.join(QLatin1Char(' ')).trimmed(), rss});
    }
    return out;
}
}

void LspManager::runTest(const QString &script)
{
    auto steps = std::make_shared<QStringList>(script.split(QLatin1Char('|'), Qt::SkipEmptyParts));
    auto run = std::make_shared<std::function<void()>>();
    QPointer<LspManager> self(this);
    *run = [this, self, steps, run] {
        if (!self || steps->isEmpty()) {
            return;
        }
        const QString step = steps->takeFirst().trimmed();
        const QStringList parts = step.split(QLatin1Char(':'));
        const QString name = parts.value(0);
        const auto next = [this, run](int ms) {
            QTimer::singleShot(ms, this, [run] {
                (*run)();
            });
        };
        const auto at = [&parts](int i) {
            return KTextEditor::Cursor(parts.value(i).toInt() - 1, parts.value(i + 1).toInt() - 1);
        };
        KTextEditor::View *view = m_current;
        qInfo().noquote() << "lsp-test step" << step;
        if (!view && name != QLatin1String("sleep") && name != QLatin1String("memory")) {
            qInfo().noquote() << "lsp-test no view";
            next(0);
            return;
        }
        if (name == QLatin1String("sleep")) {
            next(parts.value(1).toInt());
        } else if (name == QLatin1String("ready")) {
            // Until the current file has its first problems (the project is loaded).
            auto started = std::make_shared<QElapsedTimer>();
            started->start();
            const int limit = parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 60000;
            auto poll = std::make_shared<std::function<void()>>();
            QPointer<KTextEditor::View> v(view);
            *poll = [this, v, started, limit, poll, next] {
                DocState *s = v ? state(v->document()) : nullptr;
                if ((s && s->diagnosticsReceived) || started->elapsed() > limit) {
                    qInfo().noquote() << "lsp-test ready" << (s && s->diagnosticsReceived) << started->elapsed() << "ms";
                    next(0);
                    QTimer::singleShot(0, this, [poll] {
                        *poll = nullptr; // breaks the cycle
                    });
                    return;
                }
                QTimer::singleShot(100, this, [poll] {
                    if (*poll) {
                        (*poll)();
                    }
                });
            };
            (*poll)();
        } else if (name == QLatin1String("problems")) {
            if (DocState *s = state(view->document())) {
                for (const Diagnostic &d : s->diagnostics) {
                    const KTextEditor::Range r = d.range->toRange();
                    qInfo().noquote() << "lsp-test problem" << d.severity << r.start().line() + 1 << r.start().column() + 1 << r.end().column() + 1
                                      << d.message.section(QLatin1Char('\n'), 0, 0);
                }
            }
            next(0);
        } else if (name == QLatin1String("definition")) {
            goToDefinition(view, at(1));
            next(parts.value(3).toInt() > 0 ? parts.value(3).toInt() : 3000);
        } else if (name == QLatin1String("ctrlclick")) {
            // Through the event filter, as a mouse would (once the line is on screen).
            view->setCursorPosition(at(1));
            QPointer<KTextEditor::View> v(view);
            const KTextEditor::Cursor c = at(1);
            QTimer::singleShot(300, this, [v, c] {
                if (!v) {
                    return;
                }
                QWidget *inner = v->focusProxy();
                const QPoint point = inner->mapFrom(v, v->cursorToCoordinate(c) + QPoint(3, 3));
                QMouseEvent move(QEvent::MouseMove, point, inner->mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::ControlModifier);
                QCoreApplication::sendEvent(inner, &move);
                QMouseEvent press(QEvent::MouseButtonPress, point, inner->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
                QCoreApplication::sendEvent(inner, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, point, inner->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
                QCoreApplication::sendEvent(inner, &release);
            });
            next(parts.value(3).toInt() > 0 ? parts.value(3).toInt() : 3000);
        } else if (name == QLatin1String("where")) {
            qInfo().noquote() << "lsp-test where" << QDir(m_root).relativeFilePath(localPath(view->document())) << view->cursorPosition().line() + 1
                              << view->cursorPosition().column() + 1;
            next(0);
        } else if (name == QLatin1String("back")) {
            navigate(-1);
            next(500);
        } else if (name == QLatin1String("hover")) {
            LspClient *client = readyClient(view->document());
            if (!client) {
                qInfo().noquote() << "lsp-test hover: no server";
                next(0);
                return;
            }
            flush(view->document());
            qInfo().noquote() << "lsp-test hint" << textHint(view, at(1));
            client->request(QStringLiteral("textDocument/hover"), positionParams(view->document(), at(1)), this, [next](const QJsonValue &result, bool ok) {
                const QString markdown = hoverMarkdown(result.toObject().value(QLatin1String("contents")));
                qInfo().noquote() << "lsp-test hover" << ok << QString(markdown.left(300)).replace(QLatin1Char('\n'), QStringLiteral("⏎"));
                qInfo().noquote() << "lsp-test hover-html" << markdownToHtml(markdown).left(400);
                next(0);
            });
        } else if (name == QLatin1String("type")) {
            // Edits a (scratch) file: text after "type:line:col:", "\n" for newlines.
            QString text = parts.mid(3).join(QLatin1Char(':'));
            text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            view->setCursorPosition(at(1));
            view->document()->insertText(at(1), text);
            next(100);
        } else if (name == QLatin1String("delete")) {
            // delete:line:col:line:col
            view->document()->removeText(KTextEditor::Range(at(1), at(3)));
            next(100);
        } else if (name == QLatin1String("complete")) {
            const KTextEditor::Cursor c = at(1);
            view->setCursorPosition(c);
            view->setFocus();
            view->startCompletion(m_completion->completionRange(view, c), {m_completion}, KTextEditor::CodeCompletionModel::UserInvocation);
            QPointer<KTextEditor::View> v(view);
            QTimer::singleShot(parts.value(3).toInt() > 0 ? parts.value(3).toInt() : 3000, this, [this, v, next] {
                const QStringList labels = m_completion->labels();
                qInfo().noquote() << "lsp-test completion" << labels.size() << "active" << (v && v->isCompletionActive())
                                  << labels.mid(0, 12).join(QLatin1Char(','));
                next(0);
            });
        } else if (name == QLatin1String("accept")) {
            view->forceCompletion();
            next(parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 1500);
        } else if (name == QLatin1String("line")) {
            qInfo().noquote() << "lsp-test line" << parts.value(1) << view->document()->line(parts.value(1).toInt() - 1);
            next(0);
        } else if (name == QLatin1String("symbols")) {
            showDocumentSymbols(view);
            next(2000);
        } else if (name == QLatin1String("f8")) {
            nextProblem(view, true);
            qInfo().noquote() << "lsp-test f8" << view->cursorPosition().line() + 1 << view->cursorPosition().column() + 1;
            next(300);
        } else if (name == QLatin1String("status")) {
            if (QToolButton *item = m_status->item(QStringLiteral("problems"))) {
                qInfo().noquote() << "lsp-test status" << item->isVisible() << item->text() << "|"
                                  << item->toolTip().replace(QLatin1Char('\n'), QStringLiteral(" | "));
            }
            next(0);
        } else if (name == QLatin1String("keys")) {
            // Typed as keys, so the editor's own automatic completion runs.
            QWidget *inner = view->focusProxy();
            view->setFocus();
            for (const QChar c : parts.mid(1).join(QLatin1Char(':'))) {
                QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                QCoreApplication::sendEvent(inner, &press);
                QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                QCoreApplication::sendEvent(inner, &release);
            }
            QPointer<KTextEditor::View> v(view);
            QTimer::singleShot(2500, this, [this, v, next] {
                qInfo().noquote() << "lsp-test keys completion active" << (v && v->isCompletionActive()) << m_completion->labels().size()
                                  << m_completion->labels().mid(0, 8).join(QLatin1Char(','));
                next(0);
            });
        } else if (name == QLatin1String("hovershow")) {
            // The hover tooltip, saved as a picture next to the screenshot.
            LspClient *client = readyClient(view->document());
            if (!client) {
                next(0);
                return;
            }
            const KTextEditor::Cursor c = at(1);
            const QString problems = textHint(view, c);
            QPointer<KTextEditor::View> v(view);
            const QJsonObject params = positionParams(view->document(), c);
            client->request(QStringLiteral("textDocument/hover"), params, this, [v, c, problems, next](const QJsonValue &result, bool) {
                if (v) {
                    QString html = markdownToHtml(hoverMarkdown(result.toObject().value(QLatin1String("contents"))).trimmed());
                    if (!problems.isEmpty()) {
                        html = problems + QStringLiteral("<hr>") + html;
                    }
                    QToolTip::showText(v->mapToGlobal(v->cursorToCoordinate(c)), html, v->focusProxy());
                    QTimer::singleShot(500, v, [] {
                        const auto widgets = QApplication::topLevelWidgets();
                        for (QWidget *w : widgets) {
                            if (w->inherits("QTipLabel") && w->isVisible()) {
                                w->grab().save(qEnvironmentVariable("LIEVE_SCREENSHOT").replace(QStringLiteral(".png"), QStringLiteral("-tip.png")));
                            }
                        }
                    });
                }
                next(1000);
            });
        } else if (name == QLatin1String("crash")) {
            // The server dies: it is started again once.
            if (LspClient *client = m_clients.value(QStringLiteral("typescript"))) {
                for (const qint64 pid : client->processIds()) {
                    ::kill(-pid, SIGKILL);
                }
            }
            next(500);
        } else if (name == QLatin1String("close")) {
            const auto actions = m_window->actions();
            for (QAction *a : actions) {
                if (a->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_W))) {
                    a->trigger();
                    break;
                }
            }
            next(parts.value(1).toInt() > 0 ? parts.value(1).toInt() : 1500);
        } else if (name == QLatin1String("memory")) {
            qint64 total = 0;
            const auto tree = processTree(QCoreApplication::applicationPid());
            for (const auto &[command, rss] : tree) {
                qInfo().noquote() << "lsp-test memory" << rss / 1024 << "MB" << command;
                total += rss;
            }
            qInfo().noquote() << "lsp-test memory total" << total / 1024 << "MB";
            next(0);
        } else {
            qInfo().noquote() << "lsp-test unknown step" << name;
            next(0);
        }
    };
    (*run)();
}
}
