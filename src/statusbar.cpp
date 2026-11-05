#include "statusbar.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QWindow>

namespace
{
constexpr int kEdge = 5; // resize grip along the bottom edge
}

StatusBar::StatusBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("statusBar"));
    setFixedHeight(26);
    setMouseTracking(true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 1, 10, 3);
    layout->setSpacing(0);
    m_left = new QHBoxLayout;
    m_left->setSpacing(2);
    m_right = new QHBoxLayout;
    m_right->setSpacing(2);
    layout->addLayout(m_left);
    layout->addStretch(1);
    layout->addLayout(m_right);
}

void StatusBar::addItem(const QString &name, bool right, const QString &tip)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("statusItem"));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(14, 14));
    button->setToolTip(tip);
    button->hide();
    connect(button, &QToolButton::clicked, this, [this, name] {
        Q_EMIT itemClicked(name);
    });
    (right ? m_right : m_left)->addWidget(button);
    m_items.insert(name, button);
}

void StatusBar::setItem(const QString &name, const QString &text, const QIcon &icon, const QString &tip)
{
    QToolButton *button = m_items.value(name);
    if (!button) {
        return;
    }
    button->setText(text);
    button->setIcon(icon);
    button->setToolButtonStyle(icon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
    if (!tip.isNull()) {
        button->setToolTip(tip);
    }
    button->setVisible(!text.isEmpty());
}

Qt::Edges StatusBar::edgesAt(const QPoint &pos) const
{
    QWidget *w = window();
    if (w->isMaximized() || w->isFullScreen() || !(w->windowFlags() & Qt::FramelessWindowHint) || pos.y() < height() - kEdge) {
        return {};
    }
    Qt::Edges edges = Qt::BottomEdge;
    const QPoint inWindow = mapTo(w, pos);
    if (inWindow.x() <= kEdge) {
        edges |= Qt::LeftEdge;
    } else if (inWindow.x() >= w->width() - kEdge) {
        edges |= Qt::RightEdge;
    }
    return edges;
}

void StatusBar::mouseMoveEvent(QMouseEvent *event)
{
    const Qt::Edges edges = edgesAt(event->position().toPoint());
    if (edges == (Qt::BottomEdge | Qt::LeftEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges == (Qt::BottomEdge | Qt::RightEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (edges) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
    QWidget::mouseMoveEvent(event);
}

void StatusBar::mousePressEvent(QMouseEvent *event)
{
    const Qt::Edges edges = edgesAt(event->position().toPoint());
    if (edges && event->button() == Qt::LeftButton && window()->windowHandle()) {
        window()->windowHandle()->startSystemResize(edges);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}
