#include "diffview.h"

#include "gitgutter.h"
#include "gitpanel.h"
#include "linetint.h"
#include "platform.h"
#include "settings.h"
#include "tabview.h"
#include "theme.h"

#include <KActionCollection>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MovingRange>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScrollBar>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStringDecoder>
#include <QTabBar>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Lieve;

namespace
{
constexpr int kCompareDelay = 250; // ms after the last edit, like the change marks
constexpr qint64 kMaxBytes = 4 * 1024 * 1024; // larger files get the unified diff
constexpr int kStripWidth = 46;
constexpr int kButton = 20;
constexpr int kMaxWordLines = 200; // changes longer than this get no word highlights

bool unusable(const QByteArray &data)
{
    return data.size() > kMaxBytes || data.left(8000).contains('\0');
}

// MainWindow::colorDiff's colours: see-through, so they suit any editor
// theme; the changed words with the same colour laid twice, like VS Code's.
struct DiffColors {
    QColor removed;
    QColor added;
    QColor removedStrong;
    QColor addedStrong;
};

DiffColors diffColors()
{
    const bool dark = isDarkTheme();
    const int line = dark ? 60 : 45;
    const int strong = dark ? 106 : 82;
    return {QColor(248, 81, 73, line), QColor(46, 160, 67, line), QColor(248, 81, 73, strong), QColor(46, 160, 67, strong)};
}

KTextEditor::Attribute::Ptr background(const QColor &color)
{
    KTextEditor::Attribute::Ptr a(new KTextEditor::Attribute());
    a->setBackground(color);
    a->setBackgroundFillWhitespace(true);
    return a;
}

std::unique_ptr<KTextEditor::MovingRange>
movingRange(KTextEditor::Document *doc, const KTextEditor::Range &range, const KTextEditor::Attribute::Ptr &style, KTextEditor::View *view, qreal depth)
{
    std::unique_ptr<KTextEditor::MovingRange> moving(doc->newMovingRange(range));
    moving->setZDepth(depth); // before the attribute: one repaint
    if (view) {
        moving->setView(view); // the window's document: only in this page's view
    }
    moving->setAttribute(style);
    return moving;
}

// Lines [from, to): up to the start of line `to`, or the end of the text.
KTextEditor::Range linesRange(const KTextEditor::Document *doc, int from, int to)
{
    const int lines = doc->lines();
    return to < lines ? KTextEditor::Range(from, 0, to, 0) : KTextEditor::Range(from, 0, lines - 1, doc->lineLength(lines - 1));
}

void setReadOnlyText(KTextEditor::Document *doc, const QString &text)
{
    doc->setReadWrite(true);
    GitStaging::replaceText(doc, text);
    doc->setModified(false);
    doc->setReadWrite(false);
}

QString quoted(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\n'), QStringLiteral("\\n")).replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return text;
}
}

// The strip between the two sides: the splitter's handle, so it also resizes them.
class DiffStrip : public QSplitterHandle
{
public:
    DiffStrip(Qt::Orientation orientation, QSplitter *parent)
        : QSplitterHandle(orientation, parent)
    {
        setMouseTracking(true);
    }

    QPointer<DiffView> diff;
    int hovered = -1; // the change under the mouse

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Band {
        int index; // in the diff's changes
        qreal l0, l1; // rows of its lines on the left, in this strip
        qreal r0, r1; // on the right
    };
    struct Button {
        int index;
        GitStaging::Command command;
        QRect rect;
    };
    QList<Band> bands() const;
    QList<Button> buttons(const QList<Band> &bands) const;
    int bandAt(const QList<Band> &bands, QPoint pos) const;
    int buttonAt(const QList<Button> &buttons, QPoint pos) const;

    int m_hoverButton = -1;
    int m_pressedButton = -1;
    bool m_pressed = false;
    QPoint m_pressPos;
};

class DiffSplitter : public QSplitter
{
public:
    using QSplitter::QSplitter;

protected:
    QSplitterHandle *createHandle() override
    {
        return new DiffStrip(orientation(), this);
    }
};

// ---------------------------------------------------------------------------
// Loading

bool DiffView::supports(const GitEntry &entry)
{
    if (entry.group == GitEntry::Conflicts || entry.status == QLatin1Char('D') || entry.status == QLatin1Char('T')) {
        return false; // deleted, conflicts, a file that became a link
    }
    return true;
}

void DiffView::load(GitRepo *repo, const GitEntry &entry, const std::function<void(const Texts &)> &done)
{
    if (!supports(entry)) {
        done({});
        return;
    }
    if (entry.group == GitEntry::Changes) {
        // The working file: a text file of reasonable size (its document may differ: unsaved edits).
        const QFileInfo info(repo->absolutePath(entry.path));
        QFile file(info.filePath());
        if (info.isSymLink() || !info.isFile() || info.size() > kMaxBytes || !file.open(QIODevice::ReadOnly) || file.read(8000).contains('\0')) {
            done({});
            return;
        }
        repo->readFile(QLatin1Char(':') + entry.path, [done](bool, const QByteArray &index) { // none: a new file
            Texts texts;
            texts.ok = !unusable(index);
            texts.original = index;
            done(texts);
        });
        return;
    }
    const QString headPath = entry.origPath.isEmpty() ? entry.path : entry.origPath;
    repo->readFile(QLatin1Char(':') + entry.path, [repo, headPath, done](bool found, const QByteArray &index) {
        if (!found || unusable(index)) {
            done({});
            return;
        }
        repo->readFile(QStringLiteral("HEAD:") + headPath, [index, done](bool, const QByteArray &head) { // none: an added file
            Texts texts;
            texts.ok = !unusable(head);
            texts.original = head;
            texts.modified = index;
            done(texts);
        });
    });
}

QString DiffView::keyOf(const GitEntry &entry)
{
    return QString::number(int(entry.group)) + QLatin1Char(':') + entry.path;
}

std::optional<GitEntry> DiffView::entryOf(const GitRepo *repo, const QString &key)
{
    const QList<GitEntry> &entries = repo->entries();
    for (const GitEntry &e : entries) {
        if (keyOf(e) == key) {
            return e;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// The page

DiffView::DiffView(GitStaging *staging,
                   const GitEntry &entry,
                   const Texts &texts,
                   KTextEditor::Document *working,
                   const std::function<void(KTextEditor::View *)> &configure,
                   QWidget *parent)
    : QWidget(parent)
    , m_staging(staging)
    , m_entry(entry)
    , m_configure(configure)
{
    GitRepo *repo = staging->repo();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_split = new DiffSplitter(Qt::Horizontal, this);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(kStripWidth);
    layout->addWidget(m_split);

    KTextEditor::Editor *editor = KTextEditor::Editor::instance();
    m_leftDoc = editor->createDocument(this);
    if (staged()) {
        m_rightDoc = editor->createDocument(this);
        m_ownsRight = true;
        m_encoding = working ? working->encoding().toLatin1() : QByteArray("UTF-8");
    } else if (working) {
        m_rightDoc = working;
        m_encoding = working->encoding().toLatin1();
    } else {
        m_rightDoc = editor->createDocument(this);
        m_rightDoc->setConfigValue(QStringLiteral("auto-reload-on-external-changes"), true);
        m_rightDoc->openUrl(QUrl::fromLocalFile(path()));
        m_ownsRight = true;
        m_encoding = m_rightDoc->encoding().toLatin1();
    }
    setReadOnlyText(m_leftDoc, decode(texts.original));
    if (staged()) {
        setReadOnlyText(m_rightDoc, decode(texts.modified));
        m_rightDoc->setHighlightingMode(editor->repository().definitionForFileName(entry.path).name());
    }
    m_leftDoc->setHighlightingMode(m_rightDoc->highlightingMode());
    m_state = repo->indexState(entry.path) + QLatin1Char('\n') + (staged() ? repo->head() : QString());

    m_left = m_leftDoc->createView(m_split);
    m_right = m_rightDoc->createView(m_split);
    m_split->addWidget(m_left);
    m_split->addWidget(m_right);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);
    m_strip = static_cast<DiffStrip *>(m_split->handle(1));
    m_strip->diff = this;
    setupView(m_left);
    setupView(m_right);
    // One scroll bar, at the right (like VS Code's): the left side follows.
    if (QScrollBar *bar = m_left->verticalScrollBar()) {
        bar->setFixedWidth(0);
    }
    setFocusProxy(m_right);

    m_compareTimer.setSingleShot(true);
    m_compareTimer.setInterval(kCompareDelay);
    connect(&m_compareTimer, &QTimer::timeout, this, &DiffView::compare);
    KTextEditor::Document *right = m_rightDoc;
    connect(right, &KTextEditor::Document::textChanged, this, [this] {
        if (isVisible()) {
            m_compareTimer.start();
        } else {
            m_stale = true; // typed in the file's tab: compared when shown
        }
    });
    // Until then the tints move with the lines around them.
    connect(right, &KTextEditor::Document::lineWrapped, this, [this](KTextEditor::Document *, KTextEditor::Cursor position) {
        shiftRight(position.column() == 0 ? position.line() : position.line() + 1, 1);
    });
    connect(right, &KTextEditor::Document::lineUnwrapped, this, [this](KTextEditor::Document *, int line) {
        shiftRight(line, -1);
    });
    connect(right, &KTextEditor::Document::reloaded, this, &DiffView::compare);
    connect(right, &KTextEditor::Document::modifiedChanged, this, [this] {
        Q_EMIT titleChanged();
        if (ownsDocument() && m_rightDoc->isModified()) {
            QTimer::singleShot(0, this, [this] { // not inside the edit
                if (ownsDocument() && m_rightDoc && m_rightDoc->isModified()) {
                    Q_EMIT documentEdited();
                }
            });
        }
    });
    // The ranges are ours to delete while the document is whole (reload, close, its end).
    for (KTextEditor::Document *doc : {right, m_leftDoc.data()}) {
        connect(doc, &KTextEditor::Document::aboutToInvalidateMovingInterfaceContent, this, [this, doc] {
            (doc == m_leftDoc ? m_leftRanges : m_rightRanges).clear();
        });
    }
    connect(right, &QObject::destroyed, this, [this] {
        if (!m_closing) {
            Q_EMIT documentClosed();
        }
    });
    connect(repo, &GitRepo::changed, this, &DiffView::refresh);
    compare();
}

DiffView::~DiffView()
{
    m_closing = true;
    m_leftRanges.clear();
    m_rightRanges.clear();
    if (m_rightDoc) {
        m_rightDoc->disconnect(this);
    }
    if (m_ownsRight) {
        delete m_rightDoc.data(); // and its view
    } else {
        delete m_right.data(); // our view of the window's document
    }
    delete m_leftDoc.data();
}

bool DiffView::staged() const
{
    return m_entry.group == GitEntry::Staged;
}

QString DiffView::key() const
{
    return keyOf(m_entry);
}

QString DiffView::title() const
{
    const QString name = QFileInfo(m_entry.path).fileName();
    if (staged()) {
        return tr("%1 (Index)").arg(name);
    }
    QString title = tr("%1 (Working Tree)").arg(name);
    if (m_rightDoc && m_rightDoc->isModified()) {
        title += QStringLiteral(" ●");
    }
    return title;
}

QString DiffView::path() const
{
    return m_staging->repo()->absolutePath(m_entry.path);
}

QString DiffView::headPath() const
{
    return m_entry.origPath.isEmpty() ? m_entry.path : m_entry.origPath;
}

KTextEditor::Document *DiffView::document() const
{
    return m_rightDoc;
}

bool DiffView::ownsDocument() const
{
    return !staged() && m_ownsRight && m_rightDoc;
}

KTextEditor::Document *DiffView::releaseDocument()
{
    if (!ownsDocument()) {
        return nullptr;
    }
    m_ownsRight = false;
    return m_rightDoc;
}

KTextEditor::View *DiffView::leftView() const
{
    return m_left;
}

KTextEditor::View *DiffView::rightView() const
{
    return m_right;
}

KTextEditor::View *DiffView::focusedView() const
{
    return m_focused ? m_focused.data() : m_right.data();
}

void DiffView::setupView(KTextEditor::View *view)
{
    view->setStatusBarEnabled(false);
    // Like the window's editors, without what would break the alignment
    // (folding, wrapped lines) or repeat the diff (change markers, minimap).
    QSettings &c = config();
    const QStringList keys = view->configKeys();
    const auto set = [&](const QString &key, const QVariant &value) {
        if (keys.contains(key)) {
            view->setConfigValue(key, value);
        }
    };
    set(QStringLiteral("theme"), Theme::editorThemeName());
    set(QStringLiteral("line-numbers"), true);
    set(QStringLiteral("icon-bar"), false);
    set(QStringLiteral("folding-bar"), false);
    set(QStringLiteral("modification-markers"), false);
    set(QStringLiteral("scrollbar-minimap"), false);
    set(QStringLiteral("scrollbar-preview"), false);
    set(QStringLiteral("dynamic-word-wrap"), false);
    const QString family = c.value(QStringLiteral("editor/fontFamily")).toString().trimmed();
    const double size = c.value(QStringLiteral("editor/fontSize"), 0).toDouble();
    if (!family.isEmpty() || size > 0) {
        QFont font = Lieve::Platform::fixedFont();
        if (!family.isEmpty()) {
            font.setFamily(family);
        }
        if (size > 0) {
            font.setPointSizeF(size);
        }
        set(QStringLiteral("font"), font);
    }
    view->setContextMenu(view->defaultContextMenu());
    if (m_configure) {
        m_configure(view);
    }
    connect(view, &KTextEditor::View::focusIn, this, [this, view] {
        m_focused = view;
        if (m_strip) {
            m_strip->update();
        }
        Q_EMIT focused();
    });
    connect(view, &KTextEditor::View::contextMenuAboutToShow, this, &DiffView::fillMenu);
    connect(view, &KTextEditor::View::cursorPositionChanged, this, [this] {
        if (m_strip) {
            m_strip->update();
        }
    });
    // Scrolling by the user (wheel, keys, scroll bar); scrolling from code
    // (setScrollPosition) says nothing, so the other side does not echo back.
    connect(view, &KTextEditor::View::verticalScrollPositionChanged, this, [this, view] {
        syncFrom(view);
    });
    connect(view, &KTextEditor::View::horizontalScrollPositionChanged, this, [this, view] {
        syncHorizontal(view, view->horizontalScrollBar() ? view->horizontalScrollBar()->value() : 0);
    });
}

// The commands for the selection, at the end of each side's context menu.
void DiffView::fillMenu(KTextEditor::View *view, QMenu *menu)
{
    if (!menu) {
        return;
    }
    const QList<GitStaging::Command> commands = staged() ? QList<GitStaging::Command>{GitStaging::Unstage}
                                                         : QList<GitStaging::Command>{GitStaging::Stage, GitStaging::Revert};
    if (!menu->property("lieveDiffActions").toBool()) {
        menu->setProperty("lieveDiffActions", true);
        menu->addSeparator();
        for (const GitStaging::Command command : commands) {
            auto *action = new QAction(GitStaging::commandIcon(command), QString(), menu);
            action->setObjectName(QStringLiteral("lieveDiffCommand%1").arg(int(command)));
            menu->addAction(action);
            connect(action, &QAction::triggered, this, [this, target = QPointer<KTextEditor::View>(view), command] {
                if (target) {
                    m_focused = target;
                    run(command);
                }
            });
        }
    }
    for (const GitStaging::Command command : commands) {
        if (auto *action = menu->findChild<QAction *>(QStringLiteral("lieveDiffCommand%1").arg(int(command)), Qt::FindDirectChildrenOnly)) {
            action->setText(GitStaging::commandText(command, view->selection()));
            action->setVisible(command != GitStaging::Revert || (m_rightDoc && m_rightDoc->isReadWrite()));
        }
    }
}

QString DiffView::decode(const QByteArray &data) const
{
    QStringDecoder decoder(m_encoding.isEmpty() ? "UTF-8" : m_encoding.constData());
    if (!decoder.isValid()) {
        decoder = QStringDecoder(QStringDecoder::Utf8);
    }
    // Lines as a document loaded from the file has them (and as DiffText splits them).
    QString text = decoder.decode(data);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return text;
}

void DiffView::refresh()
{
    if (!isVisible()) {
        m_stale = true; // behind another tab: read again when shown
        return;
    }
    GitRepo *repo = m_staging->repo();
    const QString state = repo->indexState(m_entry.path) + QLatin1Char('\n') + (staged() ? repo->head() : QString());
    if (state == m_state || !repo->isRepository()) {
        return;
    }
    m_state = state;
    const quint64 serial = ++m_readSerial;
    const QPointer<DiffView> self(this);
    load(repo, m_entry, [self, serial](const Texts &texts) {
        if (self && serial == self->m_readSerial && texts.ok) {
            self->setTexts(texts);
        }
    });
}

void DiffView::setTexts(const Texts &texts)
{
    if (!m_leftDoc || !m_rightDoc) {
        return;
    }
    setReadOnlyText(m_leftDoc, decode(texts.original));
    if (staged()) {
        setReadOnlyText(m_rightDoc, decode(texts.modified));
    }
    compare();
}

void DiffView::compare()
{
    m_compareTimer.stop();
    if (!m_leftDoc || !m_rightDoc || !m_left || !m_right) {
        return;
    }
    const QStringList left = GitStaging::documentLines(m_leftDoc);
    const QStringList right = GitStaging::documentLines(m_rightDoc);
    QList<size_t> a;
    QList<size_t> b;
    a.reserve(left.size());
    b.reserve(right.size());
    for (const QString &line : left) {
        a.append(qHash(QStringView(line)));
    }
    for (const QString &line : right) {
        b.append(qHash(QStringView(line)));
    }
    m_hunks = diffLines(a, b);
    rebuildRanges(left, right);
    updateTints();
    if (m_strip) {
        m_strip->update();
    }
    if (m_shown && m_reveal) {
        doReveal();
    }
}

void DiffView::rebuildRanges(const QStringList &left, const QStringList &right)
{
    m_leftRanges.clear();
    m_rightRanges.clear();
    const DiffColors c = diffColors();
    const KTextEditor::Attribute::Ptr removed = background(c.removed);
    const KTextEditor::Attribute::Ptr added = background(c.added);
    const KTextEditor::Attribute::Ptr removedWords = background(c.removedStrong);
    const KTextEditor::Attribute::Ptr addedWords = background(c.addedStrong);
    for (const LineHunk &h : std::as_const(m_hunks)) {
        if (h.aCount > 0) {
            m_leftRanges.push_back(movingRange(m_leftDoc, linesRange(m_leftDoc, h.aStart, h.aStart + h.aCount), removed, nullptr, 0));
        }
        if (h.bCount > 0) {
            m_rightRanges.push_back(movingRange(m_rightDoc, linesRange(m_rightDoc, h.bStart, h.bStart + h.bCount), added, m_right, 0));
        }
        if (h.aCount == 0 || h.bCount == 0 || h.aCount > kMaxWordLines || h.bCount > kMaxWordLines) {
            continue;
        }
        QList<TextSpan> inLeft;
        QList<TextSpan> inRight;
        diffWords(left.mid(h.aStart, h.aCount), right.mid(h.bStart, h.bCount), &inLeft, &inRight);
        for (const TextSpan &s : std::as_const(inLeft)) {
            const KTextEditor::Range range(h.aStart + s.line, s.column, h.aStart + s.line, s.column + s.length);
            m_leftRanges.push_back(movingRange(m_leftDoc, range, removedWords, nullptr, -1)); // over the line's colour
        }
        for (const TextSpan &s : std::as_const(inRight)) {
            const KTextEditor::Range range(h.bStart + s.line, s.column, h.bStart + s.line, s.column + s.length);
            m_rightRanges.push_back(movingRange(m_rightDoc, range, addedWords, m_right, -1));
        }
    }
}

// The rest of the tinted rows, past the text; a hairline where the other side has lines this one lacks.
void DiffView::updateTints()
{
    const DiffColors c = diffColors();
    QList<LineTint::Span> left;
    QList<LineTint::Span> right;
    QList<LineTint::Edge> leftEdges;
    QList<LineTint::Edge> rightEdges;
    for (const LineHunk &h : std::as_const(m_hunks)) {
        if (h.aCount > 0) {
            left.append({h.aStart, h.aStart + h.aCount - 1, c.removed});
        } else {
            leftEdges.append({h.aStart, c.addedStrong});
        }
        if (h.bCount > 0) {
            right.append({h.bStart, h.bStart + h.bCount - 1, c.added});
        } else {
            rightEdges.append({h.bStart, c.removedStrong});
        }
    }
    if (LineTint *tint = LineTint::of(m_left)) {
        tint->set(left, leftEdges);
    }
    if (LineTint *tint = LineTint::of(m_right)) {
        tint->set(right, rightEdges);
    }
}

void DiffView::shiftRight(int from, int delta)
{
    // `delta` lines were inserted at `from`, or line `from` was joined to the one above.
    if (m_hunks.isEmpty()) {
        return;
    }
    for (LineHunk &h : m_hunks) {
        if (h.bStart >= from) {
            h.bStart = std::max(0, h.bStart + delta);
        } else if (h.bCount > 0 && from < h.bStart + h.bCount) {
            h.bCount = std::max(1, h.bCount + delta);
        }
    }
    updateTints();
    if (m_strip) {
        m_strip->update();
    }
}

// ---------------------------------------------------------------------------
// Scrolling together

int DiffView::rowHeight(KTextEditor::View *view) const
{
    // Measured between two rows, on either side (same font): one may have a single line.
    for (KTextEditor::View *v : {view, view == m_left ? m_right.data() : m_left.data()}) {
        const int first = v ? v->firstDisplayedLine() : -1;
        if (first >= 0 && first + 1 < v->document()->lines()) {
            const QPoint a = v->cursorToCoordinate(KTextEditor::Cursor(first, 0));
            const QPoint b = v->cursorToCoordinate(KTextEditor::Cursor(first + 1, 0));
            if (a.y() >= 0 && b.y() > a.y()) {
                return b.y() - a.y();
            }
        }
    }
    const QFontMetricsF metrics(view->configValue(QStringLiteral("font")).value<QFont>());
    return std::max(1, int(std::ceil(metrics.ascent() + metrics.descent())));
}

int DiffView::visibleRows(KTextEditor::View *view) const
{
    return std::max(1, view->textAreaRect().height() / rowHeight(view));
}

// A line of one side on the other: the same distance from the changes
// around it, or as far into a change as it is into its counterpart.
double DiffView::mapLine(double line, bool fromLeft) const
{
    double offset = 0;
    for (const LineHunk &h : m_hunks) {
        const int fromStart = fromLeft ? h.aStart : h.bStart;
        const int fromCount = fromLeft ? h.aCount : h.bCount;
        const int toStart = fromLeft ? h.bStart : h.aStart;
        const int toCount = fromLeft ? h.bCount : h.aCount;
        if (line < fromStart) {
            return line + (toStart - fromStart);
        }
        if (line < fromStart + fromCount) {
            return toStart + (line - fromStart) * double(toCount) / fromCount;
        }
        offset = (toStart + toCount) - (fromStart + fromCount);
    }
    return line + offset;
}

void DiffView::syncFrom(KTextEditor::View *view)
{
    if (m_syncing || !m_left || !m_right) {
        return;
    }
    KTextEditor::View *other = view == m_left ? m_right.data() : m_left.data();
    // The middle line of the view is kept aligned.
    const double middle = view->firstDisplayedLine() + visibleRows(view) / 2.0;
    const int target = int(std::lround(mapLine(middle, view == m_left) - visibleRows(other) / 2.0));
    const int top = std::clamp(target, 0, std::max(0, other->document()->lines() - 1));
    if (top != other->firstDisplayedLine()) {
        m_syncing = true;
        other->setScrollPosition(KTextEditor::Cursor(top, 0)); // that line at the top
        m_syncing = false;
    }
    if (m_strip) {
        m_strip->update();
    }
}

void DiffView::syncHorizontal(KTextEditor::View *view, int value)
{
    if (m_syncing || !m_left || !m_right) {
        return;
    }
    m_syncing = true;
    (view == m_left ? m_right : m_left)->setHorizontalScrollPosition(value);
    m_syncing = false;
}

void DiffView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (std::exchange(m_stale, false)) {
        compare();
        refresh(); // reads git again only if the index (or HEAD) changed meanwhile
    }
    if (!m_shown) {
        m_shown = true;
        if (!m_reveal) {
            m_reveal = -1; // the first change
        }
        QTimer::singleShot(0, this, &DiffView::doReveal); // once laid out
    }
}

void DiffView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_shown) {
        QTimer::singleShot(0, this, [this] { // once the sides have their new height
            if (m_right) {
                syncFrom(focusedView());
            }
        });
    }
}

void DiffView::revealLine(int line)
{
    m_reveal = line;
    if (m_shown) {
        QTimer::singleShot(0, this, &DiffView::doReveal);
    }
}

void DiffView::doReveal()
{
    if (!m_reveal || !m_right || !m_rightDoc) {
        return;
    }
    const int line = *std::exchange(m_reveal, std::nullopt);
    if (m_hunks.isEmpty()) {
        syncFrom(m_right);
        return;
    }
    int index = line < 0 ? 0 : changeAt(line, false);
    if (index < 0) { // the first change after it, else the last
        index = int(m_hunks.size()) - 1;
        for (qsizetype i = 0; i < m_hunks.size(); ++i) {
            if (m_hunks.at(i).bStart >= line) {
                index = int(i);
                break;
            }
        }
    }
    showChange(index, m_right);
}

// The cursor of `view` on change `index`, which is scrolled to the middle if out of sight.
void DiffView::showChange(int index, KTextEditor::View *view)
{
    if (index < 0 || index >= m_hunks.size() || !view) {
        return;
    }
    const LineHunk &h = m_hunks.at(index);
    const bool left = view == m_left;
    const int start = left ? h.aStart : h.bStart;
    const int count = std::max(1, left ? h.aCount : h.bCount);
    const int line = std::clamp(start, 0, view->document()->lines() - 1);
    if (start < view->firstDisplayedLine() || start + count - 1 > view->lastDisplayedLine()) {
        view->setScrollPosition(KTextEditor::Cursor(std::max(0, start - visibleRows(view) / 2), 0));
    }
    view->setCursorPosition(KTextEditor::Cursor(line, 0));
    syncFrom(view);
}

void DiffView::goToChange(bool forward)
{
    if (m_compareTimer.isActive()) {
        compare();
    }
    KTextEditor::View *view = focusedView();
    if (m_hunks.isEmpty() || !view) {
        return;
    }
    const bool left = view == m_left;
    const int line = view->cursorPosition().line();
    const auto startOf = [left](const LineHunk &h) {
        return left ? h.aStart : h.bStart;
    };
    int index = -1;
    if (forward) {
        for (qsizetype i = 0; i < m_hunks.size() && index < 0; ++i) {
            index = startOf(m_hunks.at(i)) > line ? int(i) : -1;
        }
        index = index < 0 ? 0 : index;
    } else {
        for (qsizetype i = m_hunks.size() - 1; i >= 0 && index < 0; --i) {
            index = startOf(m_hunks.at(i)) < line ? int(i) : -1;
        }
        index = index < 0 ? int(m_hunks.size()) - 1 : index;
    }
    showChange(index, view);
}

// The change that line `line` of a side is in, or next to (a change with no lines on that side).
int DiffView::changeAt(int line, bool left) const
{
    for (qsizetype i = 0; i < m_hunks.size(); ++i) {
        const LineHunk &h = m_hunks.at(i);
        const int start = left ? h.aStart : h.bStart;
        const int count = left ? h.aCount : h.bCount;
        if (count > 0 ? (line >= start && line < start + count) : (line == start || line == start - 1)) {
            return int(i);
        }
    }
    return -1;
}

int DiffView::currentChange() const
{
    KTextEditor::View *view = focusedView();
    return view ? changeAt(view->cursorPosition().line(), view == m_left) : -1;
}

// ---------------------------------------------------------------------------
// Staging

void DiffView::run(GitStaging::Command command)
{
    KTextEditor::View *view = focusedView();
    if (!view || !m_rightDoc) {
        return;
    }
    const LineSelection selection = GitStaging::selectionOf(view, view == m_left ? DiffSide::Original : DiffSide::Modified);
    if (staged() != (command == GitStaging::Unstage)) {
        GitStaging::showMessage(m_rightDoc, staged() ? tr("These changes are staged: they can be unstaged.") : tr("These changes are not staged yet."));
        return;
    }
    switch (command) {
    case GitStaging::Stage:
        m_staging->stage(m_rightDoc, m_entry.path, selection);
        break;
    case GitStaging::Revert:
        m_staging->revert(m_rightDoc, m_entry.path, selection);
        break;
    case GitStaging::Unstage:
        m_staging->unstage(m_entry.path, headPath(), selection, m_rightDoc);
        break;
    }
}

void DiffView::runOnChange(int index, GitStaging::Command command)
{
    if (m_compareTimer.isActive()) {
        compare(); // still typing: what was clicked may have moved, show it as it is now
        return;
    }
    if (index < 0 || index >= m_hunks.size() || !m_rightDoc) {
        return;
    }
    LineSelection selection;
    selection.hunk = m_hunks.at(index);
    switch (command) {
    case GitStaging::Stage:
        m_staging->stage(m_rightDoc, m_entry.path, selection);
        break;
    case GitStaging::Revert:
        m_staging->revert(m_rightDoc, m_entry.path, selection);
        break;
    case GitStaging::Unstage:
        m_staging->unstage(m_entry.path, headPath(), selection, m_rightDoc);
        break;
    }
}

// ---------------------------------------------------------------------------
// The strip

QList<DiffStrip::Band> DiffStrip::bands() const
{
    QList<Band> list;
    if (!diff || !diff->m_left || !diff->m_right) {
        return list;
    }
    // Rows of each side, in this strip: from its first line shown, row by row.
    const auto origin = [this](KTextEditor::View *view, int *first, int *height) {
        *first = view->firstDisplayedLine();
        *height = diff->rowHeight(view);
        const QPoint p = view->cursorToCoordinate(KTextEditor::Cursor(std::max(0, *first), 0));
        return mapFrom(diff, view->mapTo(diff, QPoint(0, std::max(0, p.y())))).y();
    };
    int leftFirst = 0;
    int leftHeight = 1;
    int rightFirst = 0;
    int rightHeight = 1;
    const int leftTop = origin(diff->m_left, &leftFirst, &leftHeight);
    const int rightTop = origin(diff->m_right, &rightFirst, &rightHeight);
    const auto leftY = [=](int line) {
        return qreal(leftTop + (line - leftFirst) * leftHeight);
    };
    const auto rightY = [=](int line) {
        return qreal(rightTop + (line - rightFirst) * rightHeight);
    };
    for (qsizetype i = 0; i < diff->m_hunks.size(); ++i) {
        const LineHunk &h = diff->m_hunks.at(i);
        const Band band{int(i), leftY(h.aStart), leftY(h.aStart + h.aCount), rightY(h.bStart), rightY(h.bStart + h.bCount)};
        if (std::max(band.l1, band.r1) >= 0 && std::min(band.l0, band.r0) <= height()) {
            list.append(band);
        }
    }
    return list;
}

QList<DiffStrip::Button> DiffStrip::buttons(const QList<Band> &bands) const
{
    QList<Button> list;
    if (!diff) {
        return list;
    }
    const int shown = hovered >= 0 ? hovered : diff->currentChange();
    const auto band = std::find_if(bands.cbegin(), bands.cend(), [shown](const Band &b) {
        return b.index == shown;
    });
    if (band == bands.cend()) {
        return list;
    }
    QList<GitStaging::Command> commands;
    if (diff->staged()) {
        commands << GitStaging::Unstage;
    } else {
        commands << GitStaging::Stage;
        if (diff->m_rightDoc && diff->m_rightDoc->isReadWrite()) {
            commands << GitStaging::Revert;
        }
    }
    // At the top of the band, in its middle; centred on a band too thin for them.
    const qreal top = (band->l0 + band->r0) / 2;
    const qreal bottom = (band->l1 + band->r1) / 2;
    int y = bottom - top >= kButton + 2 ? int(top) + 1 : int((top + bottom) / 2) - kButton / 2;
    y = std::clamp(y, 0, std::max(0, height() - kButton));
    const int total = int(commands.size()) * kButton + (int(commands.size()) - 1) * 2;
    int x = (width() - total) / 2;
    for (const GitStaging::Command command : std::as_const(commands)) {
        list.append({band->index, command, QRect(x, y, kButton, kButton)});
        x += kButton + 2;
    }
    return list;
}

int DiffStrip::bandAt(const QList<Band> &bands, QPoint pos) const
{
    // The band's edges are S curves from the left side to the right one.
    const qreal t = std::clamp(pos.x() / qreal(std::max(1, width())), 0.0, 1.0);
    const qreal s = t * t * (3 - 2 * t);
    for (const Band &b : bands) {
        const qreal top = b.l0 + (b.r0 - b.l0) * s;
        const qreal bottom = b.l1 + (b.r1 - b.l1) * s;
        if (pos.y() >= top - 3 && pos.y() <= bottom + 3) {
            return b.index;
        }
    }
    return -1;
}

int DiffStrip::buttonAt(const QList<Button> &buttons, QPoint pos) const
{
    for (qsizetype i = 0; i < buttons.size(); ++i) {
        if (buttons.at(i).rect.contains(pos)) {
            return int(i);
        }
    }
    return -1;
}

void DiffStrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    KTextEditor::View *left = diff ? diff->m_left.data() : nullptr;
    const QColor base = left ? left->configValue(QStringLiteral("background-color")).value<QColor>() : Theme::colors().editor;
    p.fillRect(rect(), base.isValid() ? base : palette().base().color());
    if (!left || !diff->m_right) {
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    const DiffColors c = diffColors();
    const qreal w = width();
    const QList<Band> list = bands();
    for (const Band &b : list) {
        const LineHunk &h = diff->m_hunks.at(b.index);
        QPainterPath band;
        band.moveTo(0, b.l0);
        band.cubicTo(w / 2, b.l0, w / 2, b.r0, w, b.r0);
        band.lineTo(w, b.r1);
        band.cubicTo(w / 2, b.r1, w / 2, b.l1, 0, b.l1);
        band.closeSubpath();
        // Red from the removed lines to green on the added ones.
        QBrush fill;
        QBrush edge;
        if (h.aCount == 0) {
            fill = c.added;
            edge = c.addedStrong;
        } else if (h.bCount == 0) {
            fill = c.removed;
            edge = c.removedStrong;
        } else {
            QLinearGradient soft(0, 0, w, 0);
            soft.setColorAt(0, c.removed);
            soft.setColorAt(1, c.added);
            QLinearGradient strong(0, 0, w, 0);
            strong.setColorAt(0, c.removedStrong);
            strong.setColorAt(1, c.addedStrong);
            fill = soft;
            edge = strong;
        }
        p.fillPath(band, fill);
        QPainterPath top;
        top.moveTo(0, b.l0 + 0.5);
        top.cubicTo(w / 2, b.l0 + 0.5, w / 2, b.r0 + 0.5, w, b.r0 + 0.5);
        QPainterPath bottom;
        bottom.moveTo(0, b.l1 - 0.5);
        bottom.cubicTo(w / 2, b.l1 - 0.5, w / 2, b.r1 - 0.5, w, b.r1 - 0.5);
        p.strokePath(top, QPen(edge, 1));
        p.strokePath(bottom, QPen(edge, 1));
    }
    // The change's buttons, with the hover tile of the tab bar's buttons.
    const Lieve::Colors &theme = Theme::colors();
    const qreal mixing = theme.dark ? 0.14 : 0.12; // the stylesheet's @hoverStrong
    const QColor hover = QColor::fromRgbF(theme.chrome.redF() * (1 - mixing) + theme.fg.redF() * mixing,
                                          theme.chrome.greenF() * (1 - mixing) + theme.fg.greenF() * mixing,
                                          theme.chrome.blueF() * (1 - mixing) + theme.fg.blueF() * mixing);
    const QList<Button> shown = buttons(list);
    for (qsizetype i = 0; i < shown.size(); ++i) {
        const Button &button = shown.at(i);
        if (i == m_hoverButton) {
            p.setPen(Qt::NoPen);
            p.setBrush(hover);
            p.drawRoundedRect(QRectF(button.rect), 5, 5);
        }
        GitStaging::commandIcon(button.command).paint(&p, button.rect.adjusted(2, 2, -2, -2));
    }
}

void DiffStrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_pressed && m_pressedButton < 0 && (event->buttons() & Qt::LeftButton)) {
        QSplitterHandle::mouseMoveEvent(event); // resizing the sides
        return;
    }
    const QPoint pos = event->position().toPoint();
    const QList<Band> list = bands();
    int button = buttonAt(buttons(list), pos);
    if (button < 0) { // on a button the change stays the same, even past its band
        const int band = bandAt(list, pos);
        if (band != hovered) {
            hovered = band;
            button = buttonAt(buttons(list), pos);
            update();
        }
    }
    if (button != m_hoverButton) {
        m_hoverButton = button;
        update();
    }
    setCursor(button >= 0 ? Qt::PointingHandCursor : Qt::SplitHCursor);
}

void DiffStrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_pressPos = event->position().toPoint();
        m_pressedButton = buttonAt(buttons(bands()), m_pressPos);
        if (m_pressedButton >= 0) {
            event->accept();
            return;
        }
    }
    QSplitterHandle::mousePressEvent(event);
}

void DiffStrip::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_pressedButton >= 0) {
        const QList<Button> list = buttons(bands());
        const int button = buttonAt(list, pos);
        if (button == m_pressedButton && diff) {
            diff->runOnChange(list.at(button).index, list.at(button).command);
        }
        m_pressedButton = -1;
        m_pressed = false;
        return;
    }
    QSplitterHandle::mouseReleaseEvent(event);
    // A click on a band (not a drag): the cursor on that change.
    if (m_pressed && event->button() == Qt::LeftButton && (pos - m_pressPos).manhattanLength() < 3 && diff && diff->m_right) {
        const int band = bandAt(bands(), pos);
        if (band >= 0) {
            diff->showChange(band, diff->m_right);
            diff->m_right->setFocus();
        }
    }
    m_pressed = false;
}

void DiffStrip::leaveEvent(QEvent *event)
{
    QSplitterHandle::leaveEvent(event);
    if (hovered >= 0 || m_hoverButton >= 0) {
        hovered = -1;
        m_hoverButton = -1;
        update();
    }
}

void DiffStrip::wheelEvent(QWheelEvent *event)
{
    if (diff && diff->m_right && diff->m_right->verticalScrollBar()) {
        QCoreApplication::sendEvent(diff->m_right->verticalScrollBar(), event);
    }
}

bool DiffStrip::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const QList<Button> list = buttons(bands());
        const int button = buttonAt(list, help->pos());
        if (button >= 0) {
            QToolTip::showText(help->globalPos(), GitStaging::commandText(list.at(button).command, false), this);
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QSplitterHandle::event(event);
}

// ---------------------------------------------------------------------------
// The tab bar's buttons

namespace
{
class DiffCorner : public QWidget
{
public:
    DiffCorner(TabView *group, GitRepo *repo, const std::function<void(QWidget *)> &toggle, const std::function<void(QWidget *)> &openFile)
        : QWidget(group)
        , m_group(group)
        , m_repo(repo)
    {
        setObjectName(QStringLiteral("diffCorner"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 4, 0);
        layout->setSpacing(0);
        // Like the Markdown preview's button and the terminal's.
        const auto button = [this, layout](const char *name, const QString &icon) {
            auto *b = new QToolButton(this);
            b->setObjectName(QLatin1String(name));
            b->setIcon(QIcon::fromTheme(icon));
            b->setAutoRaise(true);
            b->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(b);
            return b;
        };
        m_open = button("diffOpenFile", QStringLiteral("document-open"));
        m_open->setToolTip(DiffView::tr("Open File"));
        m_previous = button("diffPrevious", QStringLiteral("go-up"));
        m_previous->setToolTip(DiffView::tr("Previous Change (Shift+Alt+F5)"));
        m_next = button("diffNext", QStringLiteral("go-down"));
        m_next->setToolTip(DiffView::tr("Next Change (Alt+F5)"));
        m_toggle = button("diffToggle", QStringLiteral("view-split-left-right"));
        group->setCornerWidget(this);
        hide();

        const auto step = [this](bool forward) {
            QWidget *page = m_group ? m_group->currentWidget() : nullptr;
            if (auto *diff = qobject_cast<DiffView *>(page)) {
                diff->goToChange(forward);
            } else if (auto *view = qobject_cast<KTextEditor::View *>(page)) {
                DiffView::goToHunk(view, forward);
            }
        };
        connect(m_previous, &QToolButton::clicked, this, [step] {
            step(false);
        });
        connect(m_next, &QToolButton::clicked, this, [step] {
            step(true);
        });
        connect(m_toggle, &QToolButton::clicked, this, [this, toggle] {
            if (m_group && m_group->currentWidget()) {
                toggle(m_group->currentWidget());
            }
        });
        connect(m_open, &QToolButton::clicked, this, [this, openFile] {
            if (m_group && m_group->currentWidget()) {
                openFile(m_group->currentWidget());
            }
        });
        connect(group, &TabView::currentChanged, this, &DiffCorner::refresh);
        connect(repo, &GitRepo::changed, this, &DiffCorner::refresh);
    }

    void refresh()
    {
        QWidget *page = m_group ? m_group->currentWidget() : nullptr;
        auto *diff = qobject_cast<DiffView *>(page);
        auto *view = qobject_cast<KTextEditor::View *>(page);
        // A unified diff of a change (not of a commit): its key is the entry's.
        const QString key = view ? view->document()->property("lieveDiffKey").toString() : QString();
        const bool unified = !key.isEmpty() && !key.startsWith(QLatin1String("commit:"));
        setVisible(diff || unified);
        if (!diff && !unified) {
            return;
        }
        if (diff) {
            m_toggle->setToolTip(DiffView::tr("Show Inline"));
            m_toggle->setEnabled(true);
            m_open->setEnabled(QFileInfo::exists(diff->path()));
            return;
        }
        const std::optional<GitEntry> entry = DiffView::entryOf(m_repo, key);
        m_toggle->setToolTip(DiffView::tr("Show Side by Side"));
        m_toggle->setEnabled(entry && DiffView::supports(*entry));
        m_open->setEnabled(QFileInfo::exists(m_repo->absolutePath(key.section(QLatin1Char(':'), 1))));
    }

private:
    QPointer<TabView> m_group;
    GitRepo *m_repo;
    QToolButton *m_open;
    QToolButton *m_previous;
    QToolButton *m_next;
    QToolButton *m_toggle;
};
}

QWidget *DiffView::createCorner(TabView *group, GitRepo *repo, const std::function<void(QWidget *)> &toggle, const std::function<void(QWidget *)> &openFile)
{
    return new DiffCorner(group, repo, toggle, openFile);
}

void DiffView::goToHunk(KTextEditor::View *view, bool forward)
{
    KTextEditor::Document *doc = view->document();
    const int lines = doc->lines();
    const int from = view->cursorPosition().line();
    for (int step = 1; step <= lines; ++step) {
        const int line = ((forward ? from + step : from - step) % lines + lines) % lines;
        if (doc->line(line).startsWith(QLatin1String("@@"))) {
            view->setCursorPosition(KTextEditor::Cursor(line, 0));
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Test hook

// LIEVE_TEST_DIFF=step;step...: one step every 800 ms (LIEVE_TEST_DIFF_AT ms
// after the start). Paths are relative to the repository root, lines 1-based.
//   open:<path>, open-staged:<path>   the side-by-side diff of an unstaged
//                                     (staged) change, as from the Git panel
//   file:<path>                       opens the file in an editor
//   gutter:<line>                     clicks the change mark at that line of the editor
//   gutter-menu:<line>[:<n>]          its right-click menu: prints it, runs the n-th entry
//   next, prev                        Alt+F5, Shift+Alt+F5
//   focus:left|right                  the side the commands work on
//   select:<a>-<b>, cursor:<n>        lines selected, or just the cursor
//   stage, unstage, revert            the window's Stage / Unstage / Revert Selected Ranges
//   stage-change:<n>, unstage-change:<n>, revert-change:<n>   the n-th change's strip button
//   hover:<n>                         the strip as with the mouse on the n-th change
//   type:<line>:<text>                inserts text there (\n = new line)
//   undo, save, close, toggle         Ctrl+Z, Ctrl+S, Ctrl+W, the Inline / Side by Side button
//   dump                              prints the diff's changes, the index (git show :path),
//                                     HEAD, the document's text, git status and numstat
//   tabs                              prints the tabs of each group
//   shot:<name>                       the window as <screenshot>-<name>.png
//   wait:<ms>
void DiffView::runTest(QWidget *window, const QString &steps)
{
    const QStringList list = steps.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    auto *repo = window->findChild<GitRepo *>();
    auto *panel = window->findChild<GitPanel *>();
    if (!repo || !panel) {
        return;
    }
    const QString screenshot = qEnvironmentVariable("LIEVE_SCREENSHOT");
    const auto currentDiff = [window]() -> DiffView * {
        const auto diffs = window->findChildren<DiffView *>();
        for (DiffView *d : diffs) {
            if (d->isVisible()) {
                return d;
            }
        }
        return nullptr;
    };
    // The editor in a tab with the focus (else the first one shown).
    const auto editor = [window]() -> KTextEditor::View * {
        KTextEditor::View *found = nullptr;
        const auto views = window->findChildren<KTextEditor::View *>();
        for (KTextEditor::View *v : views) {
            bool inDiff = false;
            for (QWidget *w = v->parentWidget(); w && !inDiff; w = w->parentWidget()) {
                inDiff = qobject_cast<DiffView *>(w) != nullptr;
            }
            if (inDiff || !v->isVisible()) {
                continue;
            }
            if (v->isAncestorOf(QApplication::focusWidget()) || !found) {
                found = v;
            }
        }
        return found;
    };
    const auto target = [currentDiff, editor]() -> KTextEditor::View * {
        DiffView *d = currentDiff();
        return d ? d->focusedView() : editor();
    };
    const auto trigger = [window](const QKeySequence &key) {
        const auto actions = window->actions();
        for (QAction *action : actions) { // by shortcut: the text depends on the language
            if (action->shortcuts().contains(key)) {
                action->trigger();
                return;
            }
        }
        qInfo().noquote() << "diff-no-action" << key.toString();
    };
    const auto git = [repo](const QStringList &args) {
        QProcess p;
        p.setWorkingDirectory(repo->root());
        p.start(QStringLiteral("git"), args);
        p.waitForFinished(5000);
        return QString::fromUtf8(p.readAllStandardOutput());
    };
    const auto entryFor = [repo](const QString &path, GitEntry::Group group) -> std::optional<GitEntry> {
        const QList<GitEntry> &entries = repo->entries();
        for (const GitEntry &e : entries) {
            if (e.path == path && e.group == group) {
                return e;
            }
        }
        return std::nullopt;
    };
    const auto gutterOf = [window](KTextEditor::View *view) -> GitGutter * {
        const auto gutters = window->findChildren<GitGutter *>();
        for (GitGutter *g : gutters) {
            if (view && g->parent() == view->document()) {
                return g;
            }
        }
        return nullptr;
    };

    auto next = std::make_shared<std::function<void(qsizetype)>>();
    *next = [=](qsizetype i) {
        if (i >= list.size()) {
            return;
        }
        const QString step = list.at(i);
        const QString arg = step.section(QLatin1Char(':'), 1);
        int delay = 800;
        qInfo().noquote() << "diff-step" << step;
        if (step.startsWith(QLatin1String("open:")) || step.startsWith(QLatin1String("open-staged:"))) {
            const std::optional<GitEntry> entry = entryFor(arg, step.startsWith(QLatin1String("open-staged:")) ? GitEntry::Staged : GitEntry::Changes);
            if (entry) {
                Q_EMIT panel->diffRequested(*entry);
            } else {
                qInfo().noquote() << "diff-no-entry" << arg;
            }
        } else if (step.startsWith(QLatin1String("file:"))) {
            Q_EMIT panel->openFileRequested(repo->absolutePath(arg));
        } else if (step.startsWith(QLatin1String("gutter:"))) {
            if (GitGutter *gutter = gutterOf(editor())) {
                Q_EMIT gutter->clicked(arg.toInt() - 1);
            }
        } else if (step.startsWith(QLatin1String("gutter-menu:"))) {
            KTextEditor::View *view = editor();
            if (GitGutter *gutter = gutterOf(view)) {
                Q_EMIT gutter->menuRequested(arg.section(QLatin1Char(':'), 0, 0).toInt() - 1, view->mapToGlobal(view->rect().center()));
                const int choice = arg.section(QLatin1Char(':'), 1, 1).toInt();
                QTimer::singleShot(300, window, [choice] {
                    const auto tops = QApplication::topLevelWidgets();
                    for (QWidget *top : tops) {
                        auto *menu = qobject_cast<QMenu *>(top);
                        if (!menu || !menu->isVisible()) {
                            continue;
                        }
                        QList<QAction *> entries;
                        const auto actions = menu->actions();
                        for (QAction *a : actions) {
                            if (!a->isSeparator()) {
                                entries << a;
                                qInfo().noquote() << "diff-menu" << a->text() << (a->isEnabled() ? "" : "(disabled)");
                            }
                        }
                        menu->close();
                        if (choice > 0 && choice <= entries.size()) {
                            entries.at(choice - 1)->trigger();
                        }
                    }
                });
            }
        } else if (step == QLatin1String("next") || step == QLatin1String("prev")) {
            trigger(step == QLatin1String("next") ? QKeySequence(Qt::ALT | Qt::Key_F5) : QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F5));
        } else if (step.startsWith(QLatin1String("focus:"))) {
            if (DiffView *d = currentDiff()) {
                d->m_focused = arg == QLatin1String("left") ? d->m_left : d->m_right;
                if (d->m_focused) {
                    d->m_focused->setFocus();
                }
                if (d->m_strip) {
                    d->m_strip->update();
                }
            }
        } else if (step.startsWith(QLatin1String("select:"))) {
            if (KTextEditor::View *view = target()) {
                const int a = arg.section(QLatin1Char('-'), 0, 0).toInt() - 1;
                const int b = arg.section(QLatin1Char('-'), 1, 1).toInt() - 1;
                view->setCursorPosition(KTextEditor::Cursor(a, 0));
                view->setSelection(KTextEditor::Range(a, 0, b, view->document()->lineLength(b)));
            }
        } else if (step.startsWith(QLatin1String("cursor:"))) {
            if (KTextEditor::View *view = target()) {
                view->removeSelection();
                view->setCursorPosition(KTextEditor::Cursor(arg.toInt() - 1, 0));
            }
        } else if (step == QLatin1String("stage")) {
            trigger(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+Alt+S")));
        } else if (step == QLatin1String("unstage")) {
            trigger(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+N")));
        } else if (step == QLatin1String("revert")) {
            trigger(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+R")));
        } else if (step.startsWith(QLatin1String("stage-change:")) || step.startsWith(QLatin1String("unstage-change:"))
                   || step.startsWith(QLatin1String("revert-change:"))) {
            if (DiffView *d = currentDiff()) {
                const GitStaging::Command command = step.startsWith(QLatin1String("stage")) ? GitStaging::Stage
                    : step.startsWith(QLatin1String("unstage"))                            ? GitStaging::Unstage
                                                                                             : GitStaging::Revert;
                d->runOnChange(arg.toInt() - 1, command);
            }
        } else if (step.startsWith(QLatin1String("hover:"))) {
            if (DiffView *d = currentDiff(); d && d->m_strip) {
                d->m_strip->hovered = arg.toInt() - 1;
                d->m_strip->update();
            }
        } else if (step.startsWith(QLatin1String("type:"))) {
            if (KTextEditor::View *view = target()) {
                QString text = arg.section(QLatin1Char(':'), 1);
                text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                view->document()->insertText(KTextEditor::Cursor(arg.section(QLatin1Char(':'), 0, 0).toInt() - 1, 0), text);
            }
        } else if (step == QLatin1String("undo")) {
            if (KTextEditor::View *view = target()) {
                if (QAction *undo = view->actionCollection()->action(QStringLiteral("edit_undo"))) {
                    undo->trigger();
                }
            }
        } else if (step == QLatin1String("save")) {
            if (KTextEditor::View *view = target()) {
                view->document()->documentSave();
            }
        } else if (step == QLatin1String("close")) {
            trigger(QKeySequence(Qt::CTRL | Qt::Key_W));
        } else if (step == QLatin1String("toggle")) {
            const auto buttons = window->findChildren<QToolButton *>(QStringLiteral("diffToggle"));
            for (QToolButton *b : buttons) {
                if (b->isVisible()) {
                    b->click();
                    break;
                }
            }
        } else if (step == QLatin1String("dump")) {
            DiffView *d = currentDiff();
            KTextEditor::View *view = d ? d->m_right.data() : editor();
            QString path = d ? d->m_entry.path : QString();
            if (!d && view && view->document()->url().isLocalFile()) {
                path = view->document()->url().toLocalFile().mid(repo->root().size() + 1);
            }
            if (d) {
                qInfo().noquote() << "diff-title" << d->title() << "| focus" << (d->focusedView() == d->m_left ? "left" : "right");
                qInfo().noquote() << "diff-changes" << d->m_hunks.size();
                for (const LineHunk &h : std::as_const(d->m_hunks)) {
                    qInfo().noquote() << "diff-change" << QStringLiteral("-%1,%2 +%3,%4").arg(h.aStart + 1).arg(h.aCount).arg(h.bStart + 1).arg(h.bCount);
                }
                qInfo().noquote() << "diff-left" << quoted(d->m_leftDoc->text());
            }
            if (!path.isEmpty()) {
                qInfo().noquote() << "diff-index" << quoted(git({QStringLiteral("show"), QLatin1Char(':') + path}));
                qInfo().noquote() << "diff-head" << quoted(git({QStringLiteral("show"), QStringLiteral("HEAD:") + path}));
                qInfo().noquote() << "diff-status" << quoted(git({QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("--"), path}));
                qInfo().noquote() << "diff-numstat" << quoted(git({QStringLiteral("diff"), QStringLiteral("--numstat"), QStringLiteral("--"), path}));
                qInfo().noquote() << "diff-cached" << quoted(git({QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--numstat"), QStringLiteral("--"), path}));
            }
            if (view) {
                qInfo().noquote() << "diff-document" << quoted(view->document()->text()) << (view->document()->isModified() ? "[modified]" : "");
            }
            if (KTextEditor::View *focused = d ? d->focusedView() : view) {
                qInfo().noquote() << "diff-cursor" << focused->cursorPosition().line() + 1 << "selection" << focused->selection();
            }
        } else if (step == QLatin1String("tabs")) {
            const auto groups = window->findChildren<TabView *>();
            for (TabView *group : groups) {
                if (!group->parentWidget() || group->parentWidget()->objectName() != QLatin1String("editorSplit")) {
                    continue;
                }
                QStringList tabs;
                for (int t = 0; t < group->count(); ++t) {
                    tabs << (t == group->currentIndex() ? QStringLiteral("*") : QString()) + group->tabBar()->tabText(t);
                }
                qInfo().noquote() << "diff-tabs" << tabs.join(QStringLiteral(", ")) << "| title:" << window->windowTitle();
            }
        } else if (step.startsWith(QLatin1String("shot:")) && !screenshot.isEmpty()) {
            window->grab().save(QString(screenshot).replace(QStringLiteral(".png"), QLatin1Char('-') + arg + QStringLiteral(".png")));
        } else if (step.startsWith(QLatin1String("wait:"))) {
            delay = arg.toInt();
        }
        QTimer::singleShot(delay, window, [next, i] {
            (*next)(i + 1);
        });
    };
    const int at = qEnvironmentVariableIntValue("LIEVE_TEST_DIFF_AT");
    QTimer::singleShot(at > 0 ? at : 1200, window, [next] {
        (*next)(0);
    });
}
