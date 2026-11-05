#pragma once

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

class QAction;
class QMenu;
namespace KTextEditor
{
class View;
}

// What the command palette (Ctrl+Shift+P, ">" in Quick Open) lists: the
// window's actions and the useful actions of the current editor, gathered
// each time it opens.
namespace Lieve
{
struct Command {
    QString id; // remembered in the recently used list
    QString category; // translated, may be empty
    QString title; // without '&' mnemonics
    QString shortcut; // first key sequence, native text
    QPointer<QAction> action;
    std::function<void()> prepare; // runs before the action (focus the editor, show a panel)
    bool choices = false; // its menu holds the choices, offered as a second list
    QString label() const; // "View: Toggle Panel"
};

struct CommandChoice {
    QPointer<QAction> action;
    QString title;
    QString detail; // submenu it comes from ("Sources")
    bool current = false;
};

// Categories shown before the commands, like VS Code's ("View: …"). Any
// other English name works too; these are the translated ones.
namespace CommandCategory
{
inline constexpr const char *File = QT_TRANSLATE_NOOP("CommandPalette", "File");
inline constexpr const char *View = QT_TRANSLATE_NOOP("CommandPalette", "View");
inline constexpr const char *Search = QT_TRANSLATE_NOOP("CommandPalette", "Search");
inline constexpr const char *Terminal = QT_TRANSLATE_NOOP("CommandPalette", "Terminal");
inline constexpr const char *Git = QT_TRANSLATE_NOOP("CommandPalette", "Git");
inline constexpr const char *Preferences = QT_TRANSLATE_NOOP("CommandPalette", "Preferences");
inline constexpr const char *Bookmarks = QT_TRANSLATE_NOOP("CommandPalette", "Bookmarks");
}
void setCommandCategory(QAction *action, const char *category);

// Adds the visible, enabled actions that have a text (and those of their
// submenus), skipping ones already listed. `category` is for actions
// without their own.
void addCommands(QList<Command> &commands,
                 const QList<QAction *> &actions,
                 const char *category = nullptr,
                 const std::function<void()> &prepare = {});
// Same for a menu's actions, after letting it update them (aboutToShow).
void addMenuCommands(QList<Command> &commands, QMenu *menu, const char *category = nullptr, const std::function<void()> &prepare = {});
// Adds the editor's own commands (Toggle Comment, Change Language Mode…),
// without cursor movements and settings that Lieve manages itself.
void addEditorCommands(QList<Command> &commands, KTextEditor::View *view);
// The choices of a command whose menu holds them (languages, encodings).
QList<CommandChoice> commandChoices(QAction *action);

// Ids of the commands run last, newest first (state.ini).
QStringList recentCommands();
void rememberCommand(const QString &id);
}
