#include "gitpanel.h"
#include "jumplist.h"
#include "mainwindow.h"
#include "quickopen.h"
#include "settings.h"
#include "theme.h"
#include "updater.h"
#include "version.h"
#include "windowmanager.h"

#include <KActionCollection>
#include <KDBusService>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QElapsedTimer>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <memory>

namespace
{
// Used by the automated checks (LIEVE_SCREENSHOT=file.png): render the first
// window to a PNG and quit. Never set in normal use.
void setupTestHooks(QApplication &app)
{
    const QString screenshot = qEnvironmentVariable("LIEVE_SCREENSHOT");
    if (screenshot.isEmpty()) {
        return;
    }
    QTimer::singleShot(200, &app, [] {
        MainWindow *window = WindowManager::instance().windows().value(0);
        if (!window) {
            return;
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_TERMINAL")) {
            window->setTerminalVisible(true);
        }
        const QString search = qEnvironmentVariable("LIEVE_TEST_SEARCH");
        if (!search.isEmpty()) {
            window->showSearch(search);
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_SWITCHER")) {
            window->showProjectSwitcher();
        }
        const QString quickOpen = qEnvironmentVariable("LIEVE_TEST_QUICKOPEN");
        if (!quickOpen.isEmpty()) {
            const auto actions = window->actions();
            for (QAction *action : actions) {
                if (action->text() == QLatin1String("Apri file rapidamente")) {
                    action->trigger();
                }
            }
            QTimer::singleShot(700, window, [window, quickOpen] {
                if (auto *edit = window->findChild<QuickOpen *>()->findChild<QLineEdit *>()) {
                    edit->setText(quickOpen);
                }
            });
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_GIT")) {
            const auto actions = window->actions();
            for (QAction *action : actions) {
                if (action->text() == QLatin1String("Controllo del codice sorgente")) {
                    action->trigger();
                }
            }
            QTimer::singleShot(700, window, [window] {
                auto *panel = window->findChild<GitPanel *>();
                auto *tree = panel ? panel->findChild<QTreeWidget *>() : nullptr;
                const QString commit = qEnvironmentVariable("LIEVE_TEST_COMMIT");
                if (!commit.isEmpty() || qEnvironmentVariableIsSet("LIEVE_TEST_CLICK")) {
                    panel->findChild<QPlainTextEdit *>()->setPlainText(commit);
                    for (QPushButton *button : panel->findChildren<QPushButton *>()) {
                        if (button->property("mode").isValid()) {
                            qInfo().noquote() << "click" << button->text();
                            button->click();
                        }
                    }
                } else if (tree && tree->topLevelItemCount() > 0) {
                    // open the diff of the first unstaged change
                    QTreeWidgetItem *section = tree->topLevelItem(tree->topLevelItemCount() - 1);
                    if (section->childCount() > 0) {
                        const int which = qEnvironmentVariableIntValue("LIEVE_TEST_GIT_ITEM");
                        Q_EMIT tree->itemClicked(section->child(std::min(which, section->childCount() - 1)), 0);
                    }
                }
            });
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_HOVER")) { // show the hover look of the window buttons
            for (QToolButton *button : window->findChildren<QToolButton *>()) {
                if (button->objectName() == QLatin1String("closeButton") || button->toolTip() == QLatin1String("Riduci a icona")) {
                    button->setAttribute(Qt::WA_UnderMouse, true);
                    button->update();
                }
            }
        }
        const int saveAt = qEnvironmentVariableIntValue("LIEVE_TEST_SAVE");
        if (saveAt > 0) {
            if (auto *view = window->findChild<KTextEditor::View *>()) {
                view->document()->insertText(KTextEditor::Cursor(0, 0), QStringLiteral(" "));
                QTimer::singleShot(saveAt, window, [view] {
                    QElapsedTimer timer;
                    timer.start();
                    view->document()->documentSave();
                    qInfo().noquote() << "save-ms" << timer.elapsed() << "cursor" << view->cursorPosition().line() << view->cursorPosition().column();
                });
            }
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_KEYS")) {
            if (auto *view = window->findChild<KTextEditor::View *>()) {
                const auto actions = view->actionCollection()->actions();
                for (QAction *action : actions) {
                    if (!action->shortcuts().isEmpty()) {
                        qInfo().noquote() << "editor-key" << action->objectName() << QKeySequence::listToString(action->shortcuts());
                    }
                }
            }
        }
    });
    const int delay = qEnvironmentVariableIntValue("LIEVE_SCREENSHOT_DELAY");
    QTimer::singleShot(delay > 0 ? delay : 2500, &app, [screenshot] {
        const auto windows = WindowManager::instance().windows();
        qInfo().noquote() << "windows" << windows.size();
        for (qsizetype i = 0; i < windows.size(); ++i) {
            qInfo().noquote() << "window" << i << (windows.at(i)->project().isEmpty() ? QStringLiteral("(scelta progetto)") : windows.at(i)->project());
            QString file = screenshot;
            if (i > 0) {
                file.replace(QStringLiteral(".png"), QStringLiteral("-%1.png").arg(i));
            }
            windows.at(i)->grab().save(file);
        }
        QApplication::exit(0);
    });
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("lieve"));
    QApplication::setApplicationDisplayName(QStringLiteral("Lieve"));
    QApplication::setApplicationVersion(QStringLiteral(LIEVE_VERSION));
    QApplication::setOrganizationDomain(QStringLiteral("local"));
    QApplication::setDesktopFileName(QStringLiteral("lieve"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/lieve.svg")));

    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslator);
    }

    QCommandLineParser parser;
    Lieve::setupCommandLine(parser);
    parser.process(app);

    // One process for all windows: a second "lieve" hands its arguments to
    // the running one (via D-Bus) and exits immediately.
    std::unique_ptr<KDBusService> service;
    const bool testRun = !qEnvironmentVariableIsEmpty("LIEVE_SCREENSHOT") && !qEnvironmentVariableIsSet("LIEVE_TEST_UNIQUE");
    const bool separate = parser.isSet(QStringLiteral("new-instance")) || testRun;
    if (!separate) {
        service = std::make_unique<KDBusService>(KDBusService::Unique | KDBusService::NoExitOnFailure);
        QObject::connect(service.get(), &KDBusService::activateRequested, &app, [](const QStringList &arguments, const QString &workingDirectory) {
            WindowManager::instance().handleCommandLine(arguments, workingDirectory);
        });
    }

    Lieve::ensureConfigFile();
    Lieve::Theme::apply(app);
    WindowManager::instance().handleCommandLine(QCoreApplication::arguments(), QDir::currentPath());
    Lieve::JumpList::update();
    Updater::instance().start();
    setupTestHooks(app);

    const int result = QApplication::exec();
    WindowManager::instance().deleteAll();
    return result;
}
