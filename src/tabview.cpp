#include "tabview.h"

#include "theme.h"

#include <algorithm>

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

TabView::TabView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_header = new QHBoxLayout;
    m_header->setContentsMargins(0, 0, 4, 0);
    m_header->setSpacing(0);

    m_bar = new QTabBar;
    m_bar->setUsesScrollButtons(false); // the strip scrolls instead
    m_bar->setExpanding(false);
    m_bar->setElideMode(Qt::ElideNone);
    m_bar->setTabsClosable(false); // our own × buttons, see insertTab()
    m_bar->setMouseTracking(true);
    m_closeIcon = Lieve::Theme::glyph(Lieve::Theme::Glyph::TabClose);
    m_bar->setMovable(true);
    m_bar->setDrawBase(false);
    m_bar->setDocumentMode(true);
    m_bar->installEventFilter(this);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("tabScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(false);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(m_bar);
    m_scroll->viewport()->installEventFilter(this);
    m_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_header->addWidget(m_scroll, 1);
    layout->addLayout(m_header);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (QWidget *page = m_pages.value(index)) {
            m_stack->setCurrentWidget(page);
        }
        ensureVisible(index);
        updateCloseButtons();
        Q_EMIT currentChanged(index);
    });
    connect(m_bar, &QTabBar::tabCloseRequested, this, &TabView::tabCloseRequested);
    connect(m_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        m_pages.move(from, to);
        updateCloseButtons();
    });
    updateStrip();
}

TabView::~TabView()
{
    // The pages die with this widget: their "destroyed" handlers must not
    // touch the half-destroyed tab view.
    for (QWidget *page : std::as_const(m_pages)) {
        disconnect(page, &QObject::destroyed, this, nullptr);
    }
    m_pages.clear();
}

int TabView::count() const
{
    return int(m_pages.size());
}

QWidget *TabView::widget(int index) const
{
    return m_pages.value(index);
}

int TabView::indexOf(QWidget *page) const
{
    return int(m_pages.indexOf(page));
}

QWidget *TabView::currentWidget() const
{
    return m_pages.value(m_bar->currentIndex());
}

int TabView::currentIndex() const
{
    return m_bar->currentIndex();
}

void TabView::setCurrentIndex(int index)
{
    if (index >= 0 && index < count()) {
        m_bar->setCurrentIndex(index);
    }
}

void TabView::setCurrentWidget(QWidget *page)
{
    setCurrentIndex(indexOf(page));
}

int TabView::addTab(QWidget *page, const QIcon &icon, const QString &text)
{
    const int index = insertTab(count(), page, text);
    setTabIcon(index, icon);
    return index;
}

int TabView::insertTab(int index, QWidget *page, const QString &text)
{
    index = std::clamp(index, 0, count());
    m_pages.insert(index, page); // before the bar emits currentChanged
    m_stack->addWidget(page);
    // A page deleted from elsewhere (e.g. a terminal whose shell exited) drops its tab.
    connect(page, &QObject::destroyed, this, [this, page] {
        const int i = int(m_pages.indexOf(page));
        if (i >= 0) {
            m_pages.removeAt(i);
            m_bar->removeTab(i);
            updateStrip();
        }
    });
    const int inserted = m_bar->insertTab(index, text);
    // × like VS Code: room always reserved, shown on the current tab and under the mouse.
    auto *close = new QToolButton(m_bar);
    close->setObjectName(QStringLiteral("tabClose"));
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(16, 16));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close"));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < m_bar->count(); ++i) {
            if (m_bar->tabButton(i, QTabBar::RightSide) == close) {
                Q_EMIT tabCloseRequested(i);
                return;
            }
        }
    });
    m_bar->setTabButton(inserted, QTabBar::RightSide, close);
    updateCloseButtons();
    if (m_pages.size() == 1) {
        m_stack->setCurrentWidget(page);
    }
    updateStrip();
    return inserted;
}

void TabView::removeTab(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }
    QWidget *page = m_pages.takeAt(index);
    disconnect(page, &QObject::destroyed, this, nullptr);
    m_bar->removeTab(index);
    m_stack->removeWidget(page);
    updateStrip();
}

void TabView::setTabText(int index, const QString &text)
{
    m_bar->setTabText(index, text);
    updateStrip();
}

void TabView::setTabToolTip(int index, const QString &tip)
{
    m_bar->setTabToolTip(index, tip);
}

void TabView::setTabIcon(int index, const QIcon &icon)
{
    m_bar->setTabIcon(index, icon);
    updateStrip();
}

void TabView::setCornerWidget(QWidget *widget)
{
    m_header->addWidget(widget, 0, Qt::AlignVCenter);
}

void TabView::updateStrip()
{
    m_bar->adjustSize();
    // Room for the thin scrollbar below the tabs, so nothing jumps when it appears.
    const int barHeight = std::max(m_bar->sizeHint().height(), 30);
    m_bar->resize(m_bar->sizeHint().width(), barHeight);
    m_scroll->setFixedHeight(barHeight + 4);
}

void TabView::updateCloseButtons()
{
    for (int i = 0; i < m_bar->count(); ++i) {
        auto *button = qobject_cast<QToolButton *>(m_bar->tabButton(i, QTabBar::RightSide));
        if (!button) {
            continue;
        }
        const bool shown = i == m_bar->currentIndex() || i == m_hovered;
        button->setIcon(shown ? m_closeIcon : QIcon());
        button->setAttribute(Qt::WA_TransparentForMouseEvents, !shown); // hidden × = click selects the tab
    }
}

void TabView::ensureVisible(int index)
{
    if (index < 0) {
        return;
    }
    const QRect r = m_bar->tabRect(index);
    m_scroll->ensureVisible(r.center().x(), r.center().y(), r.width() / 2 + 24, 0);
}

bool TabView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_bar && (event->type() == QEvent::MouseMove || event->type() == QEvent::Leave)) {
        const int hovered = event->type() == QEvent::Leave ? -1 : m_bar->tabAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            updateCloseButtons();
        }
    }
    // The wheel scrolls the strip (Qt's default would switch tab).
    if ((watched == m_bar || watched == m_scroll->viewport()) && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const QPoint delta = wheel->pixelDelta().isNull() ? wheel->angleDelta() / 2 : wheel->pixelDelta();
        QScrollBar *bar = m_scroll->horizontalScrollBar();
        bar->setValue(bar->value() - (delta.y() != 0 ? delta.y() : delta.x()));
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
