#pragma once

#include <QHash>
#include <QTimer>
#include <QWidget>

class QJsonObject;
class QLabel;
class QLineEdit;
class QProcess;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// "Find in files" backed by ripgrep (respects .gitignore, very fast).
class SearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanel(QWidget *parent = nullptr);
    void setRoot(const QString &dir);
    void focusQuery(const QString &text = QString());

Q_SIGNALS:
    void matchActivated(const QString &path, int line, int column, int length);

private:
    void startSearch();
    void stopProcess();
    void readOutput();
    void finished(int exitCode);
    void addMatch(const QJsonObject &data);
    void activateItem(QTreeWidgetItem *item);
    QStringList globs(const QString &text, bool exclude) const;

    QLineEdit *m_query = nullptr;
    QLineEdit *m_include = nullptr;
    QLineEdit *m_exclude = nullptr;
    QToolButton *m_case = nullptr;
    QToolButton *m_word = nullptr;
    QToolButton *m_regex = nullptr;
    QToolButton *m_more = nullptr;
    QWidget *m_filters = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_results = nullptr;

    QProcess *m_proc = nullptr;
    QByteArray m_buffer;
    QString m_root;
    QTimer m_debounce;
    QHash<QString, QTreeWidgetItem *> m_fileItems;
    int m_matchCount = 0;
    int m_maxResults = 5000;
    bool m_truncated = false;
};
