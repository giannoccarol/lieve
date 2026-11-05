#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QToolButton;

// Lieve's own title bar (like VS Code's): menu on the left, a search box
// with the project name in the middle, layout toggles and window buttons on
// the right. Drag to move, double-click to maximise, top edge to resize.
class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(QWidget *parent = nullptr);
    void setMenu(QMenu *menu);
    void setSearchText(const QString &text);
    void addToggle(QAction *action);
    void setWindowButtonsVisible(bool visible);

Q_SIGNALS:
    void searchClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Qt::Edges edgesAt(const QPoint &pos) const;
    void placeSearch();
    void updateMaximizeIcon();

    QWidget *m_left = nullptr;
    QWidget *m_right = nullptr;
    QHBoxLayout *m_toggles = nullptr;
    QToolButton *m_menu = nullptr;
    QPushButton *m_search = nullptr;
    QToolButton *m_minimize = nullptr;
    QToolButton *m_maximize = nullptr;
    QToolButton *m_close = nullptr;
};
