#include "outline.h"

#include "breadcrumbs.h"
#include "commandpalette.h"
#include "lsp.h"
#include "lspclient.h"
#include "mainwindow.h"
#include "settings.h"
#include "stickyscroll.h"
#include "tabview.h"
#include "windowmanager.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MovingCursor>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QTimer>

#include <functional>

namespace Lieve
{
namespace
{
constexpr int kMaxLines = 50000; // larger files get no scopes
constexpr int kMaxCharacters = 3000000; // nor minified ones
constexpr int kServerDelay = 500; // ms after the last edit before asking the server again
constexpr int kMaxFailures = 3;

void readSymbols(const QJsonArray &symbols, ScopeTree &tree)
{
    const auto position = [](const QJsonObject &range, const char *which) {
        const QJsonObject p = range.value(QLatin1String(which)).toObject();
        return qMakePair(p.value(QLatin1String("line")).toInt(), p.value(QLatin1String("character")).toInt());
    };
    for (const QJsonValue &value : symbols) {
        // DocumentSymbol (range, selectionRange, children) or SymbolInformation (location)
        const QJsonObject o = value.toObject();
        const QJsonObject range = o.contains(QLatin1String("range")) ? o.value(QLatin1String("range")).toObject()
                                                                       : o.value(QLatin1String("location")).toObject().value(QLatin1String("range")).toObject();
        const QJsonObject selection = o.contains(QLatin1String("selectionRange")) ? o.value(QLatin1String("selectionRange")).toObject() : range;
        Scope s;
        std::tie(s.startLine, s.startColumn) = position(range, "start");
        std::tie(s.endLine, s.endColumn) = position(range, "end");
        std::tie(s.targetLine, s.targetColumn) = position(selection, "start");
        s.name = o.value(QLatin1String("name")).toString().simplified();
        s.kind = o.value(QLatin1String("kind")).toInt();
        if (s.kind <= 0) {
            s.kind = 13;
        }
        tree.scopes.push_back(s);
        readSymbols(o.value(QLatin1String("children")).toArray(), tree);
    }
}
}

DocumentScopes::DocumentScopes(LspManager *lsp, QObject *parent)
    : QObject(parent)
    , m_lsp(lsp)
{
}

DocumentScopes::~DocumentScopes()
{
    for (auto &[doc, e] : m_entries) {
        if (e.doc) {
            e.doc->disconnect(this);
            dropServer(e, true);
            if (e.request >= 0 && e.pendingGeneration == e.generation) {
                e.doc->unlockRevision(e.pendingRevision);
            }
        }
    }
}

DocumentScopes::Entry &DocumentScopes::entry(KTextEditor::Document *doc)
{
    auto it = m_entries.find(doc);
    if (it != m_entries.end()) {
        return it->second;
    }
    Entry &e = m_entries[doc];
    e.doc = doc;
    connect(doc, &QObject::destroyed, this, [this, doc] {
        if (const auto found = m_entries.find(doc); found != m_entries.end()) {
            delete found->second.timer;
            m_entries.erase(found); // its revisions went with it
        }
    });
    connect(doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, [this](KTextEditor::Document *d) {
        // Reloaded: the locked revisions are gone (never unlock them).
        if (const auto found = m_entries.find(d); found != m_entries.end()) {
            Entry &e = found->second;
            ++e.generation;
            e.server.reset();
            e.serverRevision = -1;
            e.tree.reset();
            e.revision = -1;
        }
    });
    return e;
}

void DocumentScopes::dropServer(Entry &e, bool unlock)
{
    if (e.server && unlock && e.doc) {
        e.doc->unlockRevision(e.serverRevision);
    }
    e.server.reset();
    e.serverRevision = -1;
}

std::shared_ptr<const ScopeTree> DocumentScopes::scopes(KTextEditor::Document *doc)
{
    if (!doc) {
        return {};
    }
    Entry &e = entry(doc);
    const qint64 revision = doc->revision();
    if (e.request >= 0 && !e.client) { // the server went away with the question
        if (e.pendingGeneration == e.generation) {
            doc->unlockRevision(e.pendingRevision);
        }
        e.request = -1;
    }
    LspClient *client = m_lsp ? m_lsp->readyClient(doc) : nullptr;
    const bool fromServer = client && e.failures < kMaxFailures && client->hasCapability(QStringLiteral("documentSymbolProvider"));
    if (fromServer) {
        if (!e.server) {
            if (e.request < 0) {
                request(e); // the first answer: at once
            }
        } else if (e.serverRevision != revision && e.request < 0) {
            // Edited since: asked again once typing stops; meanwhile the
            // last answer follows the edits.
            if (!e.timer) {
                e.timer = new QTimer(this);
                e.timer->setSingleShot(true);
                e.timer->setInterval(kServerDelay);
                connect(e.timer, &QTimer::timeout, this, [this, doc] {
                    if (const auto found = m_entries.find(doc); found != m_entries.end() && found->second.doc) {
                        request(found->second);
                    }
                });
            }
            e.timer->start();
        }
        if (e.server) {
            if (e.revision != revision || !e.tree || !e.tree->fromServer) {
                e.tree = moved(e);
                e.revision = revision;
            }
            return e.tree;
        }
    } else if (e.server) {
        dropServer(e, true); // the server stopped: back to the text
        e.tree.reset();
    }
    if (!e.tree || e.revision != revision || e.tree->fromServer) {
        e.tree = heuristic(doc);
        e.revision = revision;
    }
    return e.tree;
}

void DocumentScopes::request(Entry &e)
{
    KTextEditor::Document *doc = e.doc;
    LspClient *client = doc && m_lsp ? m_lsp->readyClient(doc) : nullptr;
    if (!client || e.request >= 0) {
        return;
    }
    m_lsp->flush(doc); // the server has this revision's text
    const QJsonObject params{{QStringLiteral("textDocument"), m_lsp->positionParams(doc, KTextEditor::Cursor(0, 0)).value(QLatin1String("textDocument"))}};
    const qint64 revision = doc->revision();
    doc->lockRevision(revision); // its positions follow the edits made meanwhile
    const int generation = e.generation;
    QPointer<KTextEditor::Document> d(doc);
    e.client = client;
    e.pendingRevision = revision;
    e.pendingGeneration = generation;
    e.request = client->request(
        QStringLiteral("textDocument/documentSymbol"),
        params,
        this,
        [this, d, revision, generation](const QJsonValue &result, bool ok) {
            if (!d) {
                return;
            }
            const auto found = m_entries.find(d.data());
            if (found == m_entries.end()) {
                return;
            }
            Entry &e = found->second;
            e.request = -1;
            if (e.generation != generation) {
                return; // reloaded meanwhile: that revision is gone
            }
            if (!ok) {
                d->unlockRevision(revision);
                ++e.failures;
                return;
            }
            e.failures = 0;
            auto tree = std::make_shared<ScopeTree>();
            readSymbols(result.toArray(), *tree);
            tree->fromServer = true;
            tree->build();
            dropServer(e, true);
            e.server = tree;
            e.serverRevision = revision;
            e.revision = -1;
            Q_EMIT changed(d);
        },
        10000);
}

std::shared_ptr<const ScopeTree> DocumentScopes::moved(const Entry &e) const
{
    KTextEditor::Document *doc = e.doc;
    if (!doc || e.serverRevision == doc->revision()) {
        return e.server;
    }
    auto tree = std::make_shared<ScopeTree>(*e.server);
    for (Scope &s : tree->scopes) {
        doc->transformCursor(s.startLine, s.startColumn, KTextEditor::MovingCursor::MoveOnInsert, e.serverRevision);
        doc->transformCursor(s.endLine, s.endColumn, KTextEditor::MovingCursor::StayOnInsert, e.serverRevision);
        doc->transformCursor(s.targetLine, s.targetColumn, KTextEditor::MovingCursor::MoveOnInsert, e.serverRevision);
    }
    return tree;
}

std::shared_ptr<const ScopeTree> DocumentScopes::heuristic(KTextEditor::Document *doc) const
{
    const int count = doc->lines();
    const QString mode = doc->highlightingMode();
    if (count > kMaxLines || doc->totalCharacters() > kMaxCharacters || mode == QLatin1String("Diff")) { // diff tabs: no blocks worth showing
        return std::make_shared<const ScopeTree>();
    }
    ScopeOptions o;
    o.style = scopeStyleFor(mode, doc->documentName());
    o.tabWidth = doc->configValue(QStringLiteral("tab-width")).toInt();
    if (o.tabWidth <= 0) {
        o.tabWidth = 4;
    }
    const KSyntaxHighlighting::Definition definition = KTextEditor::Editor::instance()->repository().definitionForName(mode);
    if (definition.isValid()) {
        o.lineComment = definition.singleLineCommentMarker();
        const QPair<QString, QString> block = definition.multiLineCommentMarker();
        o.blockCommentStart = block.first;
        o.blockCommentEnd = block.second;
    }
    const QString lower = mode.toLower();
    if (lower.startsWith(QLatin1String("json")) && o.lineComment.isEmpty()) {
        o.lineComment = QStringLiteral("//"); // JSON with comments (tsconfig.json...)
    }
    o.css = lower.contains(QLatin1String("css")) || lower == QLatin1String("sass") || lower == QLatin1String("stylus");
    QStringList lines;
    lines.reserve(count);
    for (int i = 0; i < count; ++i) {
        lines.append(doc->line(i));
    }
    return std::make_shared<const ScopeTree>(heuristicScopes(lines, o));
}
}

// ---------------------------------------------------------------------------

using namespace Lieve;

namespace
{
// The descriptions of the Settings window, also written above the keys.
const char kBreadcrumbsComment[] = QT_TRANSLATE_NOOP("SettingsDialog", "Shows the path of the file and the symbols around the cursor above the editor.");
const char kStickyComment[] = QT_TRANSLATE_NOOP("SettingsDialog", "Keeps the first lines of the blocks you are scrolled into at the top of the editor.");
}

EditorOutline::EditorOutline(MainWindow *window, LspManager *lsp)
    : QObject(window)
    , m_window(window)
    , m_scopes(new DocumentScopes(lsp, this))
{
    // Command palette: View: Toggle Breadcrumbs, View: Toggle Sticky Scroll
    auto *breadcrumbs = new QAction(tr("Toggle Breadcrumbs"), window);
    breadcrumbs->setObjectName(QStringLiteral("actionToggleBreadcrumbs"));
    connect(breadcrumbs, &QAction::triggered, this, [this] {
        toggle(QStringLiteral("breadcrumbs"), !m_breadcrumbs);
    });
    auto *sticky = new QAction(tr("Toggle Sticky Scroll"), window);
    sticky->setObjectName(QStringLiteral("actionToggleStickyScroll"));
    connect(sticky, &QAction::triggered, this, [this] {
        toggle(QStringLiteral("stickyScroll"), !m_sticky);
    });
    for (QAction *action : {breadcrumbs, sticky}) {
        setCommandCategory(action, CommandCategory::View);
        window->addAction(action);
    }
    applySettings();
}

void EditorOutline::toggle(const QString &key, bool on)
{
    const char *comment = key == QLatin1String("breadcrumbs") ? kBreadcrumbsComment : kStickyComment;
    setConfigValue(QStringLiteral("editor"), key, on ? QStringLiteral("true") : QStringLiteral("false"), QCoreApplication::translate("SettingsDialog", comment));
    const auto windows = WindowManager::instance().windows();
    for (MainWindow *window : windows) {
        window->applySettings();
    }
}

void EditorOutline::applySettings()
{
    QSettings &c = config();
    setBreadcrumbs(c.value(QStringLiteral("editor/breadcrumbs"), true).toBool());
    setStickyScroll(c.value(QStringLiteral("editor/stickyScroll"), true).toBool());
}

void EditorOutline::addGroup(TabView *group)
{
    if (!group || m_groups.contains(group)) {
        return;
    }
    m_groups.removeAll(nullptr);
    m_groups.append(group);
    if (m_breadcrumbs) {
        m_bars.insert(group, newBar(group));
    }
    connect(group, &QObject::destroyed, this, [this, group] {
        m_bars.remove(group);
    });
}

void EditorOutline::addView(KTextEditor::View *view)
{
    if (!view || m_views.contains(view)) {
        return;
    }
    m_views.removeAll(nullptr);
    m_views.append(view);
    if (m_sticky) {
        m_overlays.insert(view, new StickyScroll(view, m_scopes));
    }
    connect(view, &KTextEditor::View::focusIn, this, [this, view] {
        m_current = view;
    });
    connect(view, &QObject::destroyed, this, [this, view] {
        m_overlays.remove(view);
    });
}

BreadcrumbBar *EditorOutline::newBar(TabView *group)
{
    return new BreadcrumbBar(
        group,
        m_scopes,
        [this] {
            return m_window->project();
        },
        [this](const QString &path) {
            m_window->openFile(path);
        });
}

void EditorOutline::setBreadcrumbs(bool on)
{
    if (on == m_breadcrumbs) {
        return;
    }
    m_breadcrumbs = on;
    for (const QPointer<TabView> &group : std::as_const(m_groups)) {
        if (!group) {
            continue;
        }
        if (on) {
            m_bars.insert(group, newBar(group));
        } else {
            delete m_bars.take(group).data();
        }
    }
}

void EditorOutline::setStickyScroll(bool on)
{
    if (on == m_sticky) {
        return;
    }
    m_sticky = on;
    for (const QPointer<KTextEditor::View> &view : std::as_const(m_views)) {
        if (!view) {
            continue;
        }
        if (on) {
            m_overlays.insert(view, new StickyScroll(view, m_scopes));
        } else {
            delete m_overlays.take(view).data();
        }
    }
}

KTextEditor::View *EditorOutline::currentView() const
{
    if (m_current && m_current->isVisible()) {
        return m_current;
    }
    for (const QPointer<TabView> &group : m_groups) {
        if (auto *view = group ? qobject_cast<KTextEditor::View *>(group->currentWidget()) : nullptr) {
            return view;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Automated checks

void EditorOutline::runTest(const QString &script)
{
    auto steps = std::make_shared<QStringList>(script.split(QLatin1Char('|'), Qt::SkipEmptyParts));
    auto run = std::make_shared<std::function<void()>>();
    QPointer<EditorOutline> self(this);
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
        qInfo().noquote() << "outline-test step" << step;
        KTextEditor::View *view = currentView();
        BreadcrumbBar *bar = nullptr;
        for (const QPointer<TabView> &group : std::as_const(m_groups)) {
            if (group && view && group->indexOf(view) >= 0) {
                bar = m_bars.value(group);
            }
        }
        StickyScroll *overlay = view ? m_overlays.value(view).data() : nullptr;
        if (name == QLatin1String("sleep")) {
            next(parts.value(1).toInt());
            return;
        }
        if (name == QLatin1String("open")) {
            m_window->openFile(parts.value(1), parts.value(2).toInt() - 1);
            next(300);
            return;
        }
        if (name == QLatin1String("toggle")) {
            const QString action = parts.value(1) == QLatin1String("sticky") ? QStringLiteral("actionToggleStickyScroll") : QStringLiteral("actionToggleBreadcrumbs");
            if (auto *a = m_window->findChild<QAction *>(action)) {
                a->trigger();
            }
            next(200);
            return;
        }
        if (!view) {
            qInfo().noquote() << "outline-test no view";
            next(0);
            return;
        }
        if (name == QLatin1String("top")) { // the line at the top of the view
            view->setScrollPosition(KTextEditor::Cursor(parts.value(1).toInt() - 1, 0));
            next(200);
        } else if (name == QLatin1String("cursor")) {
            view->setCursorPosition(KTextEditor::Cursor(parts.value(1).toInt() - 1, std::max(0, parts.value(2).toInt() - 1)));
            next(300);
        } else if (name == QLatin1String("focus")) {
            view->setFocus();
            next(100);
        } else if (name == QLatin1String("crumbs")) {
            qInfo().noquote() << "outline-test crumbs" << (bar ? (bar->isVisible() ? bar->texts().join(QStringLiteral(" › ")) : QStringLiteral("(hidden)")) : QStringLiteral("(none)"));
            next(0);
        } else if (name == QLatin1String("sticky")) {
            QStringList shown;
            if (overlay) {
                for (const int line : overlay->lines()) {
                    shown << QStringLiteral("%1: %2").arg(line + 1).arg(view->document()->line(line).trimmed());
                }
            }
            qInfo().noquote() << "outline-test sticky" << (overlay ? QString::number(shown.size()) : QStringLiteral("(none)")) << "top"
                              << view->firstDisplayedLine() + 1 << "cursor" << view->cursorPosition().line() + 1 << "|" << shown.join(QStringLiteral(" | "));
            next(0);
        } else if (name == QLatin1String("origin")) { // where the scopes come from
            const auto tree = m_scopes->scopes(view->document());
            qInfo().noquote() << "outline-test origin" << (tree && tree->fromServer ? "server" : "text") << (tree ? int(tree->scopes.size()) : 0) << "scopes";
            next(0);
        } else if (name == QLatin1String("list")) { // list:N opens the list of breadcrumb N (negative: from the end)
            if (bar && bar->openList(parts.value(1).toInt())) {
                qInfo().noquote() << "outline-test list" << bar->listRows().join(QStringLiteral(" | "));
            } else {
                qInfo().noquote() << "outline-test list: none";
            }
            next(300);
        } else if (name == QLatin1String("pick")) {
            qInfo().noquote() << "outline-test pick" << (bar && bar->activateListRow(parts.mid(1).join(QLatin1Char(':'))));
            next(400);
        } else if (name == QLatin1String("click")) { // click:ROW on a sticky line
            if (overlay) {
                overlay->clickRow(parts.value(1).toInt());
            }
            next(300);
        } else if (name == QLatin1String("wrap") || name == QLatin1String("minimap")) {
            view->setConfigValue(name == QLatin1String("wrap") ? QStringLiteral("dynamic-word-wrap") : QStringLiteral("scrollbar-minimap"),
                                 parts.value(1) == QLatin1String("on"));
            next(300);
        } else if (name == QLatin1String("keys")) { // keys:up:5
            const int key = parts.value(1) == QLatin1String("down") ? Qt::Key_Down : parts.value(1) == QLatin1String("pageup") ? Qt::Key_PageUp : Qt::Key_Up;
            QWidget *inner = view->focusProxy() ? view->focusProxy() : view;
            view->setFocus();
            for (int i = 0; i < std::max(1, parts.value(2).toInt()); ++i) {
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::sendEvent(inner, &press);
                QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
                QCoreApplication::sendEvent(inner, &release);
                QCoreApplication::processEvents();
            }
            next(300);
        } else if (name == QLatin1String("type")) { // type:text, "\n" for new lines
            QString text = parts.mid(1).join(QLatin1Char(':'));
            text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
            view->document()->insertText(view->cursorPosition(), text);
            next(300);
        } else if (name == QLatin1String("shot")) { // the window now, next to the final screenshot
            const QString file = qEnvironmentVariable("LIEVE_SCREENSHOT");
            if (!file.isEmpty()) {
                m_window->grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-%1.png").arg(parts.value(1))));
            }
            next(0);
        } else if (name == QLatin1String("bench")) {
            KTextEditor::Document *doc = view->document();
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 20; ++i) {
                m_scopes->heuristic(doc);
            }
            const double heuristicMs = timer.nsecsElapsed() / 20e6;
            double refreshUs = 0;
            int samples = 0;
            if (overlay) {
                const int last = std::max(1, doc->lines() - 60);
                for (int line = 1; line < last; line += std::max(1, last / 200)) {
                    view->setScrollPosition(KTextEditor::Cursor(line, 0));
                    QCoreApplication::processEvents();
                    timer.restart();
                    overlay->refresh();
                    refreshUs += timer.nsecsElapsed() / 1e3;
                    ++samples;
                }
            }
            qInfo().noquote() << "outline-test bench lines" << doc->lines() << "heuristic-ms" << QString::number(heuristicMs, 'f', 2) << "sticky-refresh-us"
                              << QString::number(samples ? refreshUs / samples : 0, 'f', 1) << "samples" << samples;
            next(0);
        } else {
            qInfo().noquote() << "outline-test unknown step" << name;
            next(0);
        }
    };
    (*run)();
}
