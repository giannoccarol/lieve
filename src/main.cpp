#include "gitgutter.h"
#include "gitpanel.h"
#include "jumplist.h"
#include "mainwindow.h"
#include "quickopen.h"
#include "settings.h"
#include "theme.h"
#include "terminalpanel.h"
#include "updater.h"
#include "version.h"
#include "windowmanager.h"

#include <KActionCollection>
#include <KDBusService>
#include <KLocalizedString>
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
#include <QMenu>
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
// LIEVE_TEST_GUTTER: edits for the git change marks, e.g. "+3:new;~7:changed;-10,2"
// (insert before line 3, replace line 7, delete 2 lines from 10); then lists
// the marks, steps through them like Alt+F3 and clicks one (LIEVE_TEST_GUTTER_CLICK=line).
void testGutter(MainWindow *window, const QString &edits)
{
    auto *gutter = window->findChild<GitGutter *>();
    auto *view = window->findChild<KTextEditor::View *>();
    if (!gutter || !view) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    for (const QString &op : edits.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString arg = op.mid(1).section(QLatin1Char(':'), 0, 0);
        const QString text = op.section(QLatin1Char(':'), 1);
        const int line = arg.section(QLatin1Char(','), 0, 0).toInt() - 1;
        if (op.startsWith(QLatin1Char('+'))) {
            doc->insertLine(line, text);
        } else if (op.startsWith(QLatin1Char('~'))) {
            doc->replaceText(KTextEditor::Range(line, 0, line, doc->lineLength(line)), text);
        } else if (op.startsWith(QLatin1Char('-'))) {
            for (int n = std::max(1, arg.section(QLatin1Char(','), 1, 1).toInt()); n > 0; --n) {
                doc->removeLine(line);
            }
        }
    }
    QElapsedTimer timer;
    timer.start();
    gutter->compare();
    qInfo().noquote() << "gutter-compare-ms" << double(timer.nsecsElapsed()) / 1e6 << "lines" << doc->lines();
    static const char *kinds[] = {"added", "modified", "deleted"};
    for (const GitGutter::Change &c : gutter->changes()) {
        qInfo().noquote() << "gutter-change" << kinds[c.kind] << c.line + 1 << c.count;
    }
    const auto actions = window->actions();
    for (QAction *action : actions) { // found by shortcut: the text depends on the interface language
        if (action->shortcuts().contains(QKeySequence(Qt::ALT | Qt::Key_F3))) {
            for (qsizetype i = 0; i <= gutter->changes().size(); ++i) {
                action->trigger();
                qInfo().noquote() << "gutter-next" << view->cursorPosition().line() + 1;
            }
        }
    }
    const int click = qEnvironmentVariableIntValue("LIEVE_TEST_GUTTER_CLICK");
    if (click > 0) {
        Q_EMIT gutter->clicked(click - 1);
    }
}

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
            for (QAction *action : actions) { // found by shortcut: the text depends on the interface language
                if (action->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_P))) {
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
            for (QAction *action : actions) { // found by shortcut: the text depends on the interface language
                if (action->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G))) {
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
        const QString newShell = qEnvironmentVariable("LIEVE_TEST_NEW_SHELL"); // a second terminal with this shell
        if (!newShell.isEmpty()) {
            QTimer::singleShot(600, window, [window, newShell] {
                if (auto *panel = window->findChild<TerminalPanel *>()) {
                    panel->newTerminal(QString(), newShell);
                }
            });
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_SHELL_MENU")) {
            QTimer::singleShot(900, window, [window] {
                if (auto *button = window->findChild<QToolButton *>(QStringLiteral("shellMenu")); button && button->menu()) {
                    QMenu *menu = button->menu();
                    Q_EMIT menu->aboutToShow(); // fills it
                    const QString file = qEnvironmentVariable("LIEVE_SCREENSHOT");
                    int n = 0;
                    for (QMenu *m : QList<QMenu *>{menu} + menu->findChildren<QMenu *>()) {
                        m->adjustSize();
                        m->grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-menu%1.png").arg(n++)));
                    }
                }
            });
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_HOVER")) { // show the hover look of the window buttons
            for (QToolButton *button : window->findChildren<QToolButton *>()) {
                if (button->objectName() == QLatin1String("closeButton") || button->property("lieveRole").toString() == QLatin1String("minimize")) {
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
        const QString gutterEdits = qEnvironmentVariable("LIEVE_TEST_GUTTER");
        if (!gutterEdits.isEmpty()) {
            const int at = qEnvironmentVariableIntValue("LIEVE_TEST_GUTTER_AT");
            QTimer::singleShot(at > 0 ? at : 1000, window, [window, gutterEdits] {
                testGutter(window, gutterEdits);
            });
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
            qInfo().noquote() << "window" << i << (windows.at(i)->project().isEmpty() ? QStringLiteral("(project chooser)") : windows.at(i)->project());
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

    // Interface language: the system's, unless [ui] language= picks another.
    const QString language = Lieve::uiLanguage();
    const QString languageSetting = Lieve::config().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString().trimmed();
    if (!languageSetting.isEmpty() && languageSetting != QLatin1String("auto")) {
        QLocale::setDefault(QLocale(language));
        KLocalizedString::setLanguages({language}); // the editor and terminal parts too
    }
    QTranslator lieveTranslator;
    if (language != QLatin1String("en") && lieveTranslator.load(QStringLiteral(":/i18n/lieve_%1.qm").arg(language))) {
        QApplication::installTranslator(&lieveTranslator);
    }
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(language), QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
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
