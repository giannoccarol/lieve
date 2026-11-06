#include "delegates.h"
#include "explorer.h"
#include "gitgutter.h"
#include "gitpanel.h"
#include "jumplist.h"
#include "lsp.h"
#include "mainwindow.h"
#include "mergeconflicts.h"
#include "problemspanel.h"
#include "quickopen.h"
#include "searchpanel.h"
#include "settings.h"
#include "statusbar.h"
#include "tabview.h"
#include "settingsdialog.h"
#include "theme.h"
#include "terminalpanel.h"
#include "updater.h"
#include "version.h"
#include "windowmanager.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KDBusService>
#include <KSharedConfig>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QIcon>
#include <QElapsedTimer>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QFile>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QLocale>
#include <QTextStream>
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

// LIEVE_TEST_PROBLEMS=step;step...: the Problems panel, one step every 400 ms
// from LIEVE_TEST_PROBLEMS_AT ms (default 1000). "open:<file>" opens a file
// (relative to the project), "wait:<ms>", "key:<keys>" runs the window
// shortcut with those keys (key:Ctrl+Shift+M), "status" clicks the problems
// item of the status bar, "rows" prints the panel's rows, "select:<n>" makes
// the n-th visible row current, "hover:<n>" hovers it, "click:<n>" clicks it,
// "enter" presses Enter on the current row, "closetab:<n>" clicks the × of
// the n-th tab of the bottom panel, "where" prints the editor's file and
// cursor and the bottom panel's tabs. Problems without a language server:
// LIEVE_TEST_LSP=fake:<severity>:<line>:<column>:<message> (see LspManager::runTest()).
void testProblems(MainWindow *window, const QStringList &steps)
{
    if (steps.isEmpty()) {
        return;
    }
    const QString step = steps.first();
    const QString arg = step.section(QLatin1Char(':'), 1);
    int delay = 400;
    qInfo().noquote() << "problems-test step" << step;
    auto *panel = window->findChild<ProblemsPanel *>();
    auto *tree = panel ? panel->findChild<QTreeWidget *>() : nullptr;
    const auto row = [tree](int n) {
        QModelIndex index = tree ? tree->model()->index(0, 0) : QModelIndex();
        for (int i = 0; i < n && index.isValid(); ++i) {
            index = tree->indexBelow(index);
        }
        return index;
    };
    if (step.startsWith(QLatin1String("open:"))) {
        window->openFile(QDir(window->project()).absoluteFilePath(arg));
    } else if (step.startsWith(QLatin1String("wait:"))) {
        delay = arg.toInt();
    } else if (step.startsWith(QLatin1String("key:"))) {
        const auto actions = window->actions();
        for (QAction *action : actions) {
            if (action->shortcuts().contains(QKeySequence(arg))) {
                action->trigger();
            }
        }
    } else if (step == QLatin1String("status")) {
        if (QToolButton *item = window->findChild<StatusBar *>()->item(QStringLiteral("problems"))) {
            item->click();
        }
    } else if (step == QLatin1String("rows")) {
        if (!panel || !tree) {
            qInfo().noquote() << "problems-test no panel";
        } else {
            qInfo().noquote() << "problems-test shown" << panel->isVisible() << "empty-text" << panel->findChild<QLabel *>()->isVisible();
            for (QModelIndex index = row(0); index.isValid(); index = tree->indexBelow(index)) {
                const bool file = index.data(Lieve::KindRole).toInt() == Lieve::FileItem;
                qInfo().noquote() << "problems-test row" << (file ? "file" : "  problem") << index.data().toString() << "|"
                                  << (file ? index.data(Lieve::DirRole).toString() + QStringLiteral(" | ") + index.data(Lieve::CountRole).toString()
                                           : index.data(Qt::UserRole + 100).toString());
            }
        }
    } else if (step.startsWith(QLatin1String("select:")) && tree) {
        tree->setFocus();
        tree->setCurrentIndex(row(arg.toInt()));
    } else if (step.startsWith(QLatin1String("hover:")) && tree) {
        const QPointF pos = QRectF(tree->visualRect(row(arg.toInt()))).center();
        QHoverEvent event(QEvent::HoverMove, pos, tree->viewport()->mapToGlobal(pos), QPointF(-1, -1));
        QApplication::sendEvent(tree->viewport(), &event);
    } else if (step.startsWith(QLatin1String("click:")) && tree) {
        const QPoint pos = tree->visualRect(row(arg.toInt())).center();
        for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, pos, tree->viewport()->mapToGlobal(pos), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &event);
        }
    } else if (step == QLatin1String("enter") && tree) {
        const auto shortcuts = tree->findChildren<QShortcut *>();
        for (QShortcut *shortcut : shortcuts) {
            if (shortcut->key() == QKeySequence(Qt::Key_Return)) {
                Q_EMIT shortcut->activated();
            }
        }
    } else if (step.startsWith(QLatin1String("closetab:"))) {
        Q_EMIT window->findChild<TerminalPanel *>()->findChild<TabView *>()->tabCloseRequested(arg.toInt());
    } else if (step == QLatin1String("where")) {
        QWidget *focus = QApplication::focusWidget();
        const auto editors = window->findChildren<KTextEditor::View *>();
        for (KTextEditor::View *editor : editors) {
            if (editor->isVisible()) { // the current tab of each editor group
                qInfo().noquote() << "problems-test editor" << editor->document()->documentName() << editor->cursorPosition().line() + 1
                                  << editor->cursorPosition().column() + 1;
            }
        }
        auto *terminal = window->findChild<TerminalPanel *>();
        auto *tabs = terminal->findChild<TabView *>();
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i) {
            titles << (i == tabs->currentIndex() ? QStringLiteral("*") : QString()) + tabs->tabBar()->tabText(i);
        }
        qInfo().noquote() << "problems-test panel" << (terminal->isVisible() ? "shown" : "hidden") << titles.join(QStringLiteral(", ")) << "focus"
                          << (focus ? focus->metaObject()->className() : "none") << (panel && panel->isAncestorOf(focus) ? "(problems)" : "");
    }
    QTimer::singleShot(delay, window, [window, steps] {
        testProblems(window, steps.mid(1));
    });
}

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
            if (tabView->parentWidget() && tabView->parentWidget()->objectName() == QLatin1String("editorSplit")) { // the editor groups
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
        if (qEnvironmentVariableIsSet("LIEVE_TEST_REPLACE")) {
            testReplace(window, search);
        }
        // "<extra rows to select>:<row to hover, counted from the current one>[:nofocus]"
        const QStringList explorerTest = qEnvironmentVariable("LIEVE_TEST_EXPLORER").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        if (!explorerTest.isEmpty()) {
            QTimer::singleShot(1200, window, [window, explorerTest] {
                auto *tree = window->findChild<Explorer *>()->findChild<QTreeView *>();
                if (!explorerTest.contains(QLatin1String("nofocus"))) {
                    tree->setFocus();
                }
                QModelIndex index = tree->currentIndex();
                for (int i = 0; i < explorerTest.value(0).toInt(); ++i) {
                    index = tree->indexBelow(index);
                    tree->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                }
                const int offset = explorerTest.value(1).toInt();
                if (offset != 0) {
                    QModelIndex hover = tree->currentIndex();
                    for (int i = 0; i < std::abs(offset); ++i) {
                        hover = offset > 0 ? tree->indexBelow(hover) : tree->indexAbove(hover);
                    }
                    const QPointF pos = QRectF(tree->visualRect(hover)).center();
                    QHoverEvent event(QEvent::HoverMove, pos, tree->viewport()->mapToGlobal(pos), QPointF(-1, -1));
                    QApplication::sendEvent(tree->viewport(), &event);
                }
            });
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
                    if (qEnvironmentVariableIsSet("LIEVE_TEST_QUICKOPEN_ACCEPT")) { // then press Enter
                        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QCoreApplication::sendEvent(edit, &enter);
                    }
                }
            });
        }
        if (qEnvironmentVariableIsSet("LIEVE_TEST_PALETTE")) { // command palette with ">" and this text
            const QString query = qEnvironmentVariable("LIEVE_TEST_PALETTE");
            const auto actions = window->actions();
            for (QAction *action : actions) {
                if (action->shortcuts().contains(QKeySequence(Qt::Key_F1))) {
                    action->trigger();
                }
            }
            QTimer::singleShot(700, window, [window, query] {
                auto *edit = window->findChild<QuickOpen *>()->findChild<QLineEdit *>();
                edit->setText(QLatin1Char('>') + query);
                // LIEVE_TEST_PALETTE_ACCEPT: press Enter, then type its value in the list that follows
                if (qEnvironmentVariableIsSet("LIEVE_TEST_PALETTE_ACCEPT")) {
                    QTimer::singleShot(300, edit, [edit] {
                        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QCoreApplication::sendEvent(edit, &enter);
                        const QString then = qEnvironmentVariable("LIEVE_TEST_PALETTE_ACCEPT");
                        if (!then.isEmpty() && edit->isVisible()) {
                            edit->setText(then);
                        }
                    });
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
        if (qEnvironmentVariableIsSet("LIEVE_TEST_GITPICK")) { // branches, stashes, history: steps, see GitPanel::runTest()
            window->findChild<GitPanel *>()->runTest(qEnvironmentVariable("LIEVE_TEST_GITPICK"));
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
        // The Settings window, on a category or a search (LIEVE_TEST_SETTINGS);
        // LIEVE_TEST_SETTINGS_SET="git/inlineBlame=false;editor/fontSize=14"
        // changes its controls as a user would; LIEVE_TEST_SETTINGS_CLICK then
        // presses a button (primaryButton = Restart, openConfig).
        if (qEnvironmentVariableIsSet("LIEVE_TEST_SETTINGS")) {
            SettingsDialog *dialog = SettingsDialog::showFor(window, qEnvironmentVariable("LIEVE_TEST_SETTINGS"));
            const QStringList changes = qEnvironmentVariable("LIEVE_TEST_SETTINGS_SET").split(QLatin1Char(';'), Qt::SkipEmptyParts);
            QTimer::singleShot(600, dialog, [dialog, changes] {
                for (const QString &change : changes) {
                    const QString name = change.section(QLatin1Char('='), 0, 0);
                    const QString value = change.section(QLatin1Char('='), 1);
                    auto *control = dialog->findChild<QWidget *>(name);
                    if (auto *check = qobject_cast<QCheckBox *>(control)) {
                        check->setChecked(value == QLatin1String("true"));
                    } else if (auto *combo = qobject_cast<QComboBox *>(control); combo && combo->findData(value) >= 0) {
                        combo->setCurrentIndex(combo->findData(value));
                    } else if (auto *spin = qobject_cast<QSpinBox *>(control)) {
                        spin->setValue(value.toInt());
                    } else if (auto *edit = qobject_cast<QLineEdit *>(control)) {
                        edit->setText(value);
                        Q_EMIT edit->editingFinished();
                    }
                    qInfo().noquote() << "settings-set" << name << value << (control ? "" : "(no such control)");
                }
                const QString click = qEnvironmentVariable("LIEVE_TEST_SETTINGS_CLICK");
                qunsetenv("LIEVE_TEST_SETTINGS_CLICK"); // not again in a restarted Lieve
                if (auto *button = click.isEmpty() ? nullptr : dialog->findChild<QAbstractButton *>(click)) {
                    qInfo().noquote() << "settings-click" << click;
                    button->click();
                }
            });
        }
        // LIEVE_TEST_SPLIT=<file to open in the new group>[,close]: Ctrl+\ on the
        // current editor, then that file there; "close" closes the new group's tabs.
        if (qEnvironmentVariableIsSet("LIEVE_TEST_SPLIT")) {
            const QStringList split = qEnvironmentVariable("LIEVE_TEST_SPLIT").split(QLatin1Char(','));
            const auto trigger = [window](const QKeySequence &key) {
                const auto actions = window->actions();
                for (QAction *action : actions) { // by shortcut: the text depends on the language
                    if (action->shortcuts().contains(key)) {
                        action->trigger();
                        return;
                    }
                }
            };
            trigger(QKeySequence(Qt::CTRL | Qt::Key_Backslash));
            if (!split.value(0).isEmpty()) {
                window->openFile(split.value(0));
            }
            if (split.contains(QStringLiteral("close"))) {
                trigger(QKeySequence(Qt::CTRL | Qt::Key_W));
                trigger(QKeySequence(Qt::CTRL | Qt::Key_W));
            }
            qInfo().noquote() << "groups" << window->findChildren<TabView *>().size() - 1; // minus the terminal's
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
                // LIEVE_TEST_SAVE_TEXT: inserted at the start of the cursor's line instead
                const QString text = qEnvironmentVariable("LIEVE_TEST_SAVE_TEXT");
                if (text.isEmpty()) {
                    view->document()->insertText(KTextEditor::Cursor(0, 0), QStringLiteral(" "));
                } else {
                    view->document()->insertText(KTextEditor::Cursor(view->cursorPosition().line(), 0), text);
                }
                QTimer::singleShot(saveAt, window, [window, view] {
                    QElapsedTimer timer;
                    timer.start();
                    if (qEnvironmentVariableIsSet("LIEVE_TEST_FORMAT")) { // Format Document instead of saving
                        for (QAction *action : window->actions()) { // found by shortcut: the text depends on the interface language
                            if (action->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I))) {
                                action->trigger();
                            }
                        }
                    } else {
                        view->document()->documentSave();
                    }
                    const KTextEditor::Cursor cursor = view->cursorPosition();
                    qInfo().noquote() << "save-ms" << timer.elapsed() << "cursor" << cursor.line() << cursor.column()
                                      << view->document()->line(cursor.line()).insert(cursor.column(), QLatin1Char('|')).trimmed();
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
        const QString lspTest = qEnvironmentVariable("LIEVE_TEST_LSP"); // steps, see LspManager::runTest()
        if (!lspTest.isEmpty()) {
            if (auto *lsp = window->findChild<Lieve::LspManager *>()) {
                lsp->runTest(lspTest);
            }
        }
        const QStringList problemsTest = qEnvironmentVariable("LIEVE_TEST_PROBLEMS").split(QLatin1Char(';'), Qt::SkipEmptyParts);
        if (!problemsTest.isEmpty()) {
            const int at = qEnvironmentVariableIntValue("LIEVE_TEST_PROBLEMS_AT");
            QTimer::singleShot(at > 0 ? at : 1000, window, [window, problemsTest] {
                testProblems(window, problemsTest);
            });
        }
        // LIEVE_TEST_CONFLICTS: steps on the focused editor, see MergeConflicts::runTest()
        const QString conflictSteps = qEnvironmentVariable("LIEVE_TEST_CONFLICTS");
        if (!conflictSteps.isEmpty()) {
            QTimer::singleShot(800, window, [window, conflictSteps] {
                const auto views = window->findChildren<KTextEditor::View *>();
                KTextEditor::View *focused = views.value(0);
                for (KTextEditor::View *view : views) {
                    if (view->isAncestorOf(QApplication::focusWidget())) {
                        focused = view;
                    }
                }
                if (focused) {
                    MergeConflicts::runTest(focused, conflictSteps);
                }
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
        const auto topLevels = QApplication::topLevelWidgets();
        for (QWidget *top : topLevels) {
            if (auto *dialog = qobject_cast<SettingsDialog *>(top); dialog && dialog->isVisible()) {
                dialog->grab().save(QString(screenshot).replace(QStringLiteral(".png"), QStringLiteral("-settings.png")));
                if (auto *content = dialog->findChild<QWidget *>(QStringLiteral("settingsContent"))) { // the whole list
                    content->grab().save(QString(screenshot).replace(QStringLiteral(".png"), QStringLiteral("-settings-all.png")));
                }
            }
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
    // Lieve backs up unsaved files itself (hot exit, also against crashes), so
    // KTextEditor's swap files (".name.kate-swp" next to each edited file, which
    // git would list as new files) are turned off.
    KConfigGroup editorConfig(KSharedConfig::openConfig(), QStringLiteral("KTextEditor Document"));
    if (editorConfig.readEntry("Swap File Mode", 1) != 0) {
        editorConfig.writeEntry("Swap File Mode", 0);
        editorConfig.sync();
    }
    Lieve::Theme::apply(app);
    WindowManager::instance().handleCommandLine(QCoreApplication::arguments(), QDir::currentPath());
    Lieve::JumpList::update();
    Updater::instance().start();
    setupTestHooks(app);


    const int result = QApplication::exec();
    WindowManager::instance().deleteAll();
    return result;
}
