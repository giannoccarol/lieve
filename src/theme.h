#pragma once

#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>

class QApplication;

namespace Lieve
{
// One palette for the whole window, derived from the editor's colour theme
// (like VS Code): side bars a little darker than the editor, thin borders,
// a single accent colour.
struct Colors {
    bool dark = true;
    QColor editor; // editor background
    QColor base; // window background, behind the cards
    QColor chrome; // the cards: side bar, editor, terminal (same as the editor)
    QColor cardBorder;
    QColor elevated; // overlays, menus, tooltips
    QColor border;
    QColor fg;
    QColor dim; // secondary text
    QColor hover;
    QColor input;
    QColor inputBorder;
    QColor accent;
    QColor accentText;
    QColor selection; // selected row, focused list
    QColor selectionInactive;
};

namespace Theme
{
// Call once, after QApplication and before creating windows.
void apply(QApplication &app);
const Colors &colors();
// The KTextEditor theme used by the editor ("auto" resolved).
QString editorThemeName();
// True when Lieve's own palette and stylesheet are in use ([ui] theme=editor):
// selections are then a soft tint and keep the normal text colour.
bool styled();
// The 16 ANSI colours of Lieve's terminals (its Konsole profile and the
// built-in terminal): those of VS Code's integrated terminal, Dark+ or Light+.
QList<QColor> ansiColors(bool dark);
// Lieve's private Konsole profile that runs `shell`, with the editor's colours
// unless [terminal] matchEditor=false ("" if it could not be written).
QString konsoleProfile(const QString &shell);
// Activity bar icon: dimmed when off, full when checked or hovered, with an
// optional counter badge.
QIcon activityIcon(const QIcon &base, int badge = 0);
enum class Glyph {
    Minimize,
    Maximize,
    Restore,
    Close,
    TabClose, // the × on tabs
    Sidebar, // layout toggles: the pane fills in when shown
    Panel,
    ChevronDown, // menu next to a button (new terminal)
};
// Thin line icons for the title bar, in the theme's colours (hover variant included).
QIcon glyph(Glyph kind);
// Turns a widget into a card with rounded corners, drawn above its content
// so editor and terminal are rounded too.
void makeCard(QWidget *card);
}
}
