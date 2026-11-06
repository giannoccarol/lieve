#pragma once

#include "git.h"

#include <QFrame>
#include <QPointer>

#include <functional>

class QLineEdit;
class QListWidget;

// The Git commands that ask something, in an overlay like the command
// palette's (same box, rows and filtering): check out or create a branch,
// make and pick stashes, browse the history of the project or of a file.
class GitPicker : public QFrame
{
    Q_OBJECT
public:
    enum class StashAction {
        Apply,
        Pop,
        Drop,
    };

    GitPicker(Lieve::GitRepo *repo, QWidget *parent);
    void checkout();
    void stash(bool includeUntracked); // asks for an optional message
    void pickStash(StashAction action);
    void history(); // of the whole project
    void fileHistory(const QString &file); // absolute path ("" when no file is open: says so)
    // Test hook (GitPanel::runTest): "type:<text>", "down", "up", "enter",
    // "esc", or "rows" to print the rows.
    void testStep(const QString &step);

Q_SIGNALS:
    // A commit to show in a read-only diff tab; `key` finds that tab again.
    void diffReady(const QString &key, const QString &title, const QString &iconName, const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Row {
        QString title;
        QString detail; // dim, after the title
        QString note; // dim, at the right ("current", a hash)
        QString section; // first row of a group: a line above, this label at the right (not while filtering)
        QString extra; // also matched, not shown (hash, author)
        bool always = false; // shown whatever is typed ("Create new branch…")
        std::function<void()> run;
    };
    // Shows the box with an empty list; the rows follow with setRows().
    void open(const QString &placeholder);
    void setRows(const QList<Row> &rows, const QString &empty);
    // A line of text instead of a list. `check` gives the line shown under
    // it and whether Enter is accepted.
    void ask(const QString &placeholder,
             const QString &text,
             const std::function<QString(const QString &, bool *)> &check,
             const std::function<void(const QString &)> &done);
    void askBranchName(const std::function<void(const QString &)> &done);
    QList<Row> branchRows(const std::function<void(const Lieve::GitBranch &)> &chosen) const;
    void switchTo(const Lieve::GitBranch &branch);
    void showLog(const QString &relative); // "" = the whole project
    void showCommit(const Lieve::GitCommit &commit, const QString &file);
    void refilter();
    void addInfo(const QString &text);
    void accept();
    void finish(const std::function<void()> &action); // closes the box, then runs it
    void dismiss();
    void place();

    Lieve::GitRepo *m_repo;
    QLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QList<Row> m_rows;
    QString m_empty; // shown when there are no rows ("" while loading)
    bool m_asking = false;
    std::function<QString(const QString &, bool *)> m_check;
    std::function<void(const QString &)> m_answer;
    QString m_typed; // the text when a row was chosen
    quint64 m_request = 0; // bumped at each step: late answers for an earlier one are dropped
    QList<Lieve::GitBranch> m_branches;
    QPointer<QWidget> m_returnFocus;
};
