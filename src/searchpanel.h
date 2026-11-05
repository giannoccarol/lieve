#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QTimer>
#include <QWidget>

#include <functional>

class QJsonObject;
class QLabel;
class QLineEdit;
class QModelIndex;
class QProcess;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace KTextEditor
{
class Document;
}

// "Find in files" backed by ripgrep (respects .gitignore, very fast), with
// Replace like VS Code's.
class SearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanel(QWidget *parent = nullptr);
    void setRoot(const QString &dir);
    // replace: also shows the Replace field, focused when `text` fills the query.
    void focusQuery(const QString &text = QString(), bool replace = false);
    // The document of a file open in the editor (canonical path), if any:
    // replacing changes it there, without saving.
    void setDocumentProvider(const std::function<KTextEditor::Document *(const QString &path)> &provider);

Q_SIGNALS:
    void matchActivated(const QString &path, int line, int column, int length);
    void filesWritten(); // a replace rewrote files on disk

private:
    struct Outcome {
        int occurrences = 0;
        int files = 0;
        int skipped = 0;
        QStringList problems;
    };

    void startSearch();
    void stopProcess();
    void readOutput();
    void finished(int exitCode);
    void addMatch(const QJsonObject &data);
    void activateItem(QTreeWidgetItem *item);
    QStringList globs(const QString &text, bool exclude) const;
    void updateStatus();

    // Replace
    void setReplaceVisible(bool visible);
    bool replacing() const;
    bool canReplace() const;
    void updateReplaceAll();
    QStringList preview(const QModelIndex &index) const;
    bool replacementsFor(const QString &line, const QVariantList &ranges, QStringList *out) const;
    void runRowAction(QTreeWidgetItem *item, int action);
    void replaceAll();
    Outcome replaceRows(const QList<QTreeWidgetItem *> &rows);
    QList<QTreeWidgetItem *> replaceInFile(const QString &path, const QList<QTreeWidgetItem *> &rows, QString *problem, bool *written);
    void removeRow(QTreeWidgetItem *row);
    void removeFile(QTreeWidgetItem *file);
    void reportProblems(const Outcome &outcome);

    QLineEdit *m_query = nullptr;
    QLineEdit *m_replace = nullptr;
    QLineEdit *m_include = nullptr;
    QLineEdit *m_exclude = nullptr;
    QToolButton *m_toggleReplace = nullptr;
    QToolButton *m_replaceAll = nullptr;
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

    // The search behind the results, in Qt's syntax: matches are found again
    // with it when replacing ($1 needs the groups; ripgrep only gives offsets).
    bool m_searchedRegex = false;
    QRegularExpression m_matcher;
    std::function<KTextEditor::Document *(const QString &path)> m_documentFor;
};
