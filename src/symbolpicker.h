#pragma once

#include <QFrame>
#include <QIcon>
#include <QList>
#include <QString>

class QLineEdit;
class QListWidget;

// A filtered list shown as an overlay at the top of the window, like Quick
// Open: the symbols of a file (Ctrl+Shift+O) or the places a definition can
// be (F12 with more than one answer).
class SymbolPicker : public QFrame
{
    Q_OBJECT
public:
    struct Entry {
        QString name;
        QString detail; // shown dimmed after the name
        QIcon icon;
        int index = 0; // given back by highlighted() and chosen()
    };

    explicit SymbolPicker(QWidget *parent);
    void popup(const QString &placeholder, const QList<Entry> &entries);

Q_SIGNALS:
    void highlighted(int index);
    void chosen(int index);
    void cancelled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refilter();
    void accept();
    void cancel();
    void place();

    QLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QList<Entry> m_entries;
    QStringList m_lower;
    bool m_closing = false;
};
