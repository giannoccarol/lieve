#include "stickyscroll.h"

#include "outline.h"

#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Attribute>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kMaxLines = 5;
constexpr int kShadowHeight = 5;

// A soft shadow under the last sticky line, like VS Code's; lets the mouse through.
class StickyShadow : public QWidget
{
public:
    explicit StickyShadow(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
    QColor color;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QLinearGradient gradient(0, 0, 0, height());
        QColor end = color;
        end.setAlpha(0);
        gradient.setColorAt(0, color);
        gradient.setColorAt(1, end);
        p.fillRect(rect(), gradient);
    }
};

int firstNonSpace(const QString &s)
{
    for (int i = 0; i < s.size(); ++i) {
        if (!s.at(i).isSpace()) {
            return i;
        }
    }
    return 0;
}
}

StickyScroll::StickyScroll(KTextEditor::View *view, Lieve::DocumentScopes *scopes)
    : QWidget(view)
    , m_view(view)
    , m_scopes(scopes)
{
    setObjectName(QStringLiteral("stickyScroll"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    hide();
    m_shadow = new StickyShadow(view);
    // KTextEditor's border with the line numbers: the sticky lines show theirs in its place.
    const auto children = view->findChildren<QWidget *>();
    for (QWidget *child : children) {
        if (child->inherits("KateIconBorder")) {
            m_border = child;
            break;
        }
    }

    m_editTimer.setSingleShot(true);
    m_editTimer.setInterval(150);
    connect(&m_editTimer, &QTimer::timeout, this, [this] {
        m_layoutsStale = true;
        refresh();
    });
    connect(view, &KTextEditor::View::verticalScrollPositionChanged, this, &StickyScroll::refresh);
    connect(view, &KTextEditor::View::displayRangeChanged, this, &StickyScroll::refresh);
    connect(view, &KTextEditor::View::horizontalScrollPositionChanged, this, [this] {
        if (!isHidden()) {
            update();
        }
    });
    connect(view, &KTextEditor::View::configChanged, this, [this] { // font, colours, word wrap
        m_layoutsStale = true;
        refresh();
    });
    connect(view, &KTextEditor::View::cursorPositionChanged, this, [this] {
        QTimer::singleShot(0, this, &StickyScroll::revealCursor);
    });
    connect(view->document(), &KTextEditor::Document::textChanged, this, [this] {
        m_editTimer.start(); // the scopes and the lines shown may change
    });
    connect(scopes, &Lieve::DocumentScopes::changed, this, [this](KTextEditor::Document *doc) {
        if (m_view && doc == m_view->document()) {
            m_layoutsStale = true;
            refresh();
        }
    });
    view->installEventFilter(this);
    // Scrolling from code (setScrollPosition, a restored session) tells no
    // one: the text area's next paint shows the new top line.
    if (QWidget *inner = view->focusProxy()) {
        inner->installEventFilter(this);
    }
    QTimer::singleShot(0, this, &StickyScroll::refresh);
}

StickyScroll::~StickyScroll()
{
    delete m_shadow.data();
}

StickyScroll::Metrics StickyScroll::metrics() const
{
    Metrics m;
    m.font = m_view->configValue(QStringLiteral("font")).value<QFont>();
    const QFontMetricsF fm(m.font);
    // KTextEditor's row: the font's height, more with a custom line height
    // (measured on two rows on screen).
    const int fontHeight = std::max(1, int(std::ceil(fm.ascent() + fm.descent())));
    m.rowHeight = fontHeight;
    if (!m_view->configValue(QStringLiteral("dynamic-word-wrap")).toBool()) {
        const int top = m_view->firstDisplayedLine();
        for (int line = top; line < top + 3; ++line) {
            const QPoint a = m_view->cursorToCoordinate(KTextEditor::Cursor(line, 0));
            const QPoint b = m_view->cursorToCoordinate(KTextEditor::Cursor(line + 1, 0));
            if (a.y() >= 0 && b.y() > a.y()) {
                m.rowHeight = b.y() - a.y();
                break;
            }
        }
    }
    m.ascent = fm.ascent() + (m.rowHeight - fontHeight) / 2.0;
    return m;
}

void StickyScroll::refresh()
{
    KTextEditor::View *view = m_view;
    if (!view || !view->isVisible()) {
        return; // again when shown
    }
    const int top = view->firstDisplayedLine();
    m_top = top;
    if (top <= 0) { // at the top of the file nothing sticks: no work
        setLines({});
        return;
    }
    const QRect area = view->textAreaRect();
    m_metrics = metrics();
    const int h = m_metrics.rowHeight;
    const int maxLines = std::min(kMaxLines, area.height() / h / 3); // a third of the view at most
    const auto tree = maxLines > 0 ? m_scopes->scopes(view->document()) : nullptr;
    if (!tree || tree->scopes.empty()) {
        setLines({});
        return;
    }
    const QList<int> sticky = tree->sticky(
        [&](int row) {
            const KTextEditor::Cursor c = view->coordinatesToCursor(QPoint(area.left() + 1, area.top() + row * h + h / 2));
            return c.isValid() ? c.line() : -1;
        },
        maxLines);
    QList<int> lines;
    for (const int i : sticky) {
        lines.append(tree->scopes[i].startLine);
    }
    setLines(lines);
}

void StickyScroll::setLines(const QList<int> &lines)
{
    if (lines.isEmpty()) {
        m_lines.clear();
        m_layouts.clear();
        m_hovered = -1;
        hide();
        if (m_shadow) {
            m_shadow->hide();
        }
        return;
    }
    const QRect area = m_view->textAreaRect();
    int left = area.left();
    if (m_border && m_border->isVisible()) {
        left = std::min(left, m_border->mapTo(m_view.data(), QPoint(0, 0)).x());
    }
    const QRect bounds(left, area.top(), area.right() - left + 1, int(lines.size()) * m_metrics.rowHeight);
    const bool changed = lines != m_lines || m_layoutsStale;
    if (changed) {
        m_lines = lines;
        buildLayouts();
        if (m_hovered >= int(m_lines.size())) {
            m_hovered = -1;
        }
    }
    if (bounds != geometry()) {
        setGeometry(bounds);
    }
    if (m_shadow) {
        const QColor background = m_view->configValue(QStringLiteral("background-color")).value<QColor>();
        auto *shadow = static_cast<StickyShadow *>(m_shadow.data());
        shadow->color = QColor(0, 0, 0, background.lightness() < 128 ? 80 : 28);
        shadow->setGeometry(bounds.left(), bounds.bottom() + 1, bounds.width(), kShadowHeight);
    }
    if (isHidden()) {
        show();
        raise();
        if (m_shadow) {
            m_shadow->show();
            m_shadow->raise();
        }
    }
    if (changed) {
        update();
        if (m_shadow) {
            m_shadow->update();
        }
    }
}

void StickyScroll::buildLayouts()
{
    m_layouts.clear();
    m_layoutsStale = false;
    KTextEditor::Document *doc = m_view->document();
    // As KTextEditor lays out a line: same font, tab stops and highlighting.
    const QFontMetricsF fm(m_metrics.font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    option.setTabStopDistance(std::max(1, doc->configValue(QStringLiteral("tab-width")).toInt()) * fm.horizontalAdvance(QLatin1Char(' ')));
    for (const int line : std::as_const(m_lines)) {
        auto layout = std::make_unique<QTextLayout>(doc->line(line), m_metrics.font);
        layout->setTextOption(option);
        QList<QTextLayout::FormatRange> formats;
        const QList<KTextEditor::AttributeBlock> blocks = m_view->lineAttributes(line);
        for (const KTextEditor::AttributeBlock &block : blocks) {
            if (block.attribute) {
                QTextCharFormat format = *block.attribute;
                format.clearBackground();
                formats.append({block.start, block.length, format});
            }
        }
        layout->setFormats(formats);
        layout->beginLayout();
        QTextLine textLine = layout->createLine();
        if (textLine.isValid()) {
            textLine.setLineWidth(1e7);
            textLine.setLeadingIncluded(true);
            textLine.setPosition(QPointF(0, m_metrics.ascent - textLine.ascent()));
        }
        layout->endLayout();
        m_layouts.push_back(std::move(layout));
    }
}

int StickyScroll::textLeft() const
{
    return m_view ? m_view->textAreaRect().left() - geometry().left() : 0;
}

void StickyScroll::paintEvent(QPaintEvent *)
{
    KTextEditor::View *view = m_view;
    if (!view) {
        return;
    }
    QPainter p(this);
    // The editor's own colours, and its line numbers laid out as KTextEditor does.
    const QColor background = view->configValue(QStringLiteral("background-color")).value<QColor>();
    const QColor border = view->configValue(QStringLiteral("icon-border-color")).value<QColor>();
    const QColor number = view->configValue(QStringLiteral("line-number-color")).value<QColor>();
    const KSyntaxHighlighting::Theme theme = view->theme();
    const QColor separator = QColor::fromRgba(theme.editorColor(KSyntaxHighlighting::Theme::Separator));
    const QColor hover = QColor::fromRgba(theme.editorColor(KSyntaxHighlighting::Theme::CurrentLine));
    const QColor text = view->defaultStyleAttribute(KSyntaxHighlighting::Theme::TextStyle::Normal)->foreground().color();
    const int h = m_metrics.rowHeight;
    const int textX = textLeft();
    const int gutter = m_border && m_border->isVisible() ? m_border->mapTo(view, QPoint(m_border->width(), 0)).x() - geometry().left() : 0;
    const int scrollX = view->horizontalScrollBar() ? view->horizontalScrollBar()->value() : 0;
    const bool numbers = view->configValue(QStringLiteral("line-numbers")).toBool();
    const QFontMetricsF fm(m_metrics.font);
    qreal digit = 0;
    for (char c = '0'; c <= '9'; ++c) {
        digit = std::max(digit, std::ceil(fm.horizontalAdvance(QLatin1Char(c))));
    }
    const int digits = int(std::ceil(std::log10(double(view->document()->lines() + 1))));
    const int numbersWidth = int(std::ceil((digits + 1) * digit));
    const int numbersX = view->configValue(QStringLiteral("icon-bar")).toBool() ? int(fm.height()) : 0;
    p.setFont(m_metrics.font);
    for (int row = 0; row < int(m_lines.size()); ++row) {
        const int y = row * h;
        const QColor rowBackground = row == m_hovered ? hover : background;
        if (gutter > 0) {
            p.fillRect(0, y, gutter, h, row == m_hovered ? hover : border);
            p.fillRect(gutter - 4, y, 4, h, rowBackground); // the margin before the text
            if (numbers) {
                p.setPen(number);
                p.drawText(int(numbersX + digit / 2),
                           y,
                           int(numbersWidth - digit),
                           h,
                           Qt::TextDontClip | Qt::AlignRight | Qt::AlignVCenter,
                           QString::number(m_lines.at(row) + 1));
            }
            p.setPen(separator);
            p.drawLine(gutter - 4, y, gutter - 4, y + h - 1);
        }
        p.fillRect(gutter, y, width() - gutter, h, rowBackground);
        if (row < int(m_layouts.size())) {
            p.save();
            p.setClipRect(textX, y, width() - textX, h);
            p.setPen(text);
            m_layouts[row]->draw(&p, QPointF(textX - scrollX, y));
            p.restore();
        }
    }
}

void StickyScroll::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        goTo(int(event->position().y()) / std::max(1, m_metrics.rowHeight), int(event->position().x()));
    }
    event->accept();
}

void StickyScroll::mouseMoveEvent(QMouseEvent *event)
{
    const int row = int(event->position().y()) / std::max(1, m_metrics.rowHeight);
    const int hovered = row >= 0 && row < int(m_lines.size()) ? row : -1;
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
}

void StickyScroll::leaveEvent(QEvent *)
{
    if (m_hovered >= 0) {
        m_hovered = -1;
        update();
    }
}

void StickyScroll::wheelEvent(QWheelEvent *event)
{
    // The editor scrolls under the sticky lines too.
    QWidget *inner = m_view ? m_view->focusProxy() : nullptr;
    if (!inner) {
        return;
    }
    QWheelEvent copy(inner->mapFromGlobal(event->globalPosition()),
                     event->globalPosition(),
                     event->pixelDelta(),
                     event->angleDelta(),
                     event->buttons(),
                     event->modifiers(),
                     event->phase(),
                     event->inverted(),
                     event->source());
    QCoreApplication::sendEvent(inner, &copy);
    event->setAccepted(copy.isAccepted());
}

void StickyScroll::clickRow(int row)
{
    goTo(row, textLeft() + 2);
}

// Moves the cursor to the clicked place of a sticky line and scrolls the
// line to where it is shown (just below the outer sticky lines).
void StickyScroll::goTo(int row, int x)
{
    KTextEditor::View *view = m_view;
    if (!view || row < 0 || row >= int(m_lines.size())) {
        return;
    }
    const int line = m_lines.at(row);
    int column = firstNonSpace(view->document()->line(line));
    if (row < int(m_layouts.size()) && x >= textLeft()) {
        const QTextLine textLine = m_layouts[row]->lineAt(0);
        if (textLine.isValid()) {
            column = textLine.xToCursor(x - textLeft() + (view->horizontalScrollBar() ? view->horizontalScrollBar()->value() : 0));
        }
    }
    view->setCursorPosition(KTextEditor::Cursor(line, column));
    view->setScrollPosition(KTextEditor::Cursor(std::max(0, line - row), 0));
    view->setFocus();
    refresh();
}

// The cursor moved under the sticky lines (arrow keys, Page Up, a search, a
// jump to a symbol): scrolls up just enough to show it. Not while the mouse
// is selecting.
void StickyScroll::revealCursor()
{
    KTextEditor::View *view = m_view;
    if (!view || !view->isVisible() || QApplication::mouseButtons() != Qt::NoButton) {
        return;
    }
    if (view->firstDisplayedLine() != m_top) { // it scrolled to the cursor meanwhile
        refresh();
    }
    for (int guard = 0; guard < 3 && !isHidden() && m_metrics.rowHeight > 0; ++guard) {
        const QPoint caret = view->cursorPositionCoordinates();
        if (caret.y() < 0 || caret.y() > geometry().bottom()) {
            return;
        }
        const int row = std::max(0, (caret.y() - geometry().top()) / m_metrics.rowHeight);
        const int top = view->firstDisplayedLine();
        if (top <= 0) {
            return;
        }
        view->setScrollPosition(KTextEditor::Cursor(std::max(0, top - (int(m_lines.size()) - row)), 0));
        refresh();
    }
}

bool StickyScroll::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        QTimer::singleShot(0, this, &StickyScroll::refresh); // once KTextEditor has laid out its rows
    } else if (event->type() == QEvent::Paint && m_view && watched == m_view->focusProxy() && m_view->firstDisplayedLine() != m_top) {
        QTimer::singleShot(0, this, &StickyScroll::refresh);
    }
    return QWidget::eventFilter(watched, event);
}
