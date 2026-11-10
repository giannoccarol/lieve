#pragma once

#include "git.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>

class GitPicker;
class QAction;
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
    // "Checkout to…" and the history, in a picker over the window.
    void showCheckout();
    void showHistory(const QString &file); // absolute path, "" = the whole project
    // The file of "View File History" (the current editor's).
    void setFileSource(const std::function<QString()> &source);
    // Commands that work without the panel: the window lists them in the
    // command palette as they are (the panel's menus show the panel first).
    QList<QAction *> windowActions() const
    {
        return m_windowActions;
    }
    // Test hook: steps separated by ';', see the definition.
    void runTest(const QString &steps);

Q_SIGNALS:
    void diffRequested(const Lieve::GitEntry &entry);
    void openFileRequested(const QString &path);
    // A commit from the history, for a read-only diff tab.
    void commitDiffRequested(const QString &key, const QString &title, const QString &iconName, const QString &text);

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
    // The rows an action on `item` applies to: every selected file when it is
    // one of them (as in VS Code), else just `item`.
    QList<QTreeWidgetItem *> targetsOf(QTreeWidgetItem *item) const;
    bool confirmDiscard(const QList<Lieve::GitEntry> &entries);
    GitPicker *picker(); // created on first use

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
    Qt::KeyboardModifiers m_pressModifiers; // of the last press on the list: Ctrl / Shift only select
    QString m_anchor; // the file a Shift+click selects from (group:path)
    QSet<int> m_collapsed; // collapsed sections, by group
    QPointer<GitPicker> m_picker;
    QList<QAction *> m_windowActions;
    std::function<QString()> m_fileSource;
};
