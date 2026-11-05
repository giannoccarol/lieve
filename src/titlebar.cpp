#include "titlebar.h"

#include "theme.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QWindow>

namespace
{
constexpr int kEdge = 5; // resize grip along the top edge

QToolButton *barButton(const QIcon &icon, const QString &tip, const QString &name, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(name);
    button->setIcon(icon);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setIconSize(QSize(16, 16));
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}
}

TitleBar::TitleBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("titleBar"));
    setFixedHeight(40);
    setMouseTracking(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 4, 2);
    layout->setSpacing(4);

    // Left: logo + menu
    m_left = new QWidget(this);
    auto *left = new QHBoxLayout(m_left);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(6);
    auto *logo = new QLabel(m_left);
    logo->setPixmap(QIcon(QStringLiteral(":/lieve.svg")).pixmap(18, 18));
    logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_menu = barButton(QIcon::fromTheme(QStringLiteral("application-menu")), QStringLiteral("Menu"), QStringLiteral("titleButton"), m_left);
    m_menu->setPopupMode(QToolButton::InstantPopup);
    left->addWidget(logo);
    left->addWidget(m_menu);
    layout->addWidget(m_left);
    layout->addStretch(1);

    // Right: layout toggles + window buttons
    m_right = new QWidget(this);
    auto *right = new QHBoxLayout(m_right);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(2);
    m_toggles = new QHBoxLayout;
    m_toggles->setSpacing(2);
    right->addLayout(m_toggles);
    right->addSpacing(10);
    m_minimize = barButton(Lieve::Theme::glyph(Lieve::Theme::Glyph::Minimize), QStringLiteral("Riduci a icona"), QStringLiteral("windowButton"), m_right);
    m_maximize = barButton(Lieve::Theme::glyph(Lieve::Theme::Glyph::Maximize), QStringLiteral("Ingrandisci"), QStringLiteral("windowButton"), m_right);
    m_close = barButton(Lieve::Theme::glyph(Lieve::Theme::Glyph::Close), QStringLiteral("Chiudi"), QStringLiteral("closeButton"), m_right);
    for (QToolButton *b : {m_minimize, m_maximize, m_close}) {
        b->setFixedSize(22, 22); // round on hover
    }
    right->setSpacing(6);
    right->addWidget(m_minimize);
    right->addWidget(m_maximize);
    right->addWidget(m_close);
    layout->addWidget(m_right);

    // Middle: the "command center"
    m_search = new QPushButton(this);
    m_search->setObjectName(QStringLiteral("commandCenter"));
    m_search->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
    m_search->setIconSize(QSize(14, 14));
    m_search->setFocusPolicy(Qt::NoFocus);
    m_search->setCursor(Qt::PointingHandCursor);
    m_search->setToolTip(QStringLiteral("Apri file (Ctrl+P)"));
    connect(m_search, &QPushButton::clicked, this, &TitleBar::searchClicked);

    connect(m_minimize, &QToolButton::clicked, this, [this] {
        window()->showMinimized();
    });
    connect(m_maximize, &QToolButton::clicked, this, [this] {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    });
    connect(m_close, &QToolButton::clicked, this, [this] {
        window()->close();
    });
    if (parent) {
        parent->window()->installEventFilter(this);
    }
}

void TitleBar::setMenu(QMenu *menu)
{
    m_menu->setMenu(menu);
}

void TitleBar::setSearchText(const QString &text)
{
    m_search->setText(text);
}

void TitleBar::addToggle(QAction *action)
{
    auto *button = new QToolButton(m_right);
    button->setObjectName(QStringLiteral("toggleButton"));
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setIconSize(QSize(16, 16));
    button->setFocusPolicy(Qt::NoFocus);
    m_toggles->addWidget(button);
}

void TitleBar::setWindowButtonsVisible(bool visible)
{
    m_minimize->setVisible(visible);
    m_maximize->setVisible(visible);
    m_close->setVisible(visible);
}

void TitleBar::placeSearch()
{
    // Centred in the window, but never over the side groups.
    const int leftLimit = m_left->geometry().right() + 12;
    const int rightLimit = m_right->geometry().left() - 12;
    int w = std::clamp(width() * 2 / 5, 220, 560);
    int x = (width() - w) / 2;
    if (x < leftLimit) {
        x = leftLimit;
    }
    if (x + w > rightLimit) {
        w = std::max(120, rightLimit - x);
    }
    const int h = 26;
    m_search->setGeometry(x, (height() - h) / 2 + 1, w, h);
}

void TitleBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeSearch();
}

Qt::Edges TitleBar::edgesAt(const QPoint &pos) const
{
    if (window()->isMaximized() || window()->isFullScreen()) {
        return {};
    }
    Qt::Edges edges;
    if (pos.y() <= kEdge) {
        edges |= Qt::TopEdge;
    }
    const QPoint inWindow = mapTo(window(), pos);
    if (inWindow.x() <= kEdge) {
        edges |= Qt::LeftEdge;
    } else if (inWindow.x() >= window()->width() - kEdge) {
        edges |= Qt::RightEdge;
    }
    return (edges & Qt::TopEdge) ? edges : Qt::Edges();
}

void TitleBar::mouseMoveEvent(QMouseEvent *event)
{
    const Qt::Edges edges = edgesAt(event->position().toPoint());
    if (edges == (Qt::TopEdge | Qt::LeftEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges == (Qt::TopEdge | Qt::RightEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && window()->windowHandle()) {
        const Qt::Edges edges = edgesAt(event->position().toPoint());
        if (edges) {
            window()->windowHandle()->startSystemResize(edges);
        } else {
            window()->windowHandle()->startSystemMove(); // drag the window
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == window() && event->type() == QEvent::WindowStateChange) {
        updateMaximizeIcon();
    }
    return QWidget::eventFilter(watched, event);
}

void TitleBar::updateMaximizeIcon()
{
    const bool maximized = window()->isMaximized();
    m_maximize->setIcon(Lieve::Theme::glyph(maximized ? Lieve::Theme::Glyph::Restore : Lieve::Theme::Glyph::Maximize));
    m_maximize->setToolTip(maximized ? QStringLiteral("Ripristina") : QStringLiteral("Ingrandisci"));
}
