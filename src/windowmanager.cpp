#include "windowmanager.h"

#include "jumplist.h"
#include "mainwindow.h"
#include "projects.h"

#include <KWindowSystem>

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QWindow>

namespace Lieve
{
void setupCommandLine(QCommandLineParser &parser)
{
    parser.setApplicationDescription(QCoreApplication::translate("CommandLine", "Lieve: a lightweight code editor (explorer, search, terminal)."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(QStringLiteral("new-window"), QCoreApplication::translate("CommandLine", "Show the project chooser in a window.")));
    parser.addOption(QCommandLineOption(QStringLiteral("open-folder"), QCoreApplication::translate("CommandLine", "Ask which folder to open.")));
    parser.addOption(QCommandLineOption(QStringLiteral("new-instance"),
                                        QCoreApplication::translate("CommandLine", "Start a separate process instead of using the one already running.")));
    // The name and syntax of the positional argument only appear in --help.
    parser.addPositionalArgument(QCoreApplication::translate("CommandLine", "paths"),
                                 QCoreApplication::translate("CommandLine", "Folders or files to open. Use file:line to go straight to a line."),
                                 QCoreApplication::translate("CommandLine", "[paths...]"));
}
}

WindowManager &WindowManager::instance()
{
    static auto *manager = new WindowManager(qApp);
    return *manager;
}

MainWindow *WindowManager::createWindow()
{
    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    m_windows << window;
    connect(window, &QObject::destroyed, this, [this] {
        m_windows.removeIf([](const QPointer<MainWindow> &w) {
            return w.isNull();
        });
        Q_EMIT projectsChanged();
    });
    connect(window, &MainWindow::projectChanged, this, &WindowManager::projectsChanged);
    return window;
}

QList<MainWindow *> WindowManager::windows() const
{
    QList<MainWindow *> out;
    for (const QPointer<MainWindow> &w : m_windows) {
        if (w) {
            out << w;
        }
    }
    return out;
}

MainWindow *WindowManager::windowFor(const QString &project) const
{
    for (MainWindow *w : windows()) {
        if (w->project() == project) {
            return w;
        }
    }
    return nullptr;
}

QSet<QString> WindowManager::openProjects() const
{
    QSet<QString> open;
    for (MainWindow *w : windows()) {
        if (!w->project().isEmpty()) {
            open.insert(w->project());
        }
    }
    return open;
}

void WindowManager::activate(MainWindow *window)
{
    if (window->isMinimized()) {
        window->showNormal();
    } else {
        window->show();
    }
    // Wayland needs the activation token passed on by a second "lieve" launch.
    KWindowSystem::updateStartupId(window->windowHandle());
    window->raise();
    KWindowSystem::activateWindow(window->windowHandle());
#ifdef Q_OS_WIN
    // KWindowSystem does nothing here on Windows. Windows lets the window come
    // to the front when the second "lieve" launch allowed it
    // (Platform::allowForeground), otherwise it flashes in the taskbar.
    window->activateWindow();
#endif
}

MainWindow *WindowManager::openProject(const QString &path, MainWindow *reuse)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        QMessageBox::warning(reuse, QStringLiteral("Lieve"), tr("The folder no longer exists:\n%1").arg(path));
        return nullptr;
    }
    if (MainWindow *existing = windowFor(canonical)) {
        activate(existing);
        return existing;
    }
    MainWindow *target = (reuse && reuse->project().isEmpty()) ? reuse : createWindow();
    target->setProject(canonical);
    Lieve::Projects::touch(canonical);
    Lieve::JumpList::update();
    activate(target);
    Q_EMIT projectsChanged();
    return target;
}

void WindowManager::showChooser()
{
    for (MainWindow *w : windows()) {
        if (w->project().isEmpty()) {
            activate(w);
            w->focusChooser();
            return;
        }
    }
    MainWindow *window = createWindow();
    activate(window);
    window->focusChooser();
}

void WindowManager::openFolderDialog(MainWindow *parent)
{
    const QString start = (parent && !parent->project().isEmpty()) ? QFileInfo(parent->project()).absolutePath() : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(parent, tr("Open Folder"), start);
    if (!dir.isEmpty()) {
        openProject(dir, parent);
    } else if (windows().isEmpty()) {
        showChooser(); // started with --open-folder and cancelled: never leave Lieve without a window
    }
}

void WindowManager::handleCommandLine(const QStringList &arguments, const QString &workingDirectory)
{
    QCommandLineParser parser;
    Lieve::setupCommandLine(parser);
    parser.parse(arguments);
    const QDir cwd(workingDirectory.isEmpty() ? QDir::currentPath() : workingDirectory);

    if (parser.isSet(QStringLiteral("open-folder"))) {
        openFolderDialog(nullptr);
        return;
    }

    const QStringList paths = parser.positionalArguments();
    for (QString arg : paths) {
        arg = cwd.absoluteFilePath(arg);
        int line = -1;
        static const QRegularExpression lineSuffix(QStringLiteral("^(.*):(\\d+)$"));
        if (const auto match = lineSuffix.match(arg); match.hasMatch() && !QFileInfo::exists(arg) && QFileInfo(match.captured(1)).isFile()) {
            arg = match.captured(1);
            line = match.captured(2).toInt() - 1;
        }
        const QFileInfo info(arg);
        if (info.isDir()) {
            openProject(info.absoluteFilePath());
        } else if (info.isFile()) {
            if (MainWindow *w = openProject(Lieve::projectRootFor(info.absolutePath()))) {
                w->openFile(info.absoluteFilePath(), line);
            }
        }
    }
    if (paths.isEmpty() || parser.isSet(QStringLiteral("new-window")) || windows().isEmpty()) {
        showChooser();
    }
}

bool WindowManager::closeAll()
{
    for (MainWindow *w : windows()) {
        if (!w->close()) {
            return false; // the user cancelled on an unsaved file
        }
    }
    return true;
}

void WindowManager::deleteAll()
{
    for (MainWindow *w : windows()) {
        delete w;
    }
    m_windows.clear();
}
