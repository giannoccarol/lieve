#include "codelens.h"

#include "lsp.h"
#include "lspclient.h"
#include "lspedits.h"
#include "mainwindow.h"
#include "mergeconflicts.h"
#include "outline.h"
#include "scopes.h"
#include "symbolactions.h"
#include "theme.h"

#include <KTextEditor/Document>
#include <KTextEditor/InlineNote>
#include <KTextEditor/InlineNoteProvider>
#include <KTextEditor/MovingCursor>
#include <KTextEditor/View>

#include <QEvent>
#include <QFontMetrics>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <vector>

namespace Lieve
{
namespace
{
constexpr int kScrollDelay = 300; // ms after scrolling stops
constexpr int kTypingDelay = 1000; // ms after typing stops
constexpr int kMaxInFlight = 2; // counts asked at the same time
constexpr int kMaxPerRefresh = 40; // counts asked for one screen
constexpr int kMaxFailures = 3;

// The notes of each document, for hasNote().
QHash<const KTextEditor::Document *, LensNotes *> &registry()
{
    static QHash<const KTextEditor::Document *, LensNotes *> notes;
    return notes;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t, a.blueF() * (1 - t) + b.blueF() * t);
}

// KTextEditor's text area (KateViewInternal), which gets the mouse events.
QWidget *textArea(const KTextEditor::View *view)
{
    if (QWidget *proxy = view->focusProxy(); proxy && proxy->inherits("KateViewInternal")) {
        return proxy;
    }
    const auto children = view->findChildren<QWidget *>();
    for (QWidget *child : children) {
        if (child->inherits("KateViewInternal")) {
            return child;
        }
    }
    return nullptr;
}

// A little smaller than the code, like the merge conflict actions.
QFont smaller(const QFont &font)
{
    QFont small = font;
    if (font.pointSizeF() > 0) {
        small.setPointSizeF(font.pointSizeF() * 0.9);
    } else if (font.pixelSize() > 0) {
        small.setPixelSize(std::max(1, qRound(font.pixelSize() * 0.9)));
    }
    return small;
}
}

// The counts of one document, drawn by the editor after the end of their
// lines (one provider for all the views of the document).
class LensNotes : public KTextEditor::InlineNoteProvider
{
public:
    struct Lens {
        int id = 0;
        std::unique_ptr<KTextEditor::MovingCursor> position; // the symbol's name, following the edits
        QJsonObject serverLens; // the server's code lens, unresolved
        int count = -1; // not known yet
        qint64 countRevision = -1; // asked at this revision of the document...
        int countGeneration = -1; // ...and of the other files
        bool pending = false;
        int failures = 0;
    };
    struct Segment {
        int lens = -1; // id; -1: separator
        QString text;
        int x = 0;
        int width = 0;
    };
    struct Layout {
        QFont font; // the line's
        QFont small; // the counts'
        QList<Segment> segments;
        int width = 0;
    };

    LensNotes(ReferenceLenses *owner, KTextEditor::Document *document)
        : doc(document)
        , m_owner(owner)
        , m_key(document)
    {
        registry().insert(m_key, this);
    }
    ~LensNotes() override
    {
        if (registry().value(m_key) == this) {
            registry().remove(m_key);
        }
        for (const QPointer<KTextEditor::View> &view : std::as_const(m_registered)) {
            if (view) {
                view->unregisterInlineNoteProvider(this);
            }
        }
        if (!doc) {
            for (Lens &lens : lenses) {
                (void)lens.position.release(); // never touch a dead document's cursors
            }
        }
    }

    void registerView(KTextEditor::View *view)
    {
        m_registered.removeAll(nullptr);
        if (!m_registered.contains(view)) {
            view->registerInlineNoteProvider(this);
            m_registered.append(view);
        }
    }
    KTextEditor::View *viewFor(const KTextEditor::View *view) const
    {
        for (const QPointer<KTextEditor::View> &v : m_registered) {
            if (v == view) {
                return v;
            }
        }
        return nullptr;
    }
    Lens *find(int id)
    {
        for (Lens &lens : lenses) {
            if (lens.id == id) {
                return &lens;
            }
        }
        return nullptr;
    }
    // A new list of lenses (at the current revision); the counts of those
    // at the same places stay until they are asked again.
    void replace(const QList<QPair<KTextEditor::Cursor, QJsonObject>> &items, qint64 revision, int generation, int &nextId)
    {
        std::vector<Lens> old = std::move(lenses);
        lenses.clear();
        QHash<QPair<int, int>, size_t> at;
        for (size_t i = 0; i < old.size(); ++i) {
            if (old[i].position && old[i].position->isValid()) {
                at.insert({old[i].position->line(), old[i].position->column()}, i);
            }
        }
        for (const auto &[cursor, serverLens] : items) {
            Lens lens;
            lens.id = nextId++;
            lens.serverLens = serverLens;
            if (serverLens.contains(QLatin1String("command"))) { // resolved already
                lens.count = referenceCount(serverLens);
                if (lens.count < 0) {
                    continue; // some other lens
                }
                lens.countRevision = revision;
                lens.countGeneration = generation;
            } else if (const auto it = at.constFind({cursor.line(), cursor.column()}); it != at.cend()) {
                const Lens &was = old[*it];
                lens.count = was.count;
                lens.countRevision = was.countRevision;
                lens.countGeneration = was.countGeneration;
                lens.failures = was.failures;
            }
            lens.position.reset(doc->newMovingCursor(cursor, KTextEditor::MovingCursor::MoveOnInsert));
            lenses.push_back(std::move(lens));
        }
        listRevision = revision;
        changed();
    }
    // Reloaded or another server: the cursors go while the text is whole.
    void clear()
    {
        lenses.clear();
        listRevision = -1;
        changed();
    }
    void changed()
    {
        m_lineRevision = -2;
        Q_EMIT inlineNotesReset();
    }
    void countChanged(int line)
    {
        m_lineRevision = -2;
        Q_EMIT inlineNotesChanged(line);
    }

    // The lenses with a count on `line`, left to right (indexes in `lenses`).
    QList<int> onLine(int line) const
    {
        if (!doc) {
            return {};
        }
        if (m_lineRevision != doc->revision()) {
            m_byLine.clear();
            for (size_t i = 0; i < lenses.size(); ++i) {
                const Lens &lens = lenses[i];
                if (lens.count >= 0 && lens.position && lens.position->isValid()) {
                    m_byLine[lens.position->line()].append(int(i));
                }
            }
            for (QList<int> &list : m_byLine) {
                std::sort(list.begin(), list.end(), [this](int a, int b) {
                    return lenses[size_t(a)].position->column() < lenses[size_t(b)].position->column();
                });
            }
            m_lineRevision = doc->revision();
        }
        return m_byLine.value(line);
    }

    Layout layoutFor(int line, const QFont &font) const
    {
        Layout layout;
        layout.font = font;
        layout.small = smaller(font);
        const QFontMetrics label(layout.font);
        const QFontMetrics small(layout.small);
        int x = label.horizontalAdvance(QStringLiteral("    ")); // the inline blame's gap
        const QList<int> on = onLine(line);
        for (const int i : on) {
            if (!layout.segments.isEmpty()) {
                const QString bar = QStringLiteral(" | ");
                layout.segments.append({-1, bar, x, small.horizontalAdvance(bar)});
                x += layout.segments.last().width;
            }
            const Lens &lens = lenses[size_t(i)];
            const QString text = lens.count == 1 ? ReferenceLenses::tr("1 reference") : ReferenceLenses::tr("%1 references").arg(lens.count);
            layout.segments.append({lens.id, text, x, small.horizontalAdvance(text)});
            x += layout.segments.last().width;
        }
        layout.width = x;
        return layout;
    }

    QList<int> inlineNotes(int line) const override
    {
        if (!doc || onLine(line).isEmpty() || MergeConflicts::hasNote(doc, line)) {
            return {};
        }
        return {doc->lineLength(line)};
    }
    QSize inlineNoteSize(const KTextEditor::InlineNote &note) const override
    {
        return QSize(layoutFor(note.position().line(), note.font()).width, note.lineHeight());
    }
    void paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter, Qt::LayoutDirection) const override
    {
        const int line = note.position().line();
        const Layout layout = layoutFor(line, note.font());
        const QFontMetrics metrics(layout.font);
        const int baseline = (note.lineHeight() - metrics.height()) / 2 + metrics.ascent(); // the line's own
        const Colors &c = Theme::colors();
        const bool themed = c.fg.isValid() && c.editor.isValid();
        const QColor dim = themed ? mix(c.fg, c.editor, 0.55) : QColor(Qt::gray); // as quiet as the inline blame
        const QColor bright = themed ? c.fg : painter.pen().color();
        for (const Segment &segment : layout.segments) {
            const bool hovered = segment.lens >= 0 && note.view() == m_hoverView && line == m_hoverLine && segment.lens == m_hoverLens;
            QFont font = layout.small;
            font.setUnderline(hovered);
            painter.setFont(font);
            painter.setPen(hovered ? bright : dim);
            painter.drawText(QPoint(segment.x, baseline), segment.text);
        }
    }
    void inlineNoteActivated(const KTextEditor::InlineNote &note, Qt::MouseButtons buttons, const QPoint &globalPos) override
    {
        const int lens = buttons & Qt::LeftButton ? lensAt(note, globalPos) : -1;
        KTextEditor::View *view = viewFor(note.view());
        if (lens < 0 || !view) {
            return;
        }
        // Once the editor is done with the click.
        QPointer<KTextEditor::View> v(view);
        QPointer<LensNotes> self(this);
        ReferenceLenses *owner = m_owner;
        QTimer::singleShot(0, owner, [owner, v, self, lens] {
            if (v && self) {
                owner->openLens(v, self, lens);
            }
        });
    }
    void inlineNoteFocusInEvent(const KTextEditor::InlineNote &note, const QPoint &globalPos) override
    {
        hover(note.view(), note.position().line(), lensAt(note, globalPos));
    }
    void inlineNoteMouseMoveEvent(const KTextEditor::InlineNote &note, const QPoint &globalPos) override
    {
        hover(note.view(), note.position().line(), lensAt(note, globalPos));
    }
    void inlineNoteFocusOutEvent(const KTextEditor::InlineNote &note) override
    {
        hover(note.view(), note.position().line(), -1);
    }

    QPointer<KTextEditor::Document> doc;
    std::vector<Lens> lenses; // in the server's (or the symbols') order
    bool fromServer = true; // code lenses, else document symbols and references
    qint64 listRevision = -1; // the revision `lenses` were listed at
    qint64 listFailedRevision = -1;
    int listRequest = -1;
    QPointer<LspClient> listClient;

protected:
    // The pointer left the editor straight from a count: the editor does
    // not tell the note.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Leave) {
            watched->removeEventFilter(this);
            if (m_hoverView) {
                hover(m_hoverView, -1, -1);
            }
        }
        return false;
    }

private:
    int lensAt(const KTextEditor::InlineNote &note, const QPoint &globalPos) const
    {
        const KTextEditor::View *view = note.view();
        const QPoint origin = view->cursorToCoordinate(note.position()); // the note starts there, after the text
        if (origin.x() < 0) {
            return -1;
        }
        const int x = view->mapFromGlobal(globalPos).x() - origin.x();
        const Layout layout = layoutFor(note.position().line(), note.font());
        for (const Segment &segment : layout.segments) {
            if (segment.lens >= 0 && x >= segment.x && x < segment.x + segment.width) {
                return segment.lens;
            }
        }
        return -1;
    }
    void hover(const KTextEditor::View *view, int line, int lens)
    {
        if (lens < 0) {
            line = -1;
        }
        if (view == m_hoverView && line == m_hoverLine && lens == m_hoverLens) {
            return;
        }
        if (m_hoverView && m_hoverView != view) {
            hover(m_hoverView, -1, -1);
        }
        const bool wasLink = m_hoverLens >= 0;
        m_hoverView = lens >= 0 ? view : nullptr;
        m_hoverLine = line;
        m_hoverLens = lens;
        // Repainting is enough; the editor puts its own I-beam back only
        // when it changes its mind, so it is set again here.
        if (QWidget *area = view ? textArea(view) : nullptr) {
            if (lens >= 0) {
                area->setCursor(Qt::PointingHandCursor);
                area->installEventFilter(this);
            } else if (wasLink) {
                area->setCursor(Qt::IBeamCursor);
            }
            area->update();
        }
    }

    ReferenceLenses *m_owner;
    const KTextEditor::Document *m_key;
    QList<QPointer<KTextEditor::View>> m_registered;
    mutable QHash<int, QList<int>> m_byLine;
    mutable qint64 m_lineRevision = -2;
    QPointer<const KTextEditor::View> m_hoverView;
    int m_hoverLine = -1;
    int m_hoverLens = -1;
};

// ---------------------------------------------------------------------------

ReferenceLenses::ReferenceLenses(SymbolActions *actions)
    : QObject(actions)
    , m_actions(actions)
    , m_lsp(actions->lsp())
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ReferenceLenses::refreshPending);
}

ReferenceLenses::~ReferenceLenses()
{
    m_notes.clear(); // the notes leave the views and their cursors go while the documents live
}

bool ReferenceLenses::hasNote(const KTextEditor::Document *doc, int line)
{
    const LensNotes *notes = registry().value(doc);
    return notes && !notes->inlineNotes(line).isEmpty();
}

LensNotes *ReferenceLenses::notesFor(KTextEditor::Document *doc, bool create)
{
    if (const auto it = m_notes.find(doc); it != m_notes.end()) {
        return it->second.get();
    }
    if (!create) {
        return nullptr;
    }
    LensNotes *notes = m_notes.emplace(doc, std::make_unique<LensNotes>(this, doc)).first->second.get();
    const QList<KTextEditor::View *> views = viewsOf(doc);
    for (KTextEditor::View *view : views) {
        notes->registerView(view);
    }
    return notes;
}

QList<KTextEditor::View *> ReferenceLenses::viewsOf(const KTextEditor::Document *doc) const
{
    QList<KTextEditor::View *> out;
    for (const QPointer<KTextEditor::View> &view : m_views) {
        if (view && view->document() == doc) {
            out.append(view);
        }
    }
    return out;
}

void ReferenceLenses::addView(KTextEditor::View *view)
{
    m_views.removeAll(nullptr);
    if (!view || m_views.contains(view)) {
        return;
    }
    m_views.append(view);
    connect(view, &KTextEditor::View::displayRangeChanged, this, [this, view] {
        schedule(view, kScrollDelay);
    });
    KTextEditor::Document *doc = view->document();
    if (!m_watched.contains(doc)) {
        m_watched.insert(doc);
        connect(doc, &KTextEditor::Document::textChanged, this, [this](KTextEditor::Document *d) {
            if (notesFor(d, false)) {
                m_lastEdit.start();
                const QList<KTextEditor::View *> views = viewsOf(d);
                for (KTextEditor::View *v : views) {
                    schedule(v, kTypingDelay);
                }
            }
        });
        connect(doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, [this](KTextEditor::Document *d) {
            if (LensNotes *notes = notesFor(d, false)) {
                notes->clear();
            }
        });
        connect(doc, &QObject::destroyed, this, [this, doc] {
            m_watched.remove(doc);
            m_notes.erase(doc); // its views and text went first: nothing of it is touched
        });
    }
    if (LensNotes *notes = notesFor(doc, false)) {
        notes->registerView(view);
    }
    schedule(view, kScrollDelay);
}

void ReferenceLenses::removeDocument(KTextEditor::Document *doc)
{
    m_notes.erase(doc);
    m_queue.removeIf([](const Job &job) {
        return !job.notes;
    });
}

void ReferenceLenses::filesChanged()
{
    ++m_generation;
    scheduleAll(kScrollDelay);
}

void ReferenceLenses::schedule(KTextEditor::View *view, int delay)
{
    if (!view) {
        return;
    }
    if (!m_pending.contains(view)) {
        m_pending.append(view);
    }
    if (m_lastEdit.isValid() && m_lastEdit.elapsed() < kTypingDelay) { // still typing: later
        delay = std::max(delay, int(kTypingDelay - m_lastEdit.elapsed()));
    }
    m_timer.start(delay);
}

void ReferenceLenses::scheduleAll(int delay)
{
    for (const QPointer<KTextEditor::View> &view : std::as_const(m_views)) {
        if (view && view->isVisible()) {
            schedule(view, delay);
        }
    }
}

void ReferenceLenses::refreshPending()
{
    const QList<QPointer<KTextEditor::View>> views = std::exchange(m_pending, {});
    for (const QPointer<KTextEditor::View> &view : views) {
        if (view && view->isVisible()) { // hidden ones when they are shown again
            refresh(view);
        }
    }
}

void ReferenceLenses::refresh(KTextEditor::View *view)
{
    KTextEditor::Document *doc = view->document();
    LspClient *client = m_lsp->readyClient(doc);
    const bool lenses = client && client->hasCapability(QStringLiteral("codeLensProvider"));
    const bool symbols = client && !lenses && client->hasCapability(QStringLiteral("referencesProvider"))
        && client->hasCapability(QStringLiteral("documentSymbolProvider"));
    if (!lenses && !symbols) {
        if (notesFor(doc, false)) {
            m_notes.erase(doc); // no server any more: nothing shown
        }
        return;
    }
    LensNotes *notes = notesFor(doc, true);
    if (notes->fromServer != lenses) {
        notes->clear();
        notes->fromServer = lenses;
    }
    if (notes->listRevision != doc->revision()) {
        if (lenses) {
            configure(client);
            requestList(notes, client); // goes on when it answers
            return;
        }
        listFromSymbols(notes);
        if (notes->listRevision != doc->revision()) {
            return; // when the symbols come (DocumentScopes::changed)
        }
    }
    queueVisible(notes);
    pump();
}

// typescript-language-server makes reference lenses only when told to
// (VS Code's typescript.referencesCodeLens settings), once per server.
void ReferenceLenses::configure(LspClient *client)
{
    if (client->spec().id != QLatin1String("typescript") || m_configured.contains(client)) {
        return;
    }
    m_configured.insert(client);
    if (!m_seen.contains(client)) {
        m_seen.insert(client);
        connect(client, &LspClient::ready, this, [this, client] {
            m_configured.remove(client); // started again: without them
        });
        connect(client, &QObject::destroyed, this, [this, client] {
            m_configured.remove(client);
            m_seen.remove(client);
        });
    }
    const QJsonObject language{
        {QStringLiteral("referencesCodeLens"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("showOnAllFunctions"), true}}}};
    client->notify(QStringLiteral("workspace/didChangeConfiguration"),
                   QJsonObject{{QStringLiteral("settings"), QJsonObject{{QStringLiteral("typescript"), language}, {QStringLiteral("javascript"), language}}}});
}

void ReferenceLenses::requestList(LensNotes *notes, LspClient *client)
{
    KTextEditor::Document *doc = notes->doc;
    if ((notes->listRequest >= 0 && notes->listClient) || notes->listFailedRevision == doc->revision()) {
        return;
    }
    m_lsp->flush(doc); // the server has this revision
    const qint64 revision = doc->revision();
    QPointer<LensNotes> n(notes);
    notes->listClient = client;
    notes->listRequest = client->request(
        QStringLiteral("textDocument/codeLens"),
        QJsonObject{{QStringLiteral("textDocument"), m_lsp->positionParams(doc, KTextEditor::Cursor(0, 0)).value(QLatin1String("textDocument"))}},
        this,
        [this, n, revision](const QJsonValue &result, bool ok) {
            if (!n || !n->doc) {
                return;
            }
            n->listRequest = -1;
            KTextEditor::Document *doc = n->doc;
            if (doc->revision() != revision) { // typed meanwhile: once it stops
                const QList<KTextEditor::View *> views = viewsOf(doc);
                for (KTextEditor::View *v : views) {
                    schedule(v, kTypingDelay);
                }
                return;
            }
            if (!ok) {
                n->listFailedRevision = revision;
                return;
            }
            QList<QPair<KTextEditor::Cursor, QJsonObject>> items;
            const QJsonArray list = result.toArray();
            for (const QJsonValue &value : list) {
                const QJsonObject lens = value.toObject();
                const QJsonObject start = lens.value(QLatin1String("range")).toObject().value(QLatin1String("start")).toObject();
                items.append({KTextEditor::Cursor(start.value(QLatin1String("line")).toInt(), start.value(QLatin1String("character")).toInt()), lens});
            }
            n->replace(items, revision, m_generation, m_nextLens);
            queueVisible(n);
            pump();
        },
        10000);
}

// Without code lenses: the server's document symbols, as the breadcrumbs have them.
void ReferenceLenses::listFromSymbols(LensNotes *notes)
{
    DocumentScopes *documentScopes = scopes();
    const std::shared_ptr<const ScopeTree> tree = documentScopes ? documentScopes->scopes(notes->doc) : nullptr;
    if (!tree || !tree->fromServer) {
        return;
    }
    QList<QPair<KTextEditor::Cursor, QJsonObject>> items;
    for (const Scope &s : tree->scopes) {
        if (s.name.isEmpty()) {
            continue;
        }
        int parentKind = 0;
        for (int p = s.parent; p >= 0; p = tree->scopes[size_t(p)].parent) {
            if (!tree->scopes[size_t(p)].name.isEmpty()) {
                parentKind = tree->scopes[size_t(p)].kind;
                break;
            }
        }
        if (wantsReferenceCount(s.kind, parentKind)) {
            items.append({KTextEditor::Cursor(s.targetLine, s.targetColumn), QJsonObject()});
        }
    }
    notes->replace(items, notes->doc->revision(), m_generation, m_nextLens);
}

DocumentScopes *ReferenceLenses::scopes()
{
    if (!m_scopes) {
        if (auto *outline = m_actions->window()->findChild<EditorOutline *>()) {
            m_scopes = outline->scopes();
            connect(m_scopes, &DocumentScopes::changed, this, [this](KTextEditor::Document *doc) {
                if (LensNotes *notes = notesFor(doc, false); notes && !notes->fromServer) {
                    notes->listRevision = -1; // newer symbols
                    const QList<KTextEditor::View *> views = viewsOf(doc);
                    for (KTextEditor::View *v : views) {
                        schedule(v, 0);
                    }
                }
            });
        }
    }
    return m_scopes;
}

// The counts to ask for: those on screen not known for this revision.
void ReferenceLenses::queueVisible(LensNotes *notes)
{
    m_queue.removeIf([notes](const Job &job) {
        return !job.notes || job.notes == notes; // scrolled away
    });
    KTextEditor::Document *doc = notes->doc;
    const qint64 revision = doc->revision();
    int added = 0;
    const QList<KTextEditor::View *> views = viewsOf(doc);
    for (KTextEditor::View *view : views) {
        if (!view->isVisible()) {
            continue;
        }
        const int first = view->firstDisplayedLine();
        const int last = view->lastDisplayedLine();
        for (const LensNotes::Lens &lens : notes->lenses) {
            if (added >= kMaxPerRefresh) {
                return;
            }
            if (lens.pending || lens.failures >= kMaxFailures || !lens.position || !lens.position->isValid()) {
                continue;
            }
            const int line = lens.position->line();
            const bool asked = lens.countRevision == revision && lens.countGeneration == m_generation;
            if (line < first || line > last || asked) {
                continue;
            }
            const bool queued = std::any_of(m_queue.cbegin(), m_queue.cend(), [notes, &lens](const Job &job) {
                return job.notes == notes && job.lens == lens.id;
            });
            if (!queued) {
                m_queue.append({notes, lens.id});
                ++added;
            }
        }
    }
}

void ReferenceLenses::pump()
{
    while (m_inFlight < kMaxInFlight && !m_queue.isEmpty()) {
        const Job job = m_queue.takeFirst();
        LensNotes *notes = job.notes;
        if (!notes || !notes->doc) {
            continue;
        }
        const LensNotes::Lens *lens = notes->find(job.lens);
        LspClient *client = m_lsp->readyClient(notes->doc);
        if (!lens || lens->pending || !client || (notes->fromServer && notes->listRevision != notes->doc->revision())) {
            continue; // an edit since: the list is asked again first
        }
        resolve(notes, job.lens, client);
    }
}

void ReferenceLenses::resolve(LensNotes *notes, int lensId, LspClient *client)
{
    LensNotes::Lens *lens = notes->find(lensId);
    KTextEditor::Document *doc = notes->doc;
    lens->pending = true;
    ++m_inFlight;
    const qint64 revision = doc->revision();
    const int generation = m_generation;
    QPointer<LensNotes> n(notes);
    if (notes->fromServer) {
        client->request(
            QStringLiteral("codeLens/resolve"),
            lens->serverLens,
            this,
            [this, n, lensId, revision, generation](const QJsonValue &result, bool ok) {
                finished(n, lensId, revision, generation, ok ? referenceCount(result.toObject()) : -1);
            },
            20000);
        return;
    }
    m_lsp->flush(doc);
    QJsonObject params = m_lsp->positionParams(doc, lens->position->toCursor());
    params.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("includeDeclaration"), false}});
    client->request(
        QStringLiteral("textDocument/references"),
        params,
        this,
        [this, n, lensId, revision, generation](const QJsonValue &result, bool ok) {
            finished(n, lensId, revision, generation, ok && result.isArray() ? int(result.toArray().size()) : -1);
        },
        20000);
}

void ReferenceLenses::finished(LensNotes *notes, int lensId, qint64 revision, int generation, int count)
{
    --m_inFlight;
    LensNotes::Lens *lens = notes && notes->doc ? notes->find(lensId) : nullptr;
    if (lens) {
        lens->pending = false;
        if (count >= 0) {
            const bool changed = lens->count != count;
            lens->count = count;
            lens->countRevision = revision;
            lens->countGeneration = generation;
            lens->failures = 0;
            if (changed && lens->position->isValid()) {
                notes->countChanged(lens->position->line());
            }
        } else if (notes->doc->revision() == revision) { // not cancelled by an edit
            ++lens->failures;
            lens->countRevision = revision;
            lens->countGeneration = generation;
        }
    }
    pump();
}

void ReferenceLenses::openLens(KTextEditor::View *view, LensNotes *notes, int lensId)
{
    const LensNotes::Lens *lens = notes->find(lensId);
    if (lens && lens->position && lens->position->isValid()) {
        m_actions->findReferences(view, lens->position->toCursor(), false);
    }
}

// ---------------------------------------------------------------------------
// For the checks

QList<ReferenceLenses::Shown> ReferenceLenses::shown(KTextEditor::View *view) const
{
    QList<Shown> out;
    const auto it = view ? m_notes.find(view->document()) : m_notes.end();
    if (it == m_notes.end()) {
        return out;
    }
    const LensNotes *notes = it->second.get();
    const int last = view->lastDisplayedLine();
    for (int line = view->firstDisplayedLine(); line <= last; ++line) {
        const QList<int> on = notes->onLine(line);
        if (on.isEmpty()) {
            continue;
        }
        Shown s;
        s.line = line;
        for (const int i : on) {
            s.counts << notes->lenses[size_t(i)].count;
        }
        out.append(s);
    }
    return out;
}

bool ReferenceLenses::idle() const
{
    const bool listing = std::any_of(m_notes.cbegin(), m_notes.cend(), [](const auto &entry) {
        return entry.second->listRequest >= 0 && entry.second->listClient;
    });
    return !m_timer.isActive() && m_inFlight == 0 && m_queue.isEmpty() && !listing;
}

QRect ReferenceLenses::noteRect(KTextEditor::View *view, int line, int segment) const
{
    const auto it = view ? m_notes.find(view->document()) : m_notes.end();
    if (it == m_notes.end() || line < 0 || line >= view->document()->lines()) {
        return {};
    }
    const QPoint origin = view->cursorToCoordinate(KTextEditor::Cursor(line, view->document()->lineLength(line)));
    if (origin.x() < 0) {
        return {};
    }
    const QFont font = view->configValue(QStringLiteral("font")).value<QFont>();
    const LensNotes::Layout layout = it->second->layoutFor(line, font);
    int n = 0;
    for (const LensNotes::Segment &s : layout.segments) {
        if (s.lens >= 0 && n++ == segment) {
            return QRect(origin.x() + s.x, origin.y(), s.width, QFontMetrics(font).height());
        }
    }
    return {};
}

void ReferenceLenses::open(KTextEditor::View *view, int line, int segment)
{
    LensNotes *notes = view ? notesFor(view->document(), false) : nullptr;
    const QList<int> on = notes ? notes->onLine(line) : QList<int>();
    if (segment >= 0 && segment < on.size()) {
        openLens(view, notes, notes->lenses[size_t(on.at(segment))].id);
    }
}
}
