#pragma once

#include <QHash>
#include <QIcon>
#include <QWidget>

class QHBoxLayout;
class QToolButton;

// The bottom bar, like VS Code's: small clickable items on the left (branch,
// sync) and on the right (blame, cursor, indentation, encoding, ...).
class StatusBar : public QWidget
{
    Q_OBJECT
public:
    explicit StatusBar(QWidget *parent = nullptr);
    void addItem(const QString &name, bool right, const QString &tip = QString());
    // Empty text hides the item.
    void setItem(const QString &name, const QString &text, const QIcon &icon = QIcon(), const QString &tip = QString());
    QToolButton *item(const QString &name) const
    {
        return m_items.value(name);
    }

Q_SIGNALS:
    void itemClicked(const QString &name);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    Qt::Edges edgesAt(const QPoint &pos) const;

    QHBoxLayout *m_left = nullptr;
    QHBoxLayout *m_right = nullptr;
    QHash<QString, QToolButton *> m_items;
};
