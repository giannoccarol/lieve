#include "delegates.h"
#include "gitpanel.h"
#include "jumplist.h"
#include "mainwindow.h"
#include "quickopen.h"
#include "searchpanel.h"
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
#include <QHoverEvent>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <memory>

namespace
{
// Replace in files: LIEVE_TEST_SEARCH=<text> (or, if empty, Ctrl+Shift+H with
// the first word of the editor selected); LIEVE_TEST_REPLACE=<text> fills
// the Replace field; LIEVE_TEST_FLAGS=<c, w, r> turns on Match Case, Whole
// Word, Regular Expression; LIEVE_TEST_HOVER_ROW=<n> hovers the n-th result
// row; LIEVE_TEST_CLICK_ROW=<n>:<k> clicks its k-th hover button;
// LIEVE_TEST_REPLACE_ALL runs Replace All. Dialogs are answered (and saved as
// <screenshot>-dialogN.png), then the open documents are printed;
// LIEVE_TEST_UNDO undoes once in each and prints them again.
void testReplace(MainWindow *window, const QString &search)
{
    auto *panel = window->findChild<SearchPanel *>();
    const QString flags = qEnvironmentVariable("LIEVE_TEST_FLAGS");
    for (QToolButton *button : panel->findChildren<QToolButton *>()) {
        const QString text = button->text();
        if ((text == QLatin1String("Aa") && flags.contains(QLatin1Char('c'))) || (text == QLatin1String("ab") && flags.contains(QLatin1Char('w')))
            || (text == QLatin1String(".*") && flags.contains(QLatin1Char('r')))) {
            button->setChecked(true);
        }
    }
    if (!search.isEmpty()) {
        window->showSearch(search, true);
    } else if (auto *view = window->findChild<KTextEditor::View *>()) {
        view->setSelection(view->document()->wordRangeAt(KTextEditor::Cursor(0, 0)));
        const auto actions = window->actions();
        for (QAction *action : actions) { // found by shortcut: the text depends on the interface language
            if (action->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H))) {
                action->trigger();
            }
        }
    }
    if (auto *edit = window->findChild<QLineEdit *>(QStringLiteral("replaceInput")); edit && !qEnvironmentVariable("LIEVE_TEST_REPLACE").isEmpty()) {
        edit->setText(qEnvironmentVariable("LIEVE_TEST_REPLACE"));
    }
    auto *answer = new QTimer(window);
    answer->setInterval(250);
    QObject::connect(answer, &QTimer::timeout, window, [n = 0]() mutable {
        const auto widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->isVisible()) {
                continue;
            }
            qInfo().noquote() << "dialog" << box->text() << "|" << box->informativeText();
            box->grab().save(qEnvironmentVariable("LIEVE_SCREENSHOT").replace(QStringLiteral(".png"), QStringLiteral("-dialog%1.png").arg(n++)));
            QAbstractButton *accept = box->button(QMessageBox::Ok);
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::AcceptRole) {
                    accept = button;
                }
            }
            if (accept) {
                accept->click();
            }
        }
    });
    answer->start();
    const auto dump = [window](const char *label) {
        for (KTextEditor::View *view : window->findChildren<KTextEditor::View *>()) {
            KTextEditor::Document *doc = view->document();
            qInfo().noquote() << label << doc->url().toLocalFile() << "modified" << doc->isModified()
                              << QString(doc->text()).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        }
    };
    QTimer::singleShot(1500, window, [window, dump] {
        auto *tree = window->findChild<SearchPanel *>()->findChild<QTreeWidget *>();
        const int hover = qEnvironmentVariableIsSet("LIEVE_TEST_HOVER_ROW") ? qEnvironmentVariableIntValue("LIEVE_TEST_HOVER_ROW") : -1;
        const QStringList click = qEnvironmentVariable("LIEVE_TEST_CLICK_ROW").split(QLatin1Char(':'));
        const int clickRow = click.size() == 2 ? click.at(0).toInt() : -1;
        QList<QPoint> clicks;
        int row = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it, ++row) {
            const QRect rect = tree->visualItemRect(*it);
            if (row == hover) {
                const QPointF pos = rect.center();
                QHoverEvent event(QEvent::HoverMove, pos, tree->viewport()->mapToGlobal(pos), QPointF(-1, -1));
                QApplication::sendEvent(tree->viewport(), &event);
            }
            if (row == clickRow) { // buttons: 22 px apart, from the right end (left of the count badge); k = 0 is the last
                int right = rect.right() - 4;
                if (const int count = (*it)->data(0, Lieve::CountRole).toInt(); count > 0) {
                    right -= QFontMetrics(tree->font()).horizontalAdvance(QString::number(count)) + 12 + 6;
                }
                clicks << QPoint(right - click.at(1).toInt() * 22 - 12, rect.center().y());
            }
        }
        for (const QPoint &pos : std::as_const(clicks)) {
            for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, pos, tree->viewport()->mapToGlobal(pos), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &event);
            }
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_REPLACE_ALL")) {
            if (auto *button = window->findChild<QToolButton *>(QStringLiteral("replaceAllButton"))) {
                QElapsedTimer timer;
                timer.start();
                button->click();
                qInfo().noquote() << "replace-all-ms" << timer.elapsed() << "(dialogs included)";
            }
        }
        QTimer::singleShot(300, window, [window, dump] { // after the clicked actions
            qInfo().noquote() << "status" << window->findChild<QLabel *>(QStringLiteral("searchStatus"))->text();
            dump("replaced");
            if (qEnvironmentVariableIsSet("LIEVE_TEST_UNDO")) {
                for (KTextEditor::View *view : window->findChildren<KTextEditor::View *>()) {
                    view->actionCollection()->action(QStringLiteral("edit_undo"))->trigger();
                }
                dump("undone");
            }
        });
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
        if (qEnvironmentVariableIsSet("LIEVE_TEST_REPLACE")) {
            testReplace(window, search);
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
