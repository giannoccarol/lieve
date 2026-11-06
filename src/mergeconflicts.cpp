#include "mergeconflicts.h"

#include "commandpalette.h"
#include "theme.h"

#include <KActionCollection>
#include <KTextEditor/Document>
#include <KTextEditor/InlineNote>
#include <KTextEditor/InlineNoteProvider>
#include <KTextEditor/MovingRange>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QTextFormat>

#include <algorithm>

using namespace Lieve;

namespace
{
constexpr int kScanDelay = 250; // ms after the last edit

// The palette shows them as "Merge Conflict: Next Conflict", like VS Code.
const char kCategory[] = QT_TRANSLATE_NOOP("CommandPalette", "Merge Conflict");
const char *const kCommandNames[] = {
    "merge_conflict_next",
    "merge_conflict_previous",
    "merge_conflict_accept_all_current",
    "merge_conflict_accept_all_incoming",
    "merge_conflict_accept_all_both",
};

enum Style {
    CurrentHeader,
    CurrentContent,
    BaseHeader,
    BaseContent,
    IncomingContent,
    IncomingHeader,
};

ConflictSide sideOf(int action)
{
    return action == 0 ? ConflictSide::Current : action == 1 ? ConflictSide::Incoming : ConflictSide::Both;
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

// What a note shows, left to right, in note coordinates: the label after the
// marker ("(Current Change)") and on the <<<<<<< line the actions, a little
// smaller like VS Code's code lenses.
struct NoteLayout {
    struct Segment {
        QString text;
        int x = 0;
        int width = 0;
        int action = -1; // -1: label or separator
        bool small = false;
    };
    QFont labelFont;
    QFont actionFont;
    QList<Segment> segments;
    int width = 0;
};

NoteLayout noteLayout(bool start, bool actions, const QFont &font)
{
    NoteLayout layout;
    layout.labelFont = font;
    layout.actionFont = font;
    if (font.pointSizeF() > 0) {
        layout.actionFont.setPointSizeF(font.pointSizeF() * 0.9);
    } else if (font.pixelSize() > 0) {
        layout.actionFont.setPixelSize(std::max(1, qRound(font.pixelSize() * 0.9)));
    }
    const QFontMetrics label(layout.labelFont);
    const QFontMetrics small(layout.actionFont);
    int x = 0;
    const auto add = [&](const QString &text, int action, bool isSmall) {
        const int width = (isSmall ? small : label).horizontalAdvance(text);
        layout.segments.append({text, x, width, action, isSmall});
        x += width;
    };
    add(QLatin1Char(' ') + (start ? MergeConflicts::tr("(Current Change)") : MergeConflicts::tr("(Incoming Change)")), -1, false);
    if (start && actions) {
        x += label.horizontalAdvance(QStringLiteral("   "));
        add(MergeConflicts::tr("Accept Current Change"), 0, true);
        add(QStringLiteral(" | "), -1, true);
        add(MergeConflicts::tr("Accept Incoming Change"), 1, true);
        add(QStringLiteral(" | "), -1, true);
        add(MergeConflicts::tr("Accept Both Changes"), 2, true);
    }
    layout.width = x;
    return layout;
}
}

// The labels and actions on the marker lines, drawn by the editor after the
// end of the line (one provider for all the views of a document).
class ConflictNotes : public KTextEditor::InlineNoteProvider
{
public:
    explicit ConflictNotes(MergeConflicts *owner)
        : m_owner(owner)
    {
    }

    QList<int> inlineNotes(int line) const override
    {
        if (!m_owner->m_document || m_owner->noteAt(line) == MergeConflicts::NoNote) {
            return {};
        }
        return {m_owner->m_document->lineLength(line)};
    }
    QSize inlineNoteSize(const KTextEditor::InlineNote &note) const override
    {
        return QSize(layoutFor(note).width, note.lineHeight());
    }
    void paintInlineNote(const KTextEditor::InlineNote &note, QPainter &painter, Qt::LayoutDirection) const override
    {
        const NoteLayout layout = layoutFor(note);
        const QFontMetrics metrics(layout.labelFont);
        const int baseline = (note.lineHeight() - metrics.height()) / 2 + metrics.ascent(); // the line's own
        const Colors &c = Theme::colors();
        const bool themed = c.fg.isValid() && c.editor.isValid();
        // A little brighter than the inline blame: it sits on a tinted line.
        const QColor dim = themed ? mix(c.fg, c.editor, 0.45) : QColor(Qt::gray);
        const QColor bright = themed ? c.fg : painter.pen().color();
        for (const NoteLayout::Segment &segment : layout.segments) {
            const bool hovered = segment.action >= 0 && note.view() == m_hoverView && note.position().line() == m_hoverLine
                && segment.action == m_hoverAction;
            QFont font = segment.small ? layout.actionFont : layout.labelFont;
            font.setUnderline(hovered);
            painter.setFont(font);
            painter.setPen(hovered ? bright : dim);
            painter.drawText(QPoint(segment.x, baseline), segment.text);
        }
    }
    void inlineNoteActivated(const KTextEditor::InlineNote &note, Qt::MouseButtons buttons, const QPoint &globalPos) override
    {
        const int action = buttons & Qt::LeftButton ? actionAt(note, globalPos) : -1;
        if (action < 0) {
            return;
        }
        const int line = note.position().line();
        MergeConflicts *owner = m_owner;
        // Once the editor is done with the click: the edit changes its lines.
        QTimer::singleShot(0, owner, [owner, line, action] {
            owner->update();
            owner->accept(owner->indexAt(line), sideOf(action));
        });
    }
    void inlineNoteFocusInEvent(const KTextEditor::InlineNote &note, const QPoint &globalPos) override
    {
        hover(note.view(), note.position().line(), actionAt(note, globalPos));
    }
    void inlineNoteMouseMoveEvent(const KTextEditor::InlineNote &note, const QPoint &globalPos) override
    {
        hover(note.view(), note.position().line(), actionAt(note, globalPos));
    }
    void inlineNoteFocusOutEvent(const KTextEditor::InlineNote &note) override
    {
        hover(note.view(), note.position().line(), -1);
    }
    void resetHover()
    {
        if (m_hoverView) {
            hover(m_hoverView, -1, -1);
        }
    }
    NoteLayout layoutFor(const KTextEditor::InlineNote &note) const
    {
        const KTextEditor::Document *doc = m_owner->m_document;
        return noteLayout(m_owner->noteAt(note.position().line()) == MergeConflicts::StartNote, doc && doc->isReadWrite(), note.font());
    }

protected:
    // The pointer left the editor straight from a link: the editor does not
    // tell the note.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Leave) {
            watched->removeEventFilter(this);
            resetHover();
        }
        return false;
    }

private:
    int actionAt(const KTextEditor::InlineNote &note, const QPoint &globalPos) const
    {
        const KTextEditor::View *view = note.view();
        const QPoint origin = view->cursorToCoordinate(note.position()); // the note starts there, after the text
        if (origin.x() < 0) {
            return -1;
        }
        const int x = view->mapFromGlobal(globalPos).x() - origin.x();
        const NoteLayout layout = layoutFor(note);
        for (const NoteLayout::Segment &segment : layout.segments) {
            if (segment.action >= 0 && x >= segment.x && x < segment.x + segment.width) {
                return segment.action;
            }
        }
        return -1;
    }
    void hover(const KTextEditor::View *view, int line, int action)
    {
        if (action < 0) {
            line = -1;
        }
        if (view == m_hoverView && line == m_hoverLine && action == m_hoverAction) {
            return;
        }
        if (m_hoverView && m_hoverView != view) {
            hover(m_hoverView, -1, -1);
        }
        const bool wasLink = m_hoverAction >= 0;
        m_hoverView = action >= 0 ? view : nullptr;
        m_hoverLine = line;
        m_hoverAction = action;
        // Repainting is enough (no new layout); the editor puts its own
        // I-beam back only when it changes its mind, so it is set again here.
        if (QWidget *area = view ? textArea(view) : nullptr) {
            if (action >= 0) {
                area->setCursor(Qt::PointingHandCursor);
                area->installEventFilter(this);
            } else if (wasLink) {
                area->setCursor(Qt::IBeamCursor);
            }
            area->update();
        }
    }

    MergeConflicts *m_owner;
    QPointer<const KTextEditor::View> m_hoverView;
    int m_hoverLine = -1;
    int m_hoverAction = -1;
};

MergeConflicts *MergeConflicts::watch(KTextEditor::Document *document)
{
    if (MergeConflicts *existing = of(document)) {
        return existing;
    }
    auto *conflicts = new MergeConflicts(document);
    conflicts->scan();
    return conflicts;
}

MergeConflicts *MergeConflicts::of(const KTextEditor::Document *document)
{
    return document ? document->findChild<MergeConflicts *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

bool MergeConflicts::hasNote(const KTextEditor::Document *document, int line)
{
    const MergeConflicts *conflicts = of(document);
    return conflicts && conflicts->noteAt(line) != NoNote;
}

MergeConflicts::MergeConflicts(KTextEditor::Document *document)
    : QObject(document)
    , m_document(document)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kScanDelay);
    connect(&m_timer, &QTimer::timeout, this, &MergeConflicts::scan);
    // An edit matters only if conflicts are there already or it touches a
    // marker line: other typing costs a look at the edited line.
    connect(document, &KTextEditor::Document::textInserted, this, [this](KTextEditor::Document *, KTextEditor::Cursor position, const QString &text) {
        checkLines(position.line(), position.line() + int(text.count(QLatin1Char('\n'))));
    });
    connect(document, &KTextEditor::Document::textRemoved, this, [this](KTextEditor::Document *, KTextEditor::Range range, const QString &) {
        checkLines(range.start().line(), range.start().line());
    });
    connect(document, &KTextEditor::Document::lineWrapped, this, [this](KTextEditor::Document *, KTextEditor::Cursor position) {
        checkLines(position.line(), position.line() + 1);
    });
    connect(document, &KTextEditor::Document::lineUnwrapped, this, [this](KTextEditor::Document *, int line) {
        checkLines(line - 1, line);
    });
    connect(document, &KTextEditor::Document::reloaded, this, &MergeConflicts::scan);
    // Reload, close and the document's end (emitted before its views and
    // text go): the ranges are ours to delete while the document is whole.
    connect(document, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, &MergeConflicts::clear);
    connect(document, &KTextEditor::Document::viewCreated, this, [this](KTextEditor::Document *, KTextEditor::View *view) {
        if (!m_conflicts.isEmpty()) {
            attach(view); // a split of a file with conflicts
        }
    });
}

MergeConflicts::~MergeConflicts()
{
    if (m_document) { // not through the document's own end (which cleared already)
        clear();
    }
}

void MergeConflicts::checkLines(int from, int to)
{
    if (!m_document) {
        return;
    }
    if (!m_conflicts.isEmpty()) {
        m_timer.start();
        return;
    }
    const int lines = m_document->lines();
    for (int line = std::max(0, from); line <= to && line < lines; ++line) {
        if (isAnyConflictMarker(m_document->line(line))) {
            m_timer.start();
            return;
        }
    }
}

void MergeConflicts::update()
{
    if (m_timer.isActive()) {
        scan();
    }
}

void MergeConflicts::scan()
{
    m_timer.stop();
    KTextEditor::Document *doc = m_document;
    if (!doc) {
        return;
    }
    const QList<Conflict> found = findConflicts(doc->lines(), [doc](int line) {
        return doc->line(line);
    });
    if (found == m_conflicts && intact(found)) {
        return;
    }
    rebuild(found);
}

bool MergeConflicts::intact(const QList<Conflict> &conflicts) const
{
    if (size_t(conflicts.size()) != m_blocks.size()) {
        return false;
    }
    for (qsizetype i = 0; i < conflicts.size(); ++i) {
        const Block &block = m_blocks.at(size_t(i));
        if (!block.start || !block.end || block.start->start().line() != conflicts.at(i).start || block.end->start().line() != conflicts.at(i).end) {
            return false;
        }
        // A side emptied and typed again: its range is gone, colour it again.
        for (const auto &section : block.sections) {
            if (!section->toRange().isValid()) {
                return false;
            }
        }
    }
    return true;
}

std::unique_ptr<KTextEditor::MovingRange> MergeConflicts::tint(int from, int to, const KTextEditor::Attribute::Ptr &style)
{
    const int lines = m_document->lines();
    if (from >= to || from >= lines) {
        return nullptr;
    }
    // Up to the start of the next line, so the whole width is coloured.
    const KTextEditor::Range range =
        to < lines ? KTextEditor::Range(from, 0, to, 0) : KTextEditor::Range(from, 0, lines - 1, m_document->lineLength(lines - 1));
    std::unique_ptr<KTextEditor::MovingRange> moving(
        m_document->newMovingRange(range, KTextEditor::MovingRange::DoNotExpand, KTextEditor::MovingRange::InvalidateIfEmpty));
    moving->setAttribute(style);
    return moving;
}

void MergeConflicts::rebuild(const QList<Conflict> &conflicts)
{
    m_blocks.clear();
    m_conflicts = conflicts;
    if (!conflicts.isEmpty() && !m_styles[0]) {
        // VS Code's merge colours (merge.currentHeaderBackground...), a touch
        // lighter, see-through over any editor theme, dark or light: the
        // marker lines stronger, the text between them lighter.
        const auto style = [](QColor color, int alpha) {
            color.setAlpha(alpha);
            KTextEditor::Attribute::Ptr a(new KTextEditor::Attribute());
            a->setBackground(color);
            a->setBackgroundFillWhitespace(true);
            a->setProperty(QTextFormat::FullWidthSelection, true);
            return a;
        };
        const QColor current(64, 200, 174);
        const QColor incoming(64, 166, 255);
        const QColor base(96, 96, 96);
        m_styles[CurrentHeader] = style(current, 110);
        m_styles[CurrentContent] = style(current, 44);
        m_styles[BaseHeader] = style(base, 88);
        m_styles[BaseContent] = style(base, 35);
        m_styles[IncomingContent] = style(incoming, 44);
        m_styles[IncomingHeader] = style(incoming, 110);
    }
    for (const Conflict &c : conflicts) {
        Block block;
        block.start = tint(c.start, c.start + 1, m_styles[CurrentHeader]);
        const int currentEnd = c.base >= 0 ? c.base : c.separator;
        block.sections.push_back(tint(c.start + 1, currentEnd, m_styles[CurrentContent]));
        if (c.base >= 0) {
            block.sections.push_back(tint(c.base, c.base + 1, m_styles[BaseHeader]));
            block.sections.push_back(tint(c.base + 1, c.separator, m_styles[BaseContent]));
        }
        block.sections.push_back(tint(c.separator + 1, c.end, m_styles[IncomingContent]));
        block.end = tint(c.end, c.end + 1, m_styles[IncomingHeader]);
        std::erase(block.sections, nullptr); // empty sides
        m_blocks.push_back(std::move(block));
    }
    if (m_notes) {
        m_notes->resetHover();
    }
    updateViews();
    if (m_notes) {
        Q_EMIT m_notes->inlineNotesReset();
    }
}

void MergeConflicts::clear()
{
    if (m_conflicts.isEmpty() && m_blocks.empty()) {
        return;
    }
    rebuild({});
}

void MergeConflicts::updateViews()
{
    if (!m_document) {
        return;
    }
    const bool active = !m_conflicts.isEmpty();
    const auto views = m_document->views();
    for (KTextEditor::View *view : views) {
        if (active) {
            attach(view);
        } else {
            detach(view);
        }
    }
}

void MergeConflicts::attach(KTextEditor::View *view)
{
    if (!m_notes) {
        m_notes = std::make_unique<ConflictNotes>(this);
    }
    m_registered.removeIf([](const QPointer<KTextEditor::View> &v) {
        return !v;
    });
    if (!m_registered.contains(view)) {
        view->registerInlineNoteProvider(m_notes.get());
        m_registered.append(view);
    }
    addCommands(view);
    for (const char *name : kCommandNames) {
        if (QAction *action = view->actionCollection()->action(QLatin1String(name))) {
            action->setVisible(true);
            action->setEnabled(!QByteArray(name).startsWith("merge_conflict_accept") || m_document->isReadWrite());
        }
    }
}

void MergeConflicts::detach(KTextEditor::View *view)
{
    if (m_notes && m_registered.removeAll(view) > 0) {
        view->unregisterInlineNoteProvider(m_notes.get());
    }
    for (const char *name : kCommandNames) {
        if (QAction *action = view->actionCollection()->action(QLatin1String(name))) {
            action->setVisible(false); // out of the palette while the file has no conflicts
        }
    }
}

// In the view's own actions: the palette lists them with the editor's, only
// while they are visible (the file has conflicts).
void MergeConflicts::addCommands(KTextEditor::View *view)
{
    KActionCollection *collection = view->actionCollection();
    if (collection->action(QLatin1String(kCommandNames[0]))) {
        return;
    }
    const QPointer<KTextEditor::View> target(view);
    const auto add = [this, view, collection](const char *name, const QString &text, const std::function<void()> &slot) {
        auto *action = new QAction(text, view);
        setCommandCategory(action, kCategory);
        connect(action, &QAction::triggered, this, slot);
        collection->addAction(QLatin1String(name), action);
    };
    add(kCommandNames[0], tr("Next Conflict"), [this, target] {
        goTo(target, true);
    });
    add(kCommandNames[1], tr("Previous Conflict"), [this, target] {
        goTo(target, false);
    });
    add(kCommandNames[2], tr("Accept All Current"), [this] {
        acceptAll(ConflictSide::Current);
    });
    add(kCommandNames[3], tr("Accept All Incoming"), [this] {
        acceptAll(ConflictSide::Incoming);
    });
    add(kCommandNames[4], tr("Accept All Both"), [this] {
        acceptAll(ConflictSide::Both);
    });
}

MergeConflicts::Note MergeConflicts::noteAt(int line, int *index) const
{
    if (line < 0) {
        return NoNote;
    }
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        const Block &block = m_blocks[i];
        const Note note = block.start && block.start->start().line() == line ? StartNote
            : block.end && block.end->start().line() == line                 ? EndNote
                                                                              : NoNote;
        if (note != NoNote) {
            if (index) {
                *index = int(i);
            }
            return note;
        }
    }
    return NoNote;
}

int MergeConflicts::indexAt(int startLine) const
{
    for (qsizetype i = 0; i < m_conflicts.size(); ++i) {
        if (m_conflicts.at(i).start == startLine) {
            return int(i);
        }
    }
    return -1;
}

void MergeConflicts::replaceBlock(const Conflict &conflict, ConflictSide side)
{
    KTextEditor::Document *doc = m_document;
    const QStringList kept = acceptedLines(conflict, side, [doc](int line) {
        return doc->line(line);
    });
    const int lines = doc->lines();
    KTextEditor::Range range;
    QString text;
    if (conflict.end + 1 < lines) { // whole lines, newlines included
        range = KTextEditor::Range(conflict.start, 0, conflict.end + 1, 0);
        text = kept.isEmpty() ? QString() : kept.join(QLatin1Char('\n')) + QLatin1Char('\n');
    } else if (!kept.isEmpty() || conflict.start == 0) { // up to the end of the document
        range = KTextEditor::Range(conflict.start, 0, conflict.end, doc->lineLength(conflict.end));
        text = kept.join(QLatin1Char('\n'));
    } else { // nothing kept at the end: the newline before goes too
        range = KTextEditor::Range(conflict.start - 1, doc->lineLength(conflict.start - 1), conflict.end, doc->lineLength(conflict.end));
    }
    doc->replaceText(range, text);
}

void MergeConflicts::accept(int index, ConflictSide side)
{
    update();
    if (!m_document || !m_document->isReadWrite() || index < 0 || index >= m_conflicts.size()) {
        return;
    }
    {
        KTextEditor::Document::EditingTransaction transaction(m_document);
        replaceBlock(m_conflicts.at(index), side);
    }
    scan(); // the tint and the actions go at once
}

void MergeConflicts::acceptAll(ConflictSide side)
{
    update();
    if (!m_document || !m_document->isReadWrite() || m_conflicts.isEmpty()) {
        return;
    }
    {
        KTextEditor::Document::EditingTransaction transaction(m_document);
        for (qsizetype i = m_conflicts.size() - 1; i >= 0; --i) { // from the last: the earlier lines stay put
            replaceBlock(m_conflicts.at(i), side);
        }
    }
    scan();
}

void MergeConflicts::goTo(KTextEditor::View *view, bool forward)
{
    update();
    if (!view || m_conflicts.isEmpty()) {
        return;
    }
    const int line = view->cursorPosition().line();
    const Conflict *target = nullptr;
    if (forward) {
        const auto it = std::find_if(m_conflicts.cbegin(), m_conflicts.cend(), [line](const Conflict &c) {
            return c.start > line;
        });
        target = it != m_conflicts.cend() ? &*it : &m_conflicts.first();
    } else {
        const auto it = std::find_if(m_conflicts.crbegin(), m_conflicts.crend(), [line](const Conflict &c) {
            return c.start < line;
        });
        target = it != m_conflicts.crend() ? &*it : &m_conflicts.last();
    }
    view->setCursorPosition(KTextEditor::Cursor(target->start, 0));
}

QRect MergeConflicts::actionRect(KTextEditor::View *view, int index, int action) const
{
    if (!m_document || index < 0 || size_t(index) >= m_blocks.size() || !m_blocks[size_t(index)].start) {
        return {};
    }
    const int line = m_blocks[size_t(index)].start->start().line();
    const QPoint origin = line < 0 ? QPoint(-1, -1) : view->cursorToCoordinate(KTextEditor::Cursor(line, m_document->lineLength(line)));
    if (origin.x() < 0) {
        return {};
    }
    const QFont font = view->configValue(QStringLiteral("font")).value<QFont>();
    const NoteLayout layout = noteLayout(true, m_document->isReadWrite(), font);
    for (const NoteLayout::Segment &segment : layout.segments) {
        if (segment.action == action) {
            return QRect(origin.x() + segment.x, origin.y(), segment.width, QFontMetrics(font).height());
        }
    }
    return {};
}

void MergeConflicts::runTest(KTextEditor::View *view, const QString &steps)
{
    const QPointer<KTextEditor::View> target(view);
    const QStringList list = steps.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QString step = list.at(i);
        QTimer::singleShot(int(i) * 400, view, [target, step] {
            KTextEditor::View *view = target;
            MergeConflicts *conflicts = view ? of(view->document()) : nullptr;
            if (!conflicts) {
                qInfo().noquote() << "conflicts-step" << step << "(no view or not watched)";
                return;
            }
            KTextEditor::Document *doc = view->document();
            const QStringList args = step.split(QLatin1Char(':'));
            const QString name = args.value(0);
            const auto sideNamed = [](const QString &side) {
                return side == QLatin1String("current") ? ConflictSide::Current
                    : side == QLatin1String("incoming") ? ConflictSide::Incoming
                                                         : ConflictSide::Both;
            };
            const auto trigger = [view](const char *actionName) {
                if (QAction *action = view->actionCollection()->action(QLatin1String(actionName)); action && action->isVisible() && action->isEnabled()) {
                    action->trigger();
                    return true;
                }
                return false;
            };
            const auto mouse = [view](QEvent::Type type, const QPoint &inView, Qt::MouseButton button) {
                QWidget *area = textArea(view);
                if (!area) {
                    return;
                }
                const QPoint local = area->mapFrom(view, inView);
                QMouseEvent event(type, QPointF(local), QPointF(area->mapToGlobal(local)), button, type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                                  Qt::NoModifier);
                QApplication::sendEvent(area, &event);
            };
            qInfo().noquote() << "conflicts-step" << step;
            if (name == QLatin1String("list")) {
                conflicts->update();
                qInfo().noquote() << "conflicts" << conflicts->conflicts().size();
                for (const Conflict &c : conflicts->conflicts()) {
                    qInfo().noquote() << "conflict" << c.start + 1 << c.base + 1 << c.separator + 1 << c.end + 1 << c.currentLabel << "|" << c.baseLabel << "|"
                                      << c.incomingLabel;
                }
                for (const char *actionName : kCommandNames) {
                    const QAction *action = view->actionCollection()->action(QLatin1String(actionName));
                    qInfo().noquote() << "command" << actionName << (action && action->isVisible() ? "visible" : "hidden")
                                      << (action && action->isEnabled() ? "enabled" : "disabled") << (action ? action->text() : QString());
                }
            } else if (name == QLatin1String("dump")) {
                const QStringList lines = doc->text().split(QLatin1Char('\n'));
                for (const QString &line : lines) {
                    qInfo().noquote() << "|" << line;
                }
                qInfo().noquote() << "end-of-text modified" << doc->isModified();
            } else if (name == QLatin1String("next") || name == QLatin1String("prev")) {
                trigger(name == QLatin1String("next") ? kCommandNames[0] : kCommandNames[1]);
                qInfo().noquote() << "cursor" << view->cursorPosition().line() + 1;
            } else if (name == QLatin1String("undo")) {
                if (QAction *undo = view->actionCollection()->action(QStringLiteral("edit_undo"))) {
                    undo->trigger();
                }
            } else if (name == QLatin1String("accept")) {
                conflicts->accept(args.value(1).toInt(), sideNamed(args.value(2)));
            } else if (name == QLatin1String("all")) {
                const QString side = args.value(1);
                qInfo().noquote() << "triggered"
                                  << trigger(side == QLatin1String("current")      ? kCommandNames[2]
                                                 : side == QLatin1String("incoming") ? kCommandNames[3]
                                                                                     : kCommandNames[4]);
            } else if (name == QLatin1String("hover") || name == QLatin1String("click")) {
                const QRect rect = conflicts->actionRect(view, args.value(1).toInt(), args.value(2).toInt());
                qInfo().noquote() << "action-rect" << rect;
                if (rect.isEmpty()) {
                    return;
                }
                mouse(QEvent::MouseMove, rect.center(), Qt::NoButton);
                mouse(QEvent::MouseMove, rect.center() + QPoint(1, 0), Qt::NoButton);
                if (QWidget *area = textArea(view)) {
                    qInfo().noquote() << "pointer" << (area->cursor().shape() == Qt::PointingHandCursor ? "hand" : "other") << area->metaObject()->className();
                }
                if (name == QLatin1String("click")) {
                    mouse(QEvent::MouseButtonPress, rect.center(), Qt::LeftButton);
                    mouse(QEvent::MouseButtonRelease, rect.center(), Qt::LeftButton);
                    mouse(QEvent::MouseMove, rect.center() + QPoint(0, 400), Qt::NoButton); // away
                }
            } else if (name == QLatin1String("insert")) {
                const QString text = step.section(QLatin1Char(':'), 2).replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                doc->insertText(KTextEditor::Cursor(args.value(1).toInt() - 1, 0), text);
            }
        });
    }
}
