#include "gitgutter.h"

#include "linediff.h"
#include "theme.h"

#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QProcess>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
// KateIconBorder, from the right: ... line numbers | 2 px | markers (3 px) |
// folding arrows (as wide as the font is high) | 4 px margin.
constexpr int kColumnWidth = 3;
constexpr int kEndMargin = 4;
constexpr int kCompareDelay = 250; // ms after the last edit
}

GitGutter::GitGutter(KTextEditor::Document *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kCompareDelay);
    connect(&m_timer, &QTimer::timeout, this, &GitGutter::compare);
    const auto later = [this] {
        if (m_hasBase) {
            m_timer.start();
        }
    };
    connect(document, &KTextEditor::Document::textChanged, this, later);
    connect(document, &KTextEditor::Document::reloaded, this, later);
    // Until then the marks move with the lines around them (Enter, joining lines).
    connect(document, &KTextEditor::Document::lineWrapped, this, [this](KTextEditor::Document *, KTextEditor::Cursor position) {
        shiftLines(position.column() == 0 ? position.line() : position.line() + 1, 1);
    });
    connect(document, &KTextEditor::Document::lineUnwrapped, this, [this](KTextEditor::Document *, int line) {
        shiftLines(line, -1);
    });
}

GitGutter::~GitGutter()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

void GitGutter::addView(KTextEditor::View *view)
{
    m_borders.removeIf([](const Border &b) {
        return !b.widget || !b.view;
    });
    // KTextEditor has no API for this column: the marks are painted on its
    // border widget, right after it paints itself.
    const auto children = view->findChildren<QWidget *>();
    for (QWidget *child : children) {
        if (child->inherits("KateIconBorder")) {
            child->installEventFilter(this);
            m_borders.append({child, view});
            child->update();
            return;
        }
    }
}

void GitGutter::setSource(const QString &root, const QString &relative, const QString &state)
{
    const QString key = state.isEmpty() ? QString() : root + QLatin1Char('\n') + relative + QLatin1Char('\n') + state;
    if (key == m_key) {
        return;
    }
    m_key = key;
    m_root = root;
    m_relative = relative;
    if (key.isEmpty()) { // untracked, a conflict, or outside the repository
        m_stale = false;
        if (m_process) {
            m_process->disconnect(this);
            m_process->kill();
            m_process->deleteLater();
            m_process = nullptr;
        }
        clear();
        return;
    }
    m_stale = true;
    if (isShown()) {
        fetch();
    } // else: when a view of it is painted
}

bool GitGutter::isShown() const
{
    return std::any_of(m_borders.cbegin(), m_borders.cend(), [](const Border &b) {
        return b.widget && b.widget->isVisible();
    });
}

void GitGutter::fetch()
{
    if (!m_stale || m_key.isEmpty()) {
        return;
    }
    m_stale = false;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
    }
    auto *process = new QProcess(this);
    m_process = process;
    process->setWorkingDirectory(m_root);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_process = nullptr;
        if (status == QProcess::NormalExit && code == 0) {
            setBase(process->readAllStandardOutput());
        } else {
            clear(); // not in the index (e.g. an ignored file)
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            m_process = nullptr;
        }
    });
    // The staged version, as `git show :path` (VS Code compares with it too).
    process->start(QStringLiteral("git"), {QStringLiteral("cat-file"), QStringLiteral("blob"), QLatin1Char(':') + m_relative});
}

void GitGutter::setBase(const QByteArray &data)
{
    if (!m_document || data.left(8000).contains('\0')) { // binary: no line marks
        clear();
        return;
    }
    QStringDecoder decoder(m_document->encoding().toLatin1().constData());
    if (!decoder.isValid()) {
        decoder = QStringDecoder(QStringDecoder::Utf8);
    }
    const QString text = decoder.decode(data);
    const QStringView all(text);
    QList<size_t> base;
    base.reserve(text.count(QLatin1Char('\n')) + 1);
    for (qsizetype start = 0;;) {
        const qsizetype end = all.indexOf(QLatin1Char('\n'), start);
        QStringView line = all.mid(start, (end < 0 ? all.size() : end) - start);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        base.append(qHash(line));
        if (end < 0) {
            break;
        }
        start = end + 1;
    }
    // The newline at the end of the file is not a line of its own (see compare()).
    if (base.size() > 0 && base.last() == qHash(QStringView())) {
        base.removeLast();
    }
    m_base = std::move(base);
    m_hasBase = true;
    compare();
}

void GitGutter::compare()
{
    m_timer.stop();
    if (!m_hasBase || !m_document) {
        return;
    }
    int lines = m_document->lines();
    if (lines > 0 && m_document->lineLength(lines - 1) == 0) {
        --lines; // the newline at the end of the file
    }
    QList<size_t> current;
    current.reserve(lines);
    for (int i = 0; i < lines; ++i) {
        current.append(qHash(m_document->line(i)));
    }
    QList<Change> changes;
    const QList<Lieve::LineHunk> hunks = Lieve::diffLines(m_base, current);
    changes.reserve(hunks.size());
    for (const Lieve::LineHunk &h : hunks) {
        if (h.bCount == 0) {
            changes.append({h.bStart, h.aCount, Deleted});
        } else {
            changes.append({h.bStart, h.bCount, h.aCount == 0 ? Added : Modified});
        }
    }
    if (changes != m_changes) {
        m_changes = std::move(changes);
        repaint();
    }
}

void GitGutter::clear()
{
    m_timer.stop();
    const bool had = m_hasBase;
    m_hasBase = false;
    m_base = QList<size_t>();
    m_changes = QList<Change>();
    if (had) {
        repaint(); // KTextEditor's own markers come back
    }
}

void GitGutter::repaint()
{
    for (const Border &b : std::as_const(m_borders)) {
        if (b.widget) {
            b.widget->update();
        }
    }
}

void GitGutter::shiftLines(int from, int delta)
{
    // `delta` lines were inserted at `from`, or line `from` was joined to the one above.
    if (m_changes.isEmpty()) {
        return;
    }
    for (Change &c : m_changes) {
        if (c.line >= from) {
            c.line = std::max(0, c.line + delta);
        } else if (c.kind != Deleted && from < c.line + c.count) {
            c.count = std::max(1, c.count + delta);
        }
    }
    repaint();
}

const GitGutter::Change *GitGutter::changeOn(int line) const
{
    // the last change starting on or before the line
    auto it = std::upper_bound(m_changes.cbegin(), m_changes.cend(), line, [](int l, const Change &c) {
        return l < c.line;
    });
    if (it == m_changes.cbegin()) {
        return nullptr;
    }
    --it;
    return it->kind != Deleted && line < it->line + it->count ? &*it : nullptr;
}

int GitGutter::nextChange(int line, bool forward) const
{
    if (m_changes.isEmpty()) {
        return -1;
    }
    // Like VS Code: a deletion is reached on the line above it.
    const auto target = [](const Change &c) {
        return c.kind == Deleted ? std::max(0, c.line - 1) : c.line;
    };
    if (forward) {
        for (const Change &c : m_changes) {
            if (target(c) > line) {
                return target(c);
            }
        }
        return target(m_changes.first());
    }
    for (auto it = m_changes.crbegin(); it != m_changes.crend(); ++it) {
        if (target(*it) < line) {
            return target(*it);
        }
    }
    return target(m_changes.last());
}

int GitGutter::diffLineFor(const QString &diff, int line)
{
    static const QRegularExpression header(QStringLiteral("^@@ -\\d+(?:,\\d+)? \\+(\\d+)"));
    const QStringList lines = diff.split(QLatin1Char('\n'));
    int found = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = header.match(lines.at(i));
        if (!match.hasMatch()) {
            continue;
        }
        if (match.captured(1).toInt() > line + 1) { // starts below: the hunk above, or this first one
            return found >= 0 ? found : i;
        }
        found = i;
    }
    return found;
}

GitGutter::Geometry GitGutter::geometry(const Border &border) const
{
    Geometry g;
    KTextEditor::View *view = border.view;
    if (!view->configValue(QStringLiteral("modification-markers")).toBool()) {
        return g; // no column to paint in
    }
    const QFontMetricsF metrics(view->configValue(QStringLiteral("font")).value<QFont>());
    const int folding = view->configValue(QStringLiteral("folding-bar")).toBool() ? int(metrics.height()) : 0;
    g.x = border.widget->width() - kEndMargin - folding - kColumnWidth;
    g.rowHeight = std::max(1, int(std::ceil(metrics.ascent() + metrics.descent())));
    return g;
}

int GitGutter::lineAt(const Border &border, QPoint pos) const
{
    if (!m_hasBase || m_changes.isEmpty()) {
        return -1;
    }
    const Geometry g = geometry(border);
    if (g.x < 0 || pos.x() < g.x - 2 || pos.x() >= g.x + kColumnWidth + 2) {
        return -1;
    }
    KTextEditor::View *view = border.view;
    const QPoint inView = view->mapFrom(border.widget.data(), pos);
    const KTextEditor::Cursor cursor = view->coordinatesToCursor(QPoint(view->textAreaRect().left(), inView.y()));
    if (!cursor.isValid()) {
        return -1;
    }
    const int line = cursor.line();
    if (changeOn(line)) {
        return line;
    }
    for (const Change &c : m_changes) { // a deletion wedge, on the edge between two lines
        if (c.kind == Deleted && (c.line == line || c.line == line + 1)) {
            return line;
        }
    }
    return -1;
}

bool GitGutter::eventFilter(QObject *watched, QEvent *event)
{
    const auto it = std::find_if(m_borders.cbegin(), m_borders.cend(), [watched](const Border &b) {
        return b.widget == watched;
    });
    if (it == m_borders.cend() || !it->view) {
        return false;
    }
    const Border border = *it;
    switch (event->type()) {
    case QEvent::Paint:
        if (m_stale) {
            QTimer::singleShot(0, this, &GitGutter::fetch); // shown at last
        }
        if (!m_hasBase) {
            return false;
        }
        static_cast<QObject *>(border.widget.data())->event(event); // KTextEditor paints first
        paint(border);
        return true;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        m_pressedLine = mouse->button() == Qt::LeftButton ? lineAt(border, mouse->position().toPoint()) : -1;
        return m_pressedLine >= 0; // a click on a mark does not move the cursor
    }
    case QEvent::MouseButtonRelease: {
        if (m_pressedLine < 0) {
            return false;
        }
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (lineAt(border, mouse->position().toPoint()) == m_pressedLine) {
            Q_EMIT clicked(std::exchange(m_pressedLine, -1));
        }
        m_pressedLine = -1;
        return true;
    }
    case QEvent::MouseMove: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (lineAt(border, mouse->position().toPoint()) >= 0) {
            border.widget->setCursor(Qt::PointingHandCursor);
        } else {
            border.widget->unsetCursor();
        }
        return false;
    }
    case QEvent::Leave:
        border.widget->unsetCursor();
        return false;
    default:
        return false;
    }
}

void GitGutter::paint(const Border &border)
{
    QWidget *widget = border.widget;
    KTextEditor::View *view = border.view;
    const Geometry g = geometry(border);
    if (g.x < 0) {
        return;
    }

    // The line rows on screen; folded lines share their fold's row and are skipped.
    struct Row {
        int line;
        int top;
    };
    QVarLengthArray<Row, 128> rows;
    const int lastLine = view->lastDisplayedLine();
    for (int line = view->firstDisplayedLine(); line <= lastLine; ++line) {
        const QPoint p = view->cursorToCoordinate(KTextEditor::Cursor(line, 0));
        if (p.y() < 0) {
            continue;
        }
        const int top = widget->mapFrom(view, p).y();
        if (rows.isEmpty() || top > rows.last().top) {
            rows.append({line, top});
        }
    }
    int rowHeight = g.rowHeight;
    for (qsizetype i = 1; i < rows.size(); ++i) {
        rowHeight = i == 1 ? rows[i].top - rows[i - 1].top : std::min(rowHeight, rows[i].top - rows[i - 1].top);
    }

    // KTextEditor's own markers (lines edited since the file was opened) give way.
    QPainter p(widget);
    p.fillRect(g.x, 0, kColumnWidth, widget->height(), view->configValue(QStringLiteral("icon-border-color")).value<QColor>());
    const QPoint cursor = view->cursorPositionCoordinates();
    if (cursor.y() >= 0) {
        p.fillRect(g.x,
                   widget->mapFrom(view, cursor).y(),
                   kColumnWidth,
                   rowHeight,
                   QColor::fromRgba(view->theme().editorColor(KSyntaxHighlighting::Theme::CurrentLine)));
    }
    if (rows.isEmpty() || m_changes.isEmpty()) {
        return;
    }

    // VS Code's gutter colours (Dark/Light Modern).
    const bool dark = Lieve::Theme::colors().dark;
    const QColor added(0x2e, 0xa0, 0x43);
    const QColor modified = dark ? QColor(0x00, 0x78, 0xd4) : QColor(0x00, 0x5f, 0xb8);
    const QColor deleted(0xf8, 0x51, 0x49);

    const int last = rows.last().line;
    const QPoint end = view->cursorToCoordinate(KTextEditor::Cursor(last, m_document ? m_document->lineLength(last) : 0));
    const int lastBottom = end.y() >= 0 ? widget->mapFrom(view, end).y() + rowHeight : widget->height();
    const auto bottomOf = [&](qsizetype i) {
        return i + 1 < rows.size() ? rows[i + 1].top : lastBottom;
    };
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (const Change *c = changeOn(rows[i].line)) {
            p.fillRect(g.x, rows[i].top, kColumnWidth, bottomOf(i) - rows[i].top, c->kind == Added ? added : modified);
        }
    }

    // Deletions: a small wedge on the edge above the line that followed them.
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(deleted);
    const qreal half = std::clamp(rowHeight / 4, 3, 5);
    auto it = std::lower_bound(m_changes.cbegin(), m_changes.cend(), rows.first().line, [](const Change &c, int l) {
        return c.line < l;
    });
    for (; it != m_changes.cend() && it->line <= last + 1; ++it) {
        if (it->kind != Deleted) {
            continue;
        }
        const auto row = std::lower_bound(rows.cbegin(), rows.cend(), it->line, [](const Row &r, int l) {
            return r.line < l;
        });
        const qreal y = row != rows.cend() ? row->top : lastBottom;
        const QPolygonF wedge{QPointF(g.x, y - half), QPointF(g.x + half + 1, y), QPointF(g.x, y + half)};
        p.drawPolygon(wedge);
    }
}
