#include "gitpanel.h"
#include "jumplist.h"
#include "mainwindow.h"
#include "quickopen.h"
#include "settings.h"
#include "tabview.h"
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
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QIcon>
#include <QElapsedTimer>
#include <QFile>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QLocale>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>

#include <memory>

namespace
{
// LIEVE_TEST_HOTEXIT=step,step...: in every window "type" edits the current
// document, "untitled" adds a new one with text, "prevtab" goes to the
// previous tab, "save" saves the current one, "closetab" closes it; "cancel"
// or "discard" answer the save questions. Then after LIEVE_TEST_HOTEXIT_DELAY
// ms "close" (the windows), "closeproject", "quit" (Ctrl+Q) or "restart" (the
// updater's). "report" writes each window's tabs to the screenshot's .txt.
void hotExitTest(const QString &screenshot)
{
    const QStringList steps = qEnvironmentVariable("LIEVE_TEST_HOTEXIT").split(QLatin1Char(','), Qt::SkipEmptyParts);
    const auto has = [steps](const char *step) {
        return steps.contains(QLatin1String(step));
    };
    const auto trigger = [](MainWindow *window, const QKeySequence &key) {
        const auto actions = window->actions();
        for (QAction *action : actions) { // found by shortcut: the text depends on the interface language
            if (action->shortcuts().contains(key)) {
                action->trigger();
                return;
            }
        }
    };
    const auto editorTabs = [](MainWindow *window) -> TabView * { // not the terminal's
        const auto all = window->findChildren<TabView *>();
        for (TabView *tabView : all) {
            if (tabView->parentWidget() && tabView->parentWidget()->objectName() == QLatin1String("editorCard")) {
                return tabView;
            }
        }
        return nullptr;
    };
    const auto tabs = [editorTabs](MainWindow *window) {
        QList<KTextEditor::View *> views;
        if (TabView *tabView = editorTabs(window)) {
            for (int i = 0; i < tabView->count(); ++i) {
                if (auto *view = qobject_cast<KTextEditor::View *>(tabView->widget(i))) {
                    views << view;
                }
            }
        }
        return views;
    };
    const auto current = [editorTabs](MainWindow *window) {
        TabView *tabView = editorTabs(window);
        return tabView ? qobject_cast<KTextEditor::View *>(tabView->currentWidget()) : nullptr;
    };
    const auto windows = WindowManager::instance().windows();
    for (MainWindow *window : windows) {
        if (KTextEditor::View *view = current(window); view && has("type")) {
            view->document()->insertText(KTextEditor::Cursor(0, 0), QStringLiteral("// unsaved edit\n"));
            view->setCursorPosition(KTextEditor::Cursor(1, 3));
        }
        if (has("untitled")) {
            trigger(window, QKeySequence(Qt::CTRL | Qt::Key_N));
            if (KTextEditor::View *view = current(window)) {
                view->document()->setText(QStringLiteral("scratch notes\nsecond line"));
            }
        }
        if (has("prevtab")) {
            trigger(window, QKeySequence(Qt::CTRL | Qt::Key_PageUp));
        }
        if (KTextEditor::View *view = current(window); view && has("save")) {
            view->document()->documentSave();
        }
    }
    if (has("cancel") || has("discard")) { // answers the save questions: saves the first one as a PNG
        auto *answer = new QTimer(qApp);
        QObject::connect(answer, &QTimer::timeout, qApp, [discard = has("discard"), screenshot] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            const QString file = QString(screenshot).replace(QStringLiteral(".png"), QStringLiteral("-dialog.png"));
            if (!QFile::exists(file)) {
                dialog->grab().save(file);
            }
            qInfo().noquote() << "dialog" << dialog->windowTitle() << (discard ? "-> discard" : "-> cancel");
            auto *box = dialog->findChild<QDialogButtonBox *>();
            if (discard && box && box->button(QDialogButtonBox::No)) {
                box->button(QDialogButtonBox::No)->click();
            } else {
                dialog->reject();
            }
        });
        answer->start(400);
    }
    if (has("closetab")) {
        for (MainWindow *window : windows) {
            const qsizetype before = tabs(window).size();
            trigger(window, QKeySequence(Qt::CTRL | Qt::Key_W));
            qInfo().noquote() << "tabs" << before << "->" << tabs(window).size();
        }
    }
    const int delay = qEnvironmentVariableIntValue("LIEVE_TEST_HOTEXIT_DELAY");
    QTimer::singleShot(delay > 0 ? delay : 500, qApp, [has] {
        qputenv("LIEVE_TEST_HOTEXIT", "report"); // what a restarted Lieve does
        const auto windows = WindowManager::instance().windows();
        if (has("close")) {
            for (MainWindow *window : windows) {
                qInfo().noquote() << "close" << window->close();
            }
        } else if (has("closeproject")) {
            for (MainWindow *window : windows) {
                qInfo().noquote() << "closeProject" << window->closeProject();
            }
        } else if (has("quit")) {
            qInfo().noquote() << "closeAll" << WindowManager::instance().closeAll();
        } else if (has("restart")) {
            Updater::instance().restart();
        }
    });
    const int shotDelay = qEnvironmentVariableIntValue("LIEVE_SCREENSHOT_DELAY");
    QTimer::singleShot(std::max(0, (shotDelay > 0 ? shotDelay : 2500) - 1000), qApp, [has, tabs, current, screenshot] { // this runs 300+ ms in
        if (!has("report")) {
            return;
        }
        QFile out(QString(screenshot).replace(QStringLiteral(".png"), QStringLiteral(".txt")));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return;
        }
        QTextStream text(&out);
        const auto windows = WindowManager::instance().windows();
        for (MainWindow *window : windows) {
            text << "window " << window->project() << "\n";
            for (KTextEditor::View *view : tabs(window)) {
                KTextEditor::Document *doc = view->document();
                text << (view == current(window) ? "* " : "  ") << doc->documentName() << (doc->isModified() ? " [modified]" : "")
                     << (doc->isReadWrite() ? "" : " [read-only]")
                     << " cursor " << view->cursorPosition().line() << ":" << view->cursorPosition().column() << " path " << doc->url().toLocalFile()
                     << " first-line " << doc->line(0) << "\n";
            }
        }
    });
}

// Used by the automated checks (LIEVE_SCREENSHOT=file.png): render the first
// window to a PNG and quit. Never set in normal use.
void setupTestHooks(QApplication &app)
{
    const QString screenshot = qEnvironmentVariable("LIEVE_SCREENSHOT");
    if (screenshot.isEmpty()) {
        return;
    }
    if (qEnvironmentVariableIsSet("LIEVE_TEST_HOTEXIT")) {
        QTimer::singleShot(300, &app, [screenshot] {
            hotExitTest(screenshot);
        });
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
