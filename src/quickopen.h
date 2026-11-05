#pragma once

#include <QFrame>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QProcess;

// Ctrl+P: fuzzy file finder shown as an overlay at the top of the window.
// Typing "name:42" opens the file at line 42.
class QuickOpen : public QFrame
{
    Q_OBJECT
public:
    explicit QuickOpen(QWidget *parent);
    void setRoot(const QString &dir);
    void popup(const QStringList &recentFiles);

Q_SIGNALS:
    void fileChosen(const QString &path, int line);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reloadFiles();
    void refilter();
    void accept();
    void place();

    QLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QProcess *m_proc = nullptr;
    QString m_root;
    QStringList m_files; // relative paths
    QStringList m_filesLower;
    QStringList m_recent; // absolute paths
    int m_line = -1;
};
