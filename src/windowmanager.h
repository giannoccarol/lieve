#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

class MainWindow;
class QCommandLineParser;

namespace Lieve
{
void setupCommandLine(QCommandLineParser &parser);
}

// All windows live in one process (shared editor, Qt and KDE libraries, so each
// extra project costs only a few MB). One window per project: opening a project
// that is already open brings its window to the front.
class WindowManager : public QObject
{
    Q_OBJECT
public:
    static WindowManager &instance();

    // Opens `path` in `reuse` if that window has no project, otherwise in a
    // new window. If the project is already open, activates its window.
    MainWindow *openProject(const QString &path, MainWindow *reuse = nullptr);
    MainWindow *windowFor(const QString &project) const;
    QSet<QString> openProjects() const;
    QList<MainWindow *> windows() const;

    // Shows the project chooser: an empty window if there is one, else a new one.
    void showChooser();
    void openFolderDialog(MainWindow *parent);
    // Arguments as received by main() or by a second "lieve" launch (D-Bus).
    void handleCommandLine(const QStringList &arguments, const QString &workingDirectory);
    bool closeAll();
    void deleteAll();
    static void activate(MainWindow *window);

Q_SIGNALS:
    void projectsChanged(); // a window opened/closed a project

private:
    using QObject::QObject;
    MainWindow *createWindow();

    QList<QPointer<MainWindow>> m_windows;
};
