#pragma once

#include "git.h"

#include <QSet>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Source control side page, modelled on VS Code's: commit message, Commit
// button, "Staged changes" / "Changes" sections with hover actions.
class GitPanel : public QWidget
{
    Q_OBJECT
public:
    enum Action {
        OpenFile,
        Discard,
        Stage,
        Unstage,
    };

    explicit GitPanel(Lieve::GitRepo *repo, QWidget *parent = nullptr);
    void focusMessage();

Q_SIGNALS:
    void diffRequested(const Lieve::GitEntry &entry);
    void openFileRequested(const QString &path);

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();
    void updateCommitButton();
    void adjustMessageHeight();
    void primaryAction();
    void commit(Lieve::GitRepo::After after, bool amend);
    void runAction(QTreeWidgetItem *item, Action action);
    void activate(QTreeWidgetItem *item);
    void showMenu(const QPoint &pos);
    QList<Lieve::GitEntry> entriesOf(QTreeWidgetItem *item) const;
    bool confirmDiscard(const QList<Lieve::GitEntry> &entries);

    Lieve::GitRepo *m_repo;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_noRepo = nullptr;
    QPushButton *m_init = nullptr;
    QLabel *m_branch = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_message = nullptr;
    QPushButton *m_commit = nullptr;
    QToolButton *m_commitMenu = nullptr;
    QTreeWidget *m_tree = nullptr;
    QSet<int> m_collapsed; // collapsed sections, by group
};
