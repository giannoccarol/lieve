#include "commandpalette.h"

#include "settings.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QAction>
#include <QCoreApplication>
#include <QMenu>

#include <algorithm>
#include <iterator>

namespace Lieve
{
namespace
{
const char kCategoryProperty[] = "lieveCategory";
constexpr int kMaxRecent = 30;

// Editor actions left out: cursor movements and other keys rather than
// commands, KTextEditor's own interface and settings (Lieve sets them from
// lieve.conf), Kate-only modes and actions the window already has (Ctrl+G).
const char *const kHiddenEditorActions[] = {
    "word_left", "word_right", "select_char_left", "select_char_right", "select_word_left", "select_word_right",
    "subword_left", "subword_right", "select_subword_left", "select_subword_right", "beginning_of_line",
    "beginning_of_document", "select_beginning_of_line", "select_beginning_of_document", "end_of_line", "end_of_document",
    "select_end_of_line", "select_end_of_document", "move_line_up", "move_line_down", "move_cursor_left",
    "move_cursor_right", "select_line_up", "select_line_down", "scroll_line_up", "scroll_line_down", "scroll_page_up",
    "scroll_page_down", "select_page_up", "select_page_down", "move_top_of_view", "select_top_of_view",
    "move_bottom_of_view", "select_bottom_of_view", "mark_selection", "backspace", "delete_next_character",
    "delete_word_left", "delete_word_right", "delete_subword_left", "delete_subword_right", "insert_tabulator",
    "smart_newline", "no_indent_newline", "doccomplete_sh", "doccomplete_bw", "doccomplete_fw",
    "set_confdlg", "view_schemas", "view_border", "view_line_numbers", "view_folding_markers", "view_scrollbar_marks",
    "view_static_word_wrap", "view_word_wrap_marker", "dynamic_word_wrap_indicators", "view_auto_reload",
    "switch_to_cmd_line", "switch_next_input_mode", "force_rtl_direction", "tools_mode", "tools_indentation",
    "tools_speech_say", "tools_speech_stop", "tools_speech_pause", "tools_speech_resume", "go_goto_line",
};

// KTextEditor disables these while its view has no focus (the palette has
// it); they work again once the view is focused to run them.
const char *const kFocusActions[] = {"delete_line", "transpose_char", "transpose_word", "newline_above", "newline_below"};

// Editor actions whose menu holds the choices: offered as a second list.
const char *const kChoiceActions[] = {"tools_highlighting", "set_encoding", "set_eol", "file_save_as_with_encoding", "view_input_modes"};

bool listed(const char *const *begin, const char *const *end, const QString &name)
{
    return std::any_of(begin, end, [&name](const char *n) {
        return name == QLatin1String(n);
    });
}

template<std::size_t N>
bool listed(const char *const (&names)[N], const QString &name)
{
    return listed(std::begin(names), std::end(names), name);
}

QString plainText(const QAction *action)
{
    return KLocalizedString::removeAcceleratorMarker(action->text()).trimmed();
}

QString translatedCategory(const char *category)
{
    return category && *category ? QCoreApplication::translate("CommandPalette", category) : QString();
}

// VS Code's names where KTextEditor's would be unclear out of their menu.
QString editorTitle(const QString &name, const QAction *action)
{
    if (name == QLatin1String("tools_highlighting")) {
        return QCoreApplication::translate("CommandPalette", "Change Language Mode");
    }
    if (name == QLatin1String("set_encoding")) {
        return QCoreApplication::translate("CommandPalette", "Change File Encoding");
    }
    if (name == QLatin1String("set_eol")) {
        return QCoreApplication::translate("CommandPalette", "Change End of Line Sequence");
    }
    if (name == QLatin1String("view_dynamic_word_wrap")) {
        return QCoreApplication::translate("CommandPalette", "Toggle Word Wrap");
    }
    if (name == QLatin1String("view_scrollbar_minimap")) {
        return QCoreApplication::translate("CommandPalette", "Toggle Minimap");
    }
    return plainText(action);
}

const char *editorCategory(const QString &name)
{
    if (name.startsWith(QLatin1String("file_"))) {
        return CommandCategory::File;
    }
    if (name.startsWith(QLatin1String("view_"))) {
        return CommandCategory::View;
    }
    if (name.startsWith(QLatin1String("bookmarks_"))) {
        return CommandCategory::Bookmarks;
    }
    if (name.startsWith(QLatin1String("tools_scripts_emmet"))) {
        return "Emmet";
    }
    return nullptr;
}

Command makeCommand(QAction *action, const QString &title, const QString &category, const std::function<void()> &prepare)
{
    Command command;
    command.id = action->objectName().isEmpty() ? title : action->objectName();
    command.category = category;
    command.title = title;
    const QList<QKeySequence> keys = action->shortcuts();
    command.shortcut = keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::NativeText);
    command.action = action;
    command.prepare = prepare;
    return command;
}

bool hasAction(const QList<Command> &commands, const QAction *action)
{
    return std::any_of(commands.cbegin(), commands.cend(), [action](const Command &c) {
        return c.action == action;
    });
}
}

QString Command::label() const
{
    return category.isEmpty() ? title : QCoreApplication::translate("CommandPalette", "%1: %2", "category: command").arg(category, title);
}

void setCommandCategory(QAction *action, const char *category)
{
    action->setProperty(kCategoryProperty, QString::fromUtf8(category));
}

void addCommands(QList<Command> &commands, const QList<QAction *> &actions, const char *category, const std::function<void()> &prepare)
{
    for (QAction *action : actions) {
        if (action->isSeparator() || !action->isVisible() || !action->isEnabled()) {
            continue;
        }
        if (QMenu *menu = action->menu<QMenu *>()) {
            addMenuCommands(commands, menu, category, prepare);
            continue;
        }
        const QString title = plainText(action);
        if (title.isEmpty() || hasAction(commands, action)) {
            continue;
        }
        const QByteArray own = action->property(kCategoryProperty).toString().toUtf8();
        commands << makeCommand(action, title, translatedCategory(own.isEmpty() ? category : own.constData()), prepare);
    }
}

void addMenuCommands(QList<Command> &commands, QMenu *menu, const char *category, const std::function<void()> &prepare)
{
    Q_EMIT menu->aboutToShow(); // menus fill or update their actions when shown
    addCommands(commands, menu->actions(), category, prepare);
}

void addEditorCommands(QList<Command> &commands, KTextEditor::View *view)
{
    const QPointer<KTextEditor::View> target(view);
    const auto focusView = [target] {
        if (target) {
            target->setFocus();
        }
    };
    const bool writable = view->document()->isReadWrite();
    const qsizetype windowCommands = commands.size();
    const QList<QAction *> actions = view->actionCollection()->actions();
    for (QAction *action : actions) {
        const QString name = action->objectName();
        if (action->isSeparator() || !action->isVisible() || listed(kHiddenEditorActions, name)) {
            continue;
        }
        if (!action->isEnabled() && !(writable && listed(kFocusActions, name))) {
            continue;
        }
        const bool choices = listed(kChoiceActions, name);
        if (action->menu<QMenu *>() && !choices) {
            continue; // groups of actions that are listed on their own
        }
        const QString title = editorTitle(name, action);
        if (title.isEmpty()) {
            continue;
        }
        Command command = makeCommand(action, title, translatedCategory(editorCategory(name)), focusView);
        command.id = QStringLiteral("editor/") + name;
        command.choices = choices;
        // KTextEditor has some twice (Join Lines, Remove Trailing Spaces): keep one.
        const QString label = command.label();
        const auto same = std::find_if(commands.begin() + windowCommands, commands.end(), [&label](const Command &c) {
            return c.label() == label;
        });
        if (same == commands.end()) {
            commands << command;
        } else if (same->shortcut.isEmpty() && !command.shortcut.isEmpty()) {
            *same = command;
        }
    }
}

QList<CommandChoice> commandChoices(QAction *action)
{
    QList<CommandChoice> choices;
    const std::function<void(QMenu *, const QString &)> collect = [&](QMenu *menu, const QString &detail) {
        Q_EMIT menu->aboutToShow(); // the language menu fills itself then
        const QList<QAction *> actions = menu->actions();
        for (QAction *a : actions) {
            if (a->isSeparator() || !a->isVisible() || !a->isEnabled()) {
                continue;
            }
            if (QMenu *sub = a->menu<QMenu *>()) {
                collect(sub, plainText(a));
                continue;
            }
            const QString title = plainText(a);
            if (!title.isEmpty()) {
                choices.append({a, title, detail, a->isChecked()});
            }
        }
    };
    if (QMenu *menu = action->menu<QMenu *>()) {
        collect(menu, QString());
    }
    return choices;
}

QStringList recentCommands()
{
    return state().value(QStringLiteral("commandPalette/recent")).toStringList();
}

void rememberCommand(const QString &id)
{
    QStringList recent = recentCommands();
    recent.removeAll(id);
    recent.prepend(id);
    while (recent.size() > kMaxRecent) {
        recent.removeLast();
    }
    state().setValue(QStringLiteral("commandPalette/recent"), recent);
}
}
