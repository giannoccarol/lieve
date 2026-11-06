#include "theme.h"

#include "settings.h"

#include <KConfig>
#include <KConfigGroup>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Editor>

#include <QApplication>
#include <QPainterPath>
#include <QStackedWidget>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSaveFile>
#include <QStandardPaths>

namespace Lieve::Theme
{
namespace
{
Colors g_colors;
QString g_editorTheme;
QString g_konsoleDir; // Lieve's private Konsole profiles, one per shell
bool g_konsoleColors = false; // they use the editor's colours
bool g_styled = false;

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t),
                            float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

QColor withAlpha(QColor c, double alpha)
{
    c.setAlphaF(float(alpha));
    return c;
}

QString css(const QColor &c)
{
    if (c.alpha() == 255) {
        return c.name();
    }
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QColor readableOn(const QColor &background)
{
    const double luminance = 0.2126 * background.redF() + 0.7152 * background.greenF() + 0.0722 * background.blueF();
    return luminance > 0.6 ? QColor(0x1a, 0x1a, 0x1a) : QColor(Qt::white);
}

QColor accentColor(const QPalette &system, bool dark)
{
    const QString setting = config().value(QStringLiteral("ui/accent"), QStringLiteral("system")).toString().trimmed().toLower();
    if (setting == QLatin1String("lieve")) { // the burgundy of the icon
        return dark ? QColor(0xd0, 0x60, 0x7a) : QColor(0x9c, 0x2f, 0x48);
    }
    if (setting.startsWith(QLatin1Char('#'))) {
        const QColor custom = QColor::fromString(setting);
        if (custom.isValid()) {
            return custom;
        }
    }
    return system.color(QPalette::Active, QPalette::Highlight); // Plasma's accent colour
}

// Small SVG icons for the stylesheet (tree chevrons, tab close), drawn in the
// theme's colours.
QString writeIcon(const QString &name, const QString &svg)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/theme");
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name + QStringLiteral(".svg");
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(svg.toUtf8());
        f.commit();
    }
    return path;
}

QString strokeIcon(const QString &name, const QString &shape, const QColor &color)
{
    return writeIcon(name,
                     QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>"
                                    "<path d='%1' fill='none' stroke='%2' stroke-width='1.3' stroke-linecap='round' stroke-linejoin='round'/></svg>")
                         .arg(shape, color.name()));
}

QPalette buildPalette(const Colors &c)
{
    QPalette p;
    const auto both = [&p](QPalette::ColorRole role, const QColor &color) {
        p.setColor(QPalette::Active, role, color);
        p.setColor(QPalette::Inactive, role, color);
        p.setColor(QPalette::Disabled, role, color);
    };
    both(QPalette::Window, c.chrome);
    both(QPalette::Base, c.chrome);
    both(QPalette::AlternateBase, c.hover);
    both(QPalette::Button, mix(c.chrome, c.fg, c.dark ? 0.08 : 0.06));
    both(QPalette::ToolTipBase, c.elevated);
    both(QPalette::ToolTipText, c.fg);
    both(QPalette::Light, mix(c.chrome, c.fg, 0.20));
    both(QPalette::Midlight, mix(c.chrome, c.fg, 0.12));
    both(QPalette::Mid, mix(c.chrome, c.fg, 0.10));
    both(QPalette::Dark, mix(c.chrome, Qt::black, 0.35));
    both(QPalette::Shadow, Qt::black);
    both(QPalette::Link, c.accent);
    both(QPalette::LinkVisited, c.accent);
    both(QPalette::BrightText, Qt::white);
    for (const auto group : {QPalette::Active, QPalette::Inactive}) {
        p.setColor(group, QPalette::WindowText, c.fg);
        p.setColor(group, QPalette::Text, c.fg);
        p.setColor(group, QPalette::ButtonText, c.fg);
        p.setColor(group, QPalette::PlaceholderText, c.dim);
    }
    p.setColor(QPalette::Active, QPalette::Highlight, c.accent);
    p.setColor(QPalette::Active, QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::Inactive, QPalette::Highlight, c.selectionInactive);
    p.setColor(QPalette::Inactive, QPalette::HighlightedText, c.fg);
    const QColor disabled = withAlpha(c.fg, 0.38);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::PlaceholderText, withAlpha(c.fg, 0.25));
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.selectionInactive);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QString buildStyleSheet(const Colors &c)
{
    const QString chevronRight = strokeIcon(QStringLiteral("chevron-right"), QStringLiteral("M6 4l4 4-4 4"), mix(c.fg, c.chrome, 0.25));
    const QString chevronDown = strokeIcon(QStringLiteral("chevron-down"), QStringLiteral("M4 6l4 4 4-4"), mix(c.fg, c.chrome, 0.25));
    const QString chevronDownOnAccent = strokeIcon(QStringLiteral("chevron-down-accent"), QStringLiteral("M4 6l4 4 4-4"), c.accentText);
    const QString close = strokeIcon(QStringLiteral("close"), QStringLiteral("M4.5 4.5l7 7M11.5 4.5l-7 7"), mix(c.fg, c.chrome, 0.2));

    QString s = QStringLiteral(R"(
/* ---------- activity bar ---------- */
/* the window paints @base itself, with rounded corners when it has no system frame */
QWidget#central, #titleBar, #statusBar { background: transparent; }
QToolButton#statusItem { background: transparent; border: none; border-radius: 4px; padding: 1px 6px; color: @dim; font-size: 12px; }
QToolButton#statusItem:hover { background: @tileHover; color: @fg; }
QToolButton#statusAccent { background: @accent; color: @accentText; border: none; border-radius: 4px; padding: 1px 8px; font-size: 12px; }
QToolButton#statusAccent:hover { background: @accentHover; }
#activityBar { background: transparent; border: none; padding: 2px 0px; spacing: 4px; }
/* each icon sits on a rounded tile: soft on hover, raised when active */
QToolBar#activityBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 9px; padding: 7px; margin: 0px 6px; }
QToolBar#activityBar QToolButton:hover { background: @tileHover; }
QToolBar#activityBar QToolButton:pressed { background: @tile; }
QToolBar#activityBar QToolButton:checked, QToolBar#activityBar QToolButton:checked:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @tileTop, stop:1 @tile); border: 1px solid @tileBorder; }
#activityBar QToolButton::menu-indicator { image: none; width: 0px; }

/* ---------- splitters: a hairline with a comfortable grab area ---------- */
/* ---------- cards with gaps between them ---------- */
QSplitter::handle { background: transparent; }
#sideStack, #editorCard, #terminalCard { background: @chrome; }

/* ---------- title bar ---------- */
QToolButton#titleButton, QToolButton#toggleButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton#titleButton:hover, QToolButton#toggleButton:hover { background: @tileHover; }
QToolButton#toggleButton:checked { background: transparent; }
QToolButton#toggleButton:checked:hover { background: @tileHover; }
/* window buttons like Plasma's Breeze: thin glyphs, a round highlight on hover */
QToolButton#windowButton, QToolButton#closeButton { background: transparent; border: none; border-radius: 11px; padding: 3px; }
QToolButton#windowButton:hover { background: @winHover; }
QToolButton#windowButton:pressed { background: @dim; }
QToolButton#closeButton:hover { background: #da4453; }
QToolButton#closeButton:pressed { background: #b83a48; }
QToolButton#titleButton::menu-indicator { image: none; width: 0px; }
QPushButton#commandCenter { background: @tile; color: @dim; border: 1px solid @tileBorder; border-radius: 7px; padding: 2px 10px; }
QPushButton#commandCenter:hover { background: @tileTop; color: @fg; }

/* ---------- side bar ---------- */
QLabel#sideTitle { color: @titleText; font-size: 11px; font-weight: 600; letter-spacing: 0.5px; }
#sideStack QTreeView, #sideStack QListView, #sideStack QTreeWidget, #sideStack QListWidget,
#overlay QListWidget, #chooserFrame QListWidget { background: transparent; border: none; outline: 0; }
#sideStack QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px; }
#sideStack QToolButton:hover { background: @hover; }
#sideStack QToolButton:checked { background: @accentSoft; border: 1px solid @accent; }
#sideStack QToolButton::menu-indicator { image: none; width: 0px; }
QToolButton#shellMenu::menu-indicator { image: none; width: 0px; }
#sideStack QLineEdit, #overlay QLineEdit, #chooserFrame QLineEdit, QPlainTextEdit#commitMessage {
    background: @input; color: @fg; border: 1px solid @inputBorder; border-radius: 4px; padding: 4px 6px;
    selection-background-color: @accent; selection-color: @accentText; }
#sideStack QLineEdit:focus, #overlay QLineEdit:focus, #chooserFrame QLineEdit:focus, QPlainTextEdit#commitMessage:focus { border: 1px solid @accent; }

/* ---------- lists and trees ---------- */
QTreeView, QListView { outline: 0; }
QTreeView::item, QListView::item { border: none; padding: 1px 0px; }
QTreeView::item:hover, QListView::item:hover { background: @hover; }
QTreeView::item:selected, QListView::item:selected { background: @selectionInactive; color: @fg; }
QTreeView::item:selected:active, QListView::item:selected:active { background: @selection; color: @fg; }
/* keyboard-driven lists (Ctrl+P, Ctrl+R, project chooser) never take focus */
#overlay QListView::item:selected, #chooserFrame QListView::item:selected { background: @selection; }
QTreeView::branch { background: transparent; }
QTreeView::branch:selected { background: @selectionInactive; }
QTreeView::branch:selected:active { background: @selection; }
QTreeView::branch:has-children:!has-siblings:closed, QTreeView::branch:closed:has-children:has-siblings { image: url(@chevronRight); }
QTreeView::branch:open:has-children:!has-siblings, QTreeView::branch:open:has-children:has-siblings { image: url(@chevronDown); }

/* ---------- tabs (editor and terminal) ---------- */
QTabWidget::pane { border: none; }
QTabWidget::tab-bar { left: 0px; }
QTabBar { background: transparent; }
QTabBar::tab { background: transparent; color: @dim; border: none; border-radius: 7px; padding: 4px 4px 4px 10px;
    margin: 6px 2px 2px 0px; min-width: 30px; }
QTabBar::tab:first { margin-left: 8px; }
QTabBar::tab:selected { background: @tabActive; color: @fg; }
QTabBar::tab:hover:!selected { background: @hover; color: @fg; }
QTabBar::close-button { image: url(@close); subcontrol-position: right; margin-left: 4px; padding: 1px; border-radius: 3px; }
QTabBar::close-button:hover { background: @hoverStrong; }
QTabBar QToolButton { background: transparent; border: none; }
QSplitter#editorSplit::handle { background: @border; }
QToolButton#tabClose { background: transparent; border: none; border-radius: 5px; padding: 0px; margin: 0px 2px 0px 6px; }
QToolButton#tabClose:hover { background: @hoverStrong; }
QScrollArea#tabScroll, QScrollArea#tabScroll > QWidget, QScrollArea#tabScroll > QWidget > QWidget { background: transparent; }
QScrollArea#tabScroll QScrollBar:horizontal { height: 3px; background: transparent; }

/* ---------- buttons ---------- */
QPushButton#primaryButton { background: @accent; color: @accentText; border: none; border-radius: 4px; padding: 6px 12px; }
QPushButton#primaryButton:hover { background: @accentHover; }
QPushButton#primaryButton:disabled { background: @hover; color: @disabledText; }
QToolButton#primarySplit { background: @accent; border: none; border-radius: 4px; padding: 0px 6px; }
QToolButton#primarySplit:hover { background: @accentHover; }
QToolButton#primarySplit:disabled { background: @hover; }
QToolButton#primarySplit::menu-indicator { image: url(@chevronDownOnAccent); subcontrol-position: center; subcontrol-origin: padding; width: 14px; }
QPushButton#secondaryButton { background: @hover; color: @fg; border: 1px solid @inputBorder; border-radius: 4px; padding: 6px 12px; }
QPushButton#secondaryButton:hover { background: @hoverStrong; }

/* ---------- overlays, menus, tooltips ---------- */
QFrame#overlay, QFrame#chooserFrame { background: @elevated; border: 1px solid @border; border-radius: 8px; }
QTreeWidget#breadcrumbList { background: transparent; border: none; outline: 0; }
QMenu { background: @elevated; color: @fg; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 10px; border-radius: 4px; }
QMenu::item:selected { background: @selection; }
QMenu::item:disabled { color: @disabledText; }
QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }
QToolTip { background: @elevated; color: @fg; border: 1px solid @border; padding: 4px 6px; }

/* ---------- scrollbars: thin, no track, like VS Code ---------- */
QScrollBar:vertical { background: @chrome; width: 9px; margin: 0px; border: none; }
QScrollBar:horizontal { background: @chrome; height: 9px; margin: 0px; border: none; }
KateScrollBar:vertical, KateScrollBar:horizontal { background: @editor; }
/* a slim rounded thumb that widens a little under the mouse */
QScrollBar::handle { background: @scroll; border: none; border-radius: 2px; }
QScrollBar::handle:vertical { min-height: 32px; margin: 2px 2px 2px 3px; }
QScrollBar::handle:horizontal { min-width: 32px; margin: 3px 2px 2px 2px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:vertical:pressed { margin: 1px 1px 1px 2px; border-radius: 3px; }
QScrollBar::handle:horizontal:hover, QScrollBar::handle:horizontal:pressed { margin: 2px 1px 1px 1px; border-radius: 3px; }
QScrollBar::handle:hover { background: @scrollHover; }
QScrollBar::handle:pressed { background: @scrollPressed; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QProgressBar { background: transparent; border: none; }
QProgressBar::chunk { background: @accent; }
)");
    const QList<QPair<QString, QString>> values = {
        {QStringLiteral("@chevronDownOnAccent"), chevronDownOnAccent},
        {QStringLiteral("@chevronRight"), chevronRight},
        {QStringLiteral("@chevronDown"), chevronDown},
        {QStringLiteral("@close"), close},
        {QStringLiteral("@tileBorder"), css(mix(c.base, c.fg, c.dark ? 0.17 : 0.15))},
        {QStringLiteral("@tileHover"), css(mix(c.base, c.fg, c.dark ? 0.06 : 0.05))},
        {QStringLiteral("@tileTop"), css(mix(c.base, c.fg, c.dark ? 0.13 : 0.03))},
        {QStringLiteral("@tile"), css(mix(c.base, c.fg, c.dark ? 0.08 : 0.07))},
        {QStringLiteral("@winHover"), css(mix(c.base, c.fg, c.dark ? 0.78 : 0.70))},
        {QStringLiteral("@tabActive"), css(mix(c.chrome, c.fg, c.dark ? 0.10 : 0.07))},
        {QStringLiteral("@base"), css(c.base)},
        {QStringLiteral("@chrome"), css(c.chrome)},
        {QStringLiteral("@editor"), css(c.editor)},
        {QStringLiteral("@elevated"), css(c.elevated)},
        {QStringLiteral("@borderStrong"), css(mix(c.chrome, c.fg, 0.25))},
        {QStringLiteral("@border"), css(c.border)},
        {QStringLiteral("@titleText"), css(mix(c.fg, c.chrome, 0.20))},
        {QStringLiteral("@fg"), css(c.fg)},
        {QStringLiteral("@dim"), css(c.dim)},
        {QStringLiteral("@hoverStrong"), css(mix(c.chrome, c.fg, c.dark ? 0.14 : 0.12))},
        {QStringLiteral("@hover"), css(c.hover)},
        {QStringLiteral("@inputBorder"), css(c.inputBorder)},
        {QStringLiteral("@input"), css(c.input)},
        {QStringLiteral("@accentText"), css(c.accentText)},
        {QStringLiteral("@accentHover"), css(c.dark ? c.accent.lighter(112) : c.accent.darker(110))},
        {QStringLiteral("@accentSoft"), css(withAlpha(c.accent, 0.25))},
        {QStringLiteral("@accent"), css(c.accent)},
        {QStringLiteral("@selectionInactive"), css(c.selectionInactive)},
        {QStringLiteral("@selection"), css(c.selection)},
        {QStringLiteral("@disabledText"), css(withAlpha(c.fg, 0.38))},
        {QStringLiteral("@scrollPressed"), css(withAlpha(c.fg, 0.40))},
        {QStringLiteral("@scrollHover"), css(withAlpha(c.fg, 0.28))},
        {QStringLiteral("@scroll"), css(withAlpha(c.fg, 0.16))},
    };
    for (const auto &[key, value] : values) {
        s.replace(key, value);
    }
    return s;
}

// Draws a card's rounded corners and outline above its content (editor,
// terminal...), so they stay smooth whatever is underneath.
class CardCorners : public QWidget
{
public:
    explicit CardCorners(QWidget *card)
        : QWidget(card)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        card->installEventFilter(this);
        if (auto *stack = qobject_cast<QStackedWidget *>(card)) {
            QObject::connect(stack, &QStackedWidget::currentChanged, this, [this] {
                raise();
            });
        }
        setGeometry(card->rect());
        raise();
        show();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()) {
            if (event->type() == QEvent::Resize) {
                setGeometry(parentWidget()->rect());
                m_corners = QPainterPath();
            }
            if (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded || event->type() == QEvent::Show) {
                raise();
            }
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        const qreal radius = 10;
        if (m_corners.isEmpty()) {
            const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            m_outline = QPainterPath();
            m_outline.addRoundedRect(r, radius, radius);
            QPainterPath all;
            all.addRect(rect());
            m_corners = all.subtracted(m_outline);
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(m_corners, g_colors.base);
        p.setPen(QPen(g_colors.cardBorder, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(m_outline);
    }

private:
    QPainterPath m_corners;
    QPainterPath m_outline;
};

// Lieve's own Konsole profiles (one per shell, see konsoleProfile()) and colour
// scheme. They live in Lieve's data folder, which only Lieve adds to Konsole's
// search path: the Konsole app and its profiles are left alone.
void prepareKonsole(const Colors &c)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lieve/xdg");
    const QString dir = base + QStringLiteral("/konsole");
    if (!QDir().mkpath(dir)) {
        return;
    }
    // Profiles are written again as terminals open, so Konsole never picks up
    // one left by an older run (another shell path, other colours).
    const QStringList old = QDir(dir).entryList({QStringLiteral("*.profile")}, QDir::Files);
    for (const QString &name : old) {
        QFile::remove(dir + QLatin1Char('/') + name);
    }

    QByteArray dirs = qgetenv("XDG_DATA_DIRS");
    if (dirs.isEmpty()) {
        dirs = "/usr/local/share:/usr/share";
    }
    if (!dirs.split(':').contains(base.toUtf8())) {
        qputenv("XDG_DATA_DIRS", dirs + ':' + base.toUtf8());
    }
    g_konsoleDir = dir;

    if (!config().value(QStringLiteral("terminal/matchEditor"), true).toBool()) {
        return;
    }

    // ANSI colours of VS Code's integrated terminal (Dark+/Light+)
    static const char *darkAnsi[16] = {"#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
                                       "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#ffffff"};
    static const char *lightAnsi[16] = {"#000000", "#cd3131", "#00bc00", "#949800", "#0451a5", "#bc05bc", "#0598bc", "#555555",
                                        "#666666", "#cd3131", "#14ce14", "#b5ba00", "#0451a5", "#bc05bc", "#0598bc", "#a5a5a5"};
    const char **ansi = c.dark ? darkAnsi : lightAnsi;
    const auto rgb = [](const QColor &col) {
        return QStringLiteral("%1,%2,%3").arg(col.red()).arg(col.green()).arg(col.blue());
    };
    QString scheme;
    const QColor background = c.chrome;
    for (const char *suffix : {"", "Intense", "Faint"}) {
        scheme += QStringLiteral("[Background%1]\nColor=%2\n\n").arg(QLatin1String(suffix), rgb(background));
    }
    for (int i = 0; i < 8; ++i) {
        const QColor normal(QLatin1String(ansi[i]));
        const QColor intense(QLatin1String(ansi[i + 8]));
        scheme += QStringLiteral("[Color%1]\nColor=%2\n\n").arg(i).arg(rgb(normal));
        scheme += QStringLiteral("[Color%1Intense]\nColor=%2\n\n").arg(i).arg(rgb(intense));
        scheme += QStringLiteral("[Color%1Faint]\nColor=%2\n\n").arg(i).arg(rgb(mix(normal, background, 0.4)));
    }
    scheme += QStringLiteral("[Foreground]\nColor=%1\n\n[ForegroundIntense]\nColor=%2\n\n[ForegroundFaint]\nColor=%3\n\n")
                  .arg(rgb(c.fg), rgb(c.dark ? QColor(Qt::white) : QColor(Qt::black)), rgb(c.dim));
    scheme += QStringLiteral("[General]\nBlur=false\nColorRandomization=false\nDescription=Lieve\nOpacity=1\nWallpaper=\n");
    QSaveFile schemeFile(dir + QStringLiteral("/Lieve.colorscheme"));
    if (schemeFile.open(QIODevice::WriteOnly)) {
        schemeFile.write(scheme.toUtf8());
        schemeFile.commit();
    }

    g_konsoleColors = true;
}
}

void apply(QApplication &app)
{
    const QPalette system = app.palette();
    const bool systemDark = system.color(QPalette::Window).lightness() < 128;
    QString name = config().value(QStringLiteral("editor/theme"), QStringLiteral("auto")).toString().trimmed();
    if (name.isEmpty() || name == QLatin1String("auto")) {
        name = systemDark ? QStringLiteral("VSCodium Dark") : QStringLiteral("Breeze Light");
    }
    const KSyntaxHighlighting::Repository &repository = KTextEditor::Editor::instance()->repository();
    KSyntaxHighlighting::Theme theme = repository.theme(name);
    if (!theme.isValid()) {
        theme = repository.defaultTheme(systemDark ? KSyntaxHighlighting::Repository::DarkTheme : KSyntaxHighlighting::Repository::LightTheme);
    }
    g_editorTheme = theme.name();

    Colors c;
    c.editor = QColor::fromRgb(theme.editorColor(KSyntaxHighlighting::Theme::BackgroundColor));
    c.fg = QColor::fromRgb(theme.textColor(KSyntaxHighlighting::Theme::Normal));
    c.dark = c.editor.lightness() < 128;
    c.chrome = c.editor;
    c.base = c.dark ? mix(c.editor, Qt::black, 0.30) : mix(c.editor, Qt::black, 0.06);
    c.cardBorder = mix(c.editor, c.fg, c.dark ? 0.07 : 0.10);
    c.elevated = c.dark ? mix(c.editor, Qt::white, 0.05) : QColor(Qt::white);
    c.border = mix(c.chrome, c.fg, c.dark ? 0.11 : 0.13);
    c.dim = mix(c.fg, c.chrome, 0.40);
    c.hover = mix(c.chrome, c.fg, c.dark ? 0.07 : 0.05);
    c.input = c.dark ? mix(c.editor, c.fg, 0.06) : QColor(Qt::white);
    c.inputBorder = mix(c.chrome, c.fg, c.dark ? 0.17 : 0.22);
    c.accent = accentColor(system, c.dark);
    c.accentText = readableOn(c.accent);
    c.selection = mix(c.chrome, c.accent, c.dark ? 0.38 : 0.22);
    c.selectionInactive = mix(c.chrome, c.fg, c.dark ? 0.11 : 0.09);
    g_colors = c;

    // [ui] theme=system keeps Plasma's own colours for the window.
    if (config().value(QStringLiteral("ui/theme"), QStringLiteral("editor")).toString().trimmed() != QLatin1String("system")) {
        QApplication::setPalette(buildPalette(c));
        app.setStyleSheet(buildStyleSheet(c));
        g_styled = true;
    }
    prepareKonsole(c);
}

QIcon glyph(Glyph kind)
{
    const Colors &c = g_colors;
    const QColor normal = mix(c.fg, c.base, 0.22);
    const QColor onLight = c.base; // on the light round hover of minimise/maximise
    QString stroke;
    QString fill; // filled part when "on" (layout toggles)
    switch (kind) {
    case Glyph::Minimize:
        stroke = QStringLiteral("M4.5 8.5h7");
        break;
    case Glyph::Maximize:
        stroke = QStringLiteral("M4.5 4.5h7v7h-7z");
        break;
    case Glyph::Restore:
        stroke = QStringLiteral("M4.5 6.5h5v5h-5zM6.5 6.5v-2h5v5h-2");
        break;
    case Glyph::Close:
        stroke = QStringLiteral("M5 5l6 6M11 5l-6 6");
        break;
    case Glyph::TabClose:
        stroke = QStringLiteral("M4.5 4.5l7 7M11.5 4.5l-7 7");
        break;
    case Glyph::Sidebar:
        stroke = QStringLiteral("M2.5 3.5h11v9h-11zM6.5 3.5v9");
        fill = QStringLiteral("M2.5 3.5h4v9h-4z");
        break;
    case Glyph::Panel:
        stroke = QStringLiteral("M2.5 3.5h11v9h-11zM2.5 9.5h11");
        fill = QStringLiteral("M2.5 9.5h11v3h-11z");
        break;    case Glyph::ChevronDown:
        stroke = QStringLiteral("M5 6.5l3 3 3-3");
        break;
    }
    const auto svg = [&](const QColor &color, bool filled) {
        QString body;
        if (filled && !fill.isEmpty()) {
            body += QStringLiteral("<path d='%1' fill='%2'/>").arg(fill, color.name());
        }
        body += QStringLiteral("<path d='%1' fill='none' stroke='%2' stroke-width='1.2' stroke-linecap='round' stroke-linejoin='round'/>")
                    .arg(stroke, color.name());
        return QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>%1</svg>").arg(body);
    };
    const QString name = QStringLiteral("glyph-%1").arg(int(kind));
    QIcon icon;
    const bool windowButton = kind == Glyph::Minimize || kind == Glyph::Maximize || kind == Glyph::Restore || kind == Glyph::Close;
    const QColor hover = kind == Glyph::Close ? QColor(Qt::white) : windowButton ? onLight : c.fg;
    const QColor rest = kind == Glyph::TabClose ? mix(c.fg, c.chrome, 0.30) : normal;
    icon.addFile(writeIcon(name + QStringLiteral("-normal"), svg(rest, false)), QSize(16, 16), QIcon::Normal, QIcon::Off);
    icon.addFile(writeIcon(name + QStringLiteral("-hover"), svg(hover, false)), QSize(16, 16), QIcon::Active, QIcon::Off);
    if (!fill.isEmpty()) {
        icon.addFile(writeIcon(name + QStringLiteral("-on"), svg(c.fg, true)), QSize(16, 16), QIcon::Normal, QIcon::On);
        icon.addFile(writeIcon(name + QStringLiteral("-on-hover"), svg(c.fg, true)), QSize(16, 16), QIcon::Active, QIcon::On);
    }
    return icon;
}

void makeCard(QWidget *card)
{
    if (g_styled && card) {
        new CardCorners(card);
    }
}

bool styled()
{
    return g_styled;
}

const Colors &colors()
{
    return g_colors;
}

QString editorThemeName()
{
    return g_editorTheme;
}

QString konsoleProfile(const QString &shell)
{
    if (g_konsoleDir.isEmpty() || shell.isEmpty()) {
        return {};
    }
    QString id = QFileInfo(shell).fileName();
    id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    const QString name = QStringLiteral("Lieve-") + id;
    static QHash<QString, QString> written; // profile name -> shell path
    if (written.value(name) == shell) {
        return name;
    }

    // A copy of the user's default Konsole profile (font, scrollback...) that
    // runs this shell, with the editor's colours and a small margin.
    const QString path = g_konsoleDir + QLatin1Char('/') + name + QStringLiteral(".profile");
    QFile::remove(path);
    {
        KConfig profile(path, KConfig::SimpleConfig);
        const QString userDefault = KConfig(QStringLiteral("konsolerc")).group(QStringLiteral("Desktop Entry")).readEntry("DefaultProfile");
        const QString userProfile =
            userDefault.isEmpty() ? QString() : QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("konsole/") + userDefault);
        if (!userProfile.isEmpty() && !userProfile.startsWith(g_konsoleDir)) {
            KConfig source(userProfile, KConfig::SimpleConfig);
            const QStringList groups = source.groupList();
            for (const QString &group : groups) {
                const KConfigGroup from = source.group(group);
                KConfigGroup to = profile.group(group);
                const auto entries = from.entryMap();
                for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
                    to.writeEntry(it.key(), it.value());
                }
            }
        }
        KConfigGroup general = profile.group(QStringLiteral("General"));
        general.writeEntry("Name", name);
        general.writeEntry("Parent", QStringLiteral("FALLBACK/"));
        general.writeEntry("Command", shell);
        if (g_konsoleColors) {
            general.writeEntry("TerminalMargin", 6);
            profile.group(QStringLiteral("Appearance")).writeEntry("ColorScheme", QStringLiteral("Lieve"));
        }
        if (!profile.sync()) {
            return {};
        }
    }
    written.insert(name, shell);
    return name;
}

QIcon activityIcon(const QIcon &base, int badge)
{
    const QString text = badge > 99 ? QStringLiteral("99+") : QString::number(badge);
    QIcon icon;
    for (const int dpr : {1, 2}) {
        for (const auto state : {QIcon::Off, QIcon::On}) {
            for (const auto mode : {QIcon::Normal, QIcon::Active}) {
                const int size = 24;
                QPixmap pixmap(size * dpr, size * dpr);
                pixmap.setDevicePixelRatio(dpr);
                pixmap.fill(Qt::transparent);
                QPainter p(&pixmap);
                p.setRenderHint(QPainter::Antialiasing);
                // Inactive icons are dimmed; checked or hovered ones are full.
                p.setOpacity(state == QIcon::Off && mode == QIcon::Normal ? 0.55 : 1.0);
                base.paint(&p, QRect(0, 0, size, size));
                p.setOpacity(1.0);
                if (badge > 0) {
                    QFont font = QApplication::font();
                    font.setPixelSize(9);
                    font.setBold(true);
                    p.setFont(font);
                    const int h = 14;
                    const int w = std::max(h, QFontMetrics(font).horizontalAdvance(text) + 7);
                    const QRectF rect(size - w + 2, size - h + 2, w, h);
                    p.setPen(Qt::NoPen);
                    p.setBrush(g_colors.accent.isValid() ? g_colors.accent : QColor(0x3d, 0xae, 0xe9));
                    p.drawRoundedRect(rect, h / 2.0, h / 2.0);
                    p.setPen(g_colors.accentText.isValid() ? g_colors.accentText : QColor(Qt::white));
                    p.drawText(rect, Qt::AlignCenter, text);
                }
                p.end();
                icon.addPixmap(pixmap, mode, state);
            }
        }
    }
    return icon;
}
}
