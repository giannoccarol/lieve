#include "linetint.h"

#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/MovingRange>
#include <KTextEditor/View>

#include <QEvent>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
const char kName[] = "lieveLineTint";

// KTextEditor's text area (KateViewInternal), where the text is painted.
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
}

LineTint::LineTint(KTextEditor::View *view, QWidget *area)
    : QWidget(area)
    , m_view(view)
{
    setObjectName(QLatin1String(kName));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(area->rect());
    lower(); // under the messages KTextEditor floats over the text
    area->installEventFilter(this);
    show();
}

LineTint *LineTint::of(KTextEditor::View *view)
{
    QWidget *area = view ? textArea(view) : nullptr;
    if (!area) {
        return nullptr;
    }
    if (auto *layer = area->findChild<QWidget *>(QLatin1String(kName), Qt::FindDirectChildrenOnly)) {
        return static_cast<LineTint *>(layer);
    }
    return new LineTint(view, area);
}

void LineTint::set(const QList<Span> &spans, const QList<Edge> &edges)
{
    m_spans = spans;
    m_edges = edges;
    update();
}

LineTint::Span LineTint::spanOf(const KTextEditor::MovingRange *range)
{
    const KTextEditor::Range r = range->toRange();
    int last = r.end().line();
    if (r.end().column() == 0 && last > r.start().line()) {
        --last;
    }
    const KTextEditor::Attribute::Ptr &attribute = range->attribute();
    return {r.start().line(), last, attribute ? attribute->background().color() : QColor()};
}

bool LineTint::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return false;
}

void LineTint::paintEvent(QPaintEvent *)
{
    KTextEditor::View *view = m_view;
    if (!view || (m_spans.isEmpty() && m_edges.isEmpty())) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    const int lines = doc->lines();
    const int first = view->firstDisplayedLine();
    const int last = std::min(view->lastDisplayedLine(), lines - 1);
    if (first < 0 || last < first) {
        return;
    }
    // Where a cursor position is drawn, in this layer; none if not shown.
    const auto at = [this, view](int line, int column) -> std::optional<QPoint> {
        const QPoint p = view->cursorToCoordinate(KTextEditor::Cursor(line, column));
        if (p == QPoint(-1, -1)) {
            return std::nullopt;
        }
        return mapFrom(view, p);
    };
    // Row height: the smallest step between the first rows (a wrapped line spans several).
    const QFontMetricsF metrics(view->configValue(QStringLiteral("font")).value<QFont>());
    int rowHeight = 0;
    std::optional<QPoint> previous = at(first, 0);
    for (int line = first + 1; line <= std::min(last, first + 8); ++line) {
        const std::optional<QPoint> p = at(line, 0);
        if (p && previous && p->y() > previous->y()) {
            rowHeight = rowHeight > 0 ? std::min(rowHeight, p->y() - previous->y()) : p->y() - previous->y();
        }
        previous = p;
    }
    if (rowHeight <= 0) {
        rowHeight = std::max(1, int(std::ceil(metrics.ascent() + metrics.descent())));
    }

    QPainter p(this);
    for (const Span &span : std::as_const(m_spans)) {
        if (!span.color.isValid() || span.color.alpha() == 0) {
            continue;
        }
        for (int line = std::max(span.first, first); line <= std::min(span.last, last); ++line) {
            const std::optional<QPoint> start = at(line, 0);
            const int length = doc->lineLength(line);
            const std::optional<QPoint> end = length > 0 ? at(line, length) : start;
            if (!start || !end) {
                continue; // folded away
            }
            if (start->x() > 0) {
                p.fillRect(QRect(0, start->y(), start->x(), rowHeight), span.color);
            }
            // A wrapped line: the free space at the end of each of its rows.
            if (end->y() > start->y()) {
                const QString text = doc->line(line);
                for (int y = start->y(); y < end->y(); y += rowHeight) {
                    int lo = 0; // the last column drawn on this row
                    int hi = length;
                    while (hi - lo > 1) {
                        const int mid = (lo + hi) / 2;
                        const std::optional<QPoint> m = at(line, mid);
                        if (m && m->y() > y) {
                            hi = mid;
                        } else {
                            lo = mid;
                        }
                    }
                    const std::optional<QPoint> c = at(line, lo);
                    const int x = c ? c->x() + int(std::ceil(metrics.horizontalAdvance(text.at(lo)))) : width();
                    p.fillRect(QRect(x, y, width() - x, rowHeight), span.color);
                }
            }
            p.fillRect(QRect(end->x(), end->y(), width() - end->x(), rowHeight), span.color);
        }
    }
    for (const Edge &edge : std::as_const(m_edges)) {
        std::optional<int> y;
        if (edge.line >= first && edge.line <= last) {
            if (const std::optional<QPoint> top = at(edge.line, 0)) {
                y = top->y();
            }
        } else if (edge.line == lines && last == lines - 1) { // below the last line
            if (const std::optional<QPoint> end = at(last, doc->lineLength(last))) {
                y = end->y() + rowHeight;
            }
        }
        if (y) {
            p.fillRect(QRect(0, std::max(0, *y - 1), width(), 1), edge.color);
        }
    }
}
