#pragma once

#include "commandpalette.h"
#include "npmpackages.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class NpmScriptsView;
class QAction;
class QFileSystemWatcher;
class QSplitter;

// The NPM SCRIPTS section at the bottom of the Explorer, like VS Code's: the
// scripts of the project's package.json files, each run in a terminal with a
// click. The files are read when the project opens and when the Explorer is
// shown, then watched; the section is made only for projects with scripts.
class NpmScripts : public QObject
{
    Q_OBJECT
public:
    // Category of its commands in the palette ("Tasks: Run npm Script…").
    static constexpr const char *Category = QT_TRANSLATE_NOOP("CommandPalette", "Tasks");

    // The section goes into `explorer`'s layout, below `files` (the tree).
    NpmScripts(QWidget *explorer, QWidget *files);
    void setRoot(const QString &root);

    // Opens a terminal in a folder, titled, typing a command (or runs it again
    // in the terminal given, if idle); returns that terminal.
    using Runner = std::function<QWidget *(const QString &dir, const QString &title, const QString &command, QWidget *reuse)>;
    void setRunner(const Runner &runner);
    // Kept enabled only while they have something to run, so the command
    // palette lists them only then.
    void bindActions(QAction *run, QAction *rerun);

    // The project's scripts for the palette's picker, each with the action that runs it.
    QList<Lieve::CommandChoice> choices();
    void run(const QString &dir, const QString &script, bool again = false);
    void rerunLast();

    // Automated checks (LIEVE_TEST_NPM in main.cpp): steps separated by ",",
    // see the end of npmscripts.cpp.
    void runTest(const QString &steps);

Q_SIGNALS:
    void openRequested(const QString &path, int line, int column, int length);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class NpmScriptsView;
    void scan();
    bool hasScripts() const;
    void updateActions();
    void ensureView();
    void setCollapsed(bool collapsed);
    void fit();
    void testStep(const QString &step);

    QWidget *m_explorer = nullptr;
    QWidget *m_files = nullptr;
    QString m_root;
    bool m_scanned = false;
    QList<Lieve::Npm::Package> m_packages;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer m_rescan;
    QSplitter *m_split = nullptr;
    NpmScriptsView *m_view = nullptr;
    bool m_collapsed = false;
    int m_userHeight = 0; // the user dragged the line above the section to this
    bool m_fitPending = false; // sized once the splitter has its size
    Runner m_runner;
    QPointer<QAction> m_runAction;
    QPointer<QAction> m_rerunAction;
    QObject *m_choiceActions = nullptr;
    struct LastRun {
        QString dir;
        QString script;
        QPointer<QWidget> terminal;
    };
    LastRun m_last;
};
