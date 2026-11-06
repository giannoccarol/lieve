#pragma once

#include <QIcon>
#include <QList>
#include <QWidget>

class QHBoxLayout;
class QScrollArea;
class QStackedWidget;
class QTabBar;

// Tabs like VS Code's: a strip that scrolls sideways (mouse wheel, thin
// scrollbar) instead of showing arrow buttons, and a page per tab.
// Offers the part of QTabWidget's API that Lieve uses.
class TabView : public QWidget
{
    Q_OBJECT
public:
    explicit TabView(QWidget *parent = nullptr);
    ~TabView() override;

    int count() const;
    QWidget *widget(int index) const;
    int indexOf(QWidget *page) const;
    QWidget *currentWidget() const;
    int currentIndex() const;
    void setCurrentIndex(int index);
    void setCurrentWidget(QWidget *page);
    int addTab(QWidget *page, const QIcon &icon, const QString &text);
    int insertTab(int index, QWidget *page, const QString &text);
    void removeTab(int index);
    void setTabText(int index, const QString &text);
    void setTabToolTip(int index, const QString &tip);
    void setTabIcon(int index, const QIcon &icon);
    QTabBar *tabBar() const
    {
        return m_bar;
    }
    // Shown at the right end of the strip (e.g. the terminal's + button).
    void setCornerWidget(QWidget *widget);
    // Shown between the strip and the pages (the breadcrumbs).
    void setBarBelowTabs(QWidget *bar);

Q_SIGNALS:
    void currentChanged(int index);
    void tabCloseRequested(int index);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateStrip();
    void updateCloseButtons();
    void ensureVisible(int index);

    QTabBar *m_bar = nullptr;
    QScrollArea *m_scroll = nullptr;
    QStackedWidget *m_stack = nullptr;
    QHBoxLayout *m_header = nullptr;
    QList<QWidget *> m_pages; // in tab order
    QIcon m_closeIcon;
    int m_hovered = -1;
};
