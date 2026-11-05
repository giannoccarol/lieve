#include "settingsdialog.h"

#include "mainwindow.h"
#include "prettier.h"
#include "projects.h"
#include "settings.h"
#include "terminalpanel.h"
#include "theme.h"
#include "updater.h"
#include "windowmanager.h"

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Editor>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>

using namespace Lieve;

namespace
{
QPointer<SettingsDialog> g_dialog;
// The values the running Lieve uses for the settings that need a restart,
// taken when the window is first built.
QHash<QString, QString> g_running;

// "en" -> "English", "it" -> "Italiano": each language in its own name.
QString languageName(const QString &code)
{
    if (code == QLatin1String("en")) {
        return QStringLiteral("English");
    }
    QString name = QLocale(code).nativeLanguageName();
    if (!name.isEmpty()) {
        name[0] = name.at(0).toUpper();
    }
    return name.isEmpty() ? code : name;
}

// The language "auto" picks: the first of the system's that Lieve has.
QString systemLanguage()
{
    const QStringList available = availableLanguages();
    const QStringList wanted = QLocale::system().uiLanguages();
    for (const QString &language : wanted) {
        const QString base = QLocale(language).name().section(QLatin1Char('_'), 0, 0);
        if (available.contains(base)) {
            return base;
        }
    }
    return QStringLiteral("en");
}

// Comma separated lists ("a,b ,c") as lieve.conf writes them: "a, b, c".
QString normalizeList(const QString &text)
{
    QStringList items;
    const QStringList parts = text.split(QLatin1Char(','));
    for (const QString &part : parts) {
        if (!part.trimmed().isEmpty()) {
            items << part.trimmed();
        }
    }
    return items.join(QStringLiteral(", "));
}

QIcon swatch(const QColor &color)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color.darker(140), 2));
    p.setBrush(color);
    p.drawRoundedRect(QRectF(2, 2, 28, 28), 7, 7);
    return QIcon(pixmap);
}

QString css(const QColor &c)
{
    return c.alpha() == 255 ? c.name() : QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t), float(a.blueF() * (1 - t) + b.blueF() * t));
}
}

SettingsDialog *SettingsDialog::showFor(MainWindow *owner, const QString &filter)
{
    if (!g_dialog) {
        g_dialog = new SettingsDialog(owner);
    } else if (owner && g_dialog->m_owner != owner) {
        g_dialog->hide();
        g_dialog->setOwner(owner);
    }
    SettingsDialog *dialog = g_dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    const QString wanted = filter.trimmed().toLower();
    const auto category = std::find_if(dialog->m_categories.cbegin(), dialog->m_categories.cend(), [&wanted](const Category &c) {
        return c.id == wanted;
    });
    if (category != dialog->m_categories.cend()) {
        dialog->m_search->clear();
        const int index = int(category - dialog->m_categories.cbegin());
        QTimer::singleShot(0, dialog, [dialog, index] { // once the rows are laid out
            dialog->scrollToCategory(index);
        });
    } else if (!wanted.isEmpty()) {
        dialog->m_search->setText(filter.trimmed());
    }
    dialog->m_search->setFocus();
    dialog->m_search->selectAll();
    return dialog;
}

// No parent widget: the window outlives the one it was opened from (and is
// never deleted while that one is being torn down), but stays over it.
SettingsDialog::SettingsDialog(MainWindow *owner)
    : QDialog(nullptr)
{
    setWindowTitle(tr("Settings"));
    setAttribute(Qt::WA_QuitOnClose, false); // Lieve quits with its last main window
    resize(860, 600);
    build();
    setOwner(owner);
}

void SettingsDialog::setOwner(MainWindow *owner)
{
    m_owner = owner;
    winId(); // creates windowHandle()
    if (owner && owner->windowHandle()) {
        windowHandle()->setTransientParent(owner->windowHandle());
    }
}

void SettingsDialog::build()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Search box, number of results and the link to the file.
    auto *top = new QHBoxLayout;
    top->setContentsMargins(14, 12, 14, 10);
    top->setSpacing(10);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("settingsSearch"));
    m_search->setPlaceholderText(tr("Search settings"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(QIcon::fromTheme(QStringLiteral("search"), QIcon::fromTheme(QStringLiteral("edit-find"))), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    top->addWidget(m_search, 1);
    m_found = new QLabel(this);
    m_found->setForegroundRole(QPalette::PlaceholderText);
    m_found->hide();
    top->addWidget(m_found);
    auto *file = new QToolButton(this);
    file->setObjectName(QStringLiteral("openConfig"));
    file->setText(tr("Open lieve.conf"));
    file->setToolTip(tr("Edit every setting in the file, with its comments: %1").arg(prettyPath(configFile())));
    file->setIcon(QIcon::fromTheme(QStringLiteral("document-edit"), QIcon::fromTheme(QStringLiteral("text-x-generic"))));
    file->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    file->setAutoRaise(true);
    top->addWidget(file);
    outer->addLayout(top);

    // Categories on the left, every setting on the right in one scrolling list.
    auto *body = new QHBoxLayout;
    body->setContentsMargins(8, 0, 0, 0);
    body->setSpacing(6);
    m_categoryList = new QListWidget(this);
    m_categoryList->setObjectName(QStringLiteral("settingsCategories"));
    m_categoryList->setFrameShape(QFrame::NoFrame);
    m_categoryList->setFixedWidth(176);
    m_categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->addWidget(m_categoryList);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("settingsScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget(m_scroll);
    m_content->setObjectName(QStringLiteral("settingsContent"));
    m_rows = new QVBoxLayout(m_content);
    m_rows->setContentsMargins(4, 0, 18, 18);
    m_rows->setSpacing(2);
    body->addWidget(m_scroll, 1);
    outer->addLayout(body, 1);

    // Shown when a changed setting needs a restart.
    m_restartBar = new QFrame(this);
    m_restartBar->setObjectName(QStringLiteral("restartBar"));
    auto *bar = new QHBoxLayout(m_restartBar);
    bar->setContentsMargins(14, 8, 14, 8);
    auto *restartIcon = new QLabel(m_restartBar);
    restartIcon->setPixmap(QIcon::fromTheme(QStringLiteral("view-refresh")).pixmap(16, 16));
    bar->addWidget(restartIcon);
    bar->addWidget(new QLabel(tr("Some changes take effect after a restart."), m_restartBar), 1);
    auto *restart = new QPushButton(tr("Restart"), m_restartBar);
    restart->setObjectName(QStringLiteral("primaryButton"));
    restart->setAutoDefault(false);
    bar->addWidget(restart);
    m_restartBar->hide();
    outer->addWidget(m_restartBar);

    // ---- General
    addCategory(QStringLiteral("general"), tr("General"));
    {
        QList<QPair<QString, QString>> languages{{tr("Automatic (%1)").arg(languageName(systemLanguage())), QStringLiteral("auto")}};
        const QStringList available = availableLanguages();
        for (const QString &code : available) {
            languages.append({languageName(code), code});
        }
        addChoice(QStringLiteral("ui/language"),
                  QStringLiteral("auto"),
                  Apply::Restart,
                  tr("Language"),
                  tr("Language of Lieve's interface. Automatic follows the system."),
                  languages);
    }
    {
        QLineEdit *roots = addText(QStringLiteral("projects/roots"),
                                   Apply::Now,
                                   tr("Project Folders"),
                                   tr("Folders to look for projects in (git repositories, also one level down), separated by commas."),
                                   [] {
                                       if (!config().value(QStringLiteral("projects/roots")).toStringList().join(QString()).trimmed().isEmpty()) {
                                           return QString();
                                       }
                                       QStringList found; // what an empty value means
                                       const QStringList roots = Projects::roots();
                                       for (const QString &root : roots) {
                                           found << prettyPath(root);
                                       }
                                       return found.isEmpty() ? tr("None detected automatically") : tr("Detected automatically: %1").arg(found.join(QStringLiteral(", ")));
                                   });
        QAction *add = roots->addAction(QIcon::fromTheme(QStringLiteral("folder-open")), QLineEdit::TrailingPosition);
        add->setToolTip(tr("Add Folder…"));
        connect(add, &QAction::triggered, this, [this, roots] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Add Folder"), QDir::homePath());
            if (!dir.isEmpty()) {
                roots->setText(normalizeList(roots->text() + QStringLiteral(", ") + prettyPath(dir)));
                Q_EMIT roots->editingFinished();
            }
        });
    }

    // ---- Appearance
    addCategory(QStringLiteral("appearance"), tr("Appearance"));
    {
        QList<QPair<QString, QString>> themes{{tr("Automatic"), QStringLiteral("auto")}};
        QList<KSyntaxHighlighting::Theme> all = KTextEditor::Editor::instance()->repository().themes();
        std::sort(all.begin(), all.end(), [](const KSyntaxHighlighting::Theme &a, const KSyntaxHighlighting::Theme &b) {
            return a.translatedName().localeAwareCompare(b.translatedName()) < 0;
        });
        for (const KSyntaxHighlighting::Theme &theme : std::as_const(all)) {
            themes.append({theme.translatedName(), theme.name()});
        }
        addChoice(QStringLiteral("editor/theme"),
                  QStringLiteral("auto"),
                  Apply::Restart,
                  tr("Color Theme"),
                  tr("Colors of the editor, which the window follows too. Automatic: VSCodium Dark with a dark system theme, Breeze Light with a light one."),
                  themes,
                  false);
    }
    addChoice(QStringLiteral("ui/theme"),
              QStringLiteral("editor"),
              Apply::Restart,
              tr("Window Colors"),
              tr("The colors of side bar, panels and menus: from the editor's color theme, like VS Code, or Plasma's."),
              {{tr("Editor color theme"), QStringLiteral("editor")}, {tr("System (Plasma)"), QStringLiteral("system")}});
    addAccent();
    addChoice(QStringLiteral("ui/titleBar"),
              QStringLiteral("lieve"),
              Apply::Restart,
              tr("Title Bar"),
              tr("Lieve's own title bar, with the search box, or Plasma's."),
              {{QStringLiteral("Lieve"), QStringLiteral("lieve")}, {tr("System (Plasma)"), QStringLiteral("system")}});
    addChoice(QStringLiteral("icons/theme"),
              QStringLiteral("auto"),
              Apply::Restart,
              tr("File Icon Theme"),
              tr("Icons of files and folders. Automatic uses the icon theme set in VS Code (workbench.iconTheme)."),
              {{tr("Automatic"), QStringLiteral("auto")},
               {QStringLiteral("vscode-icons"), QStringLiteral("vscode-icons")},
               {QStringLiteral("Material Icon Theme"), QStringLiteral("material-icon-theme")},
               {tr("System icons"), QStringLiteral("system")}});

    // ---- Editor
    addCategory(QStringLiteral("editor"), tr("Text Editor"));
    {
        const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QList<QPair<QString, QString>> fonts{{tr("Default (%1)", "font family").arg(fixed.family()), QString()}};
        const QStringList families = QFontDatabase::families();
        for (const QString &family : families) {
            if (QFontDatabase::isFixedPitch(family) && !QFontDatabase::isPrivateFamily(family)) {
                fonts.append({family, family});
            }
        }
        addChoice(QStringLiteral("editor/fontFamily"),
                  QString(),
                  Apply::Editors,
                  tr("Font Family"),
                  tr("Font of the editor. Default is the system fixed-width font (System Settings > Fonts)."),
                  fonts,
                  false);
        addNumber(QStringLiteral("editor/fontSize"),
                  0,
                  0,
                  72,
                  tr("Default (%1)", "font size").arg(fixed.pointSize()),
                  Apply::Editors,
                  tr("Font Size"),
                  tr("Font size of the editor, in points."));
    }
    addCheck(QStringLiteral("editor/minimap"), false, Apply::Editors, tr("Minimap"), tr("Shows a miniature of the file in the scroll bar."));
    addCheck(QStringLiteral("editor/wordWrap"), false, Apply::Editors, tr("Word Wrap"), tr("Wraps long lines at the edge of the editor."));
    addCheck(QStringLiteral("editor/vscodeKeys"),
             true,
             Apply::Restart,
             tr("VS Code Shortcuts"),
             tr("VS Code-style keyboard shortcuts in the editor (Ctrl+D, Alt+Up/Down, Ctrl+Shift+K, Ctrl+H, …)."));

    // ---- Files
    addCategory(QStringLiteral("files"), tr("Files"));
    addCheck(QStringLiteral("files/hotExit"),
             true,
             Apply::Now,
             tr("Hot Exit"),
             tr("Keeps unsaved changes when a window or Lieve is closed, without asking, and brings them back next time."));
    addText(QStringLiteral("explorer/exclude"),
            Apply::Restart,
            tr("Hidden in the Explorer"),
            tr("Names of files and folders hidden in the Explorer, separated by commas."),
            [] {
                return QString();
            });
    addText(QStringLiteral("search/exclude"),
            Apply::Now,
            tr("Excluded from Search"),
            tr("Globs excluded from search and Quick Open, separated by commas (files in .gitignore are excluded already)."),
            [] {
                return QString();
            });
    addNumber(QStringLiteral("search/maxResults"),
              5000,
              100,
              1000000,
              QString(),
              Apply::Restart,
              tr("Search Results Limit"),
              tr("Search stops after this many results."));

    // ---- Terminal
    addCategory(QStringLiteral("terminal"), tr("Terminal"));
    {
        QList<QPair<QString, QString>> shells{{tr("Automatic (%1, your login shell)").arg(TerminalPanel::loginShellName()), QStringLiteral("auto")}};
        const QStringList names = TerminalPanel::shellNames();
        for (const QString &name : names) {
            shells.append({name, name});
        }
        addChoice(QStringLiteral("terminal/shell"),
                  QStringLiteral("auto"),
                  Apply::Now,
                  tr("Default Shell"),
                  tr("Shell for new terminals. Another one can be started from the menu next to the terminal's +."),
                  shells,
                  false);
    }
    addCheck(QStringLiteral("terminal/matchEditor"),
             true,
             Apply::Restart,
             tr("Editor Colors"),
             tr("The terminal uses the same colors as the editor (your Konsole profiles are not touched)."));

    // ---- Git
    addCategory(QStringLiteral("git"), QStringLiteral("Git"));
    addCheck(QStringLiteral("git/inlineBlame"),
             true,
             Apply::Editors,
             tr("Inline Blame"),
             tr("Author, date and message of the last commit at the end of the cursor line, like GitLens."));
    addCheck(QStringLiteral("git/gutter"), true, Apply::Restart, tr("Change Marks"), tr("Marks added, changed and deleted lines next to the line numbers."));
    addAutoOnOff(QStringLiteral("git/smartCommit"),
                 Apply::Now,
                 tr("Smart Commit"),
                 tr("Committing with no staged changes stages and commits all of them. Automatic follows VS Code (git.enableSmartCommit)."));
    addAutoOnOff(QStringLiteral("git/autofetch"),
                 Apply::Restart,
                 tr("Autofetch"),
                 tr("Fetches from the remote every few minutes. Automatic follows VS Code (git.autofetch)."));
    addNumber(QStringLiteral("git/autofetchMinutes"),
              0,
              0,
              1440,
              tr("Automatic"),
              Apply::Restart,
              tr("Autofetch Interval"),
              tr("Minutes between two fetches. Automatic follows VS Code (git.autofetchPeriod, 3 minutes)."));

    // ---- Formatting
    addCategory(QStringLiteral("formatting"), tr("Formatting"));
    addAutoOnOff(QStringLiteral("format/onSave"),
                 Apply::Now,
                 tr("Format on Save"),
                 tr("Formats with Prettier when saving, if the project has Prettier installed. Automatic follows VS Code (editor.formatOnSave)."));
    addAutoOnOff(QStringLiteral("format/eslint"),
                 Apply::Now,
                 tr("ESLint Fix on Save"),
                 tr("Fixes ESLint problems when saving. Automatic follows VS Code (editor.codeActionsOnSave)."));
    addText(QStringLiteral("format/node"), Apply::Now, tr("Node.js Path"), tr("The node program that runs Prettier. Empty: found automatically (PATH, mise, volta, nvm, fnm)."), [] {
        if (!config().value(QStringLiteral("format/node")).toString().trimmed().isEmpty()) {
            return QString();
        }
        const QString node = findNode();
        return node.isEmpty() ? tr("Not found") : tr("Found automatically: %1").arg(prettyPath(node));
    });
    addNumber(QStringLiteral("format/keepAliveMinutes"),
              10,
              1,
              1440,
              QString(),
              Apply::Now,
              tr("Prettier Idle Time"),
              tr("Minutes of inactivity after which the Prettier process exits."));

    // ---- Languages
    addCategory(QStringLiteral("languages"), tr("Languages"));
    addChoice(QStringLiteral("lsp/enabled"),
              QStringLiteral("auto"),
              Apply::Restart,
              tr("Language Servers"),
              tr("Go to definition, errors and completion from language servers. Automatic uses the servers it finds installed."),
              {{tr("Automatic"), QStringLiteral("auto")}, {tr("Off"), QStringLiteral("false")}});

    // ---- Updates
    addCategory(QStringLiteral("updates"), tr("Updates"));
    addCheck(QStringLiteral("updates/check"),
             true,
             Apply::Restart,
             tr("Check for Updates"),
             tr("Checks now and then for a new version of Lieve on GitHub and offers it in the status bar. It only updates when you ask."));

    m_nothing = new QLabel(tr("No settings found."), m_content);
    m_nothing->setForegroundRole(QPalette::PlaceholderText);
    m_nothing->setAlignment(Qt::AlignCenter);
    m_nothing->setContentsMargins(0, 40, 0, 0);
    m_nothing->hide();
    m_rows->addWidget(m_nothing);
    m_rows->addStretch(1);
    m_scroll->setWidget(m_content);

    // Before reload() re-reads the file: config() still holds what Lieve read
    // when it started (or last wrote), even if lieve.conf was edited by hand since.
    for (Setting &s : m_settings) {
        const QString path = s.section + QLatin1Char('/') + s.key;
        if (s.apply == Apply::Restart && !g_running.contains(path)) {
            g_running.insert(path, value(s));
        }
    }

    // Lieve's look: the colours of the window's cards, lists and inputs.
    if (Theme::styled()) {
        const Colors &c = Theme::colors();
        QString sheet = QStringLiteral(R"(
QLineEdit#settingsSearch { background: @input; color: @fg; border: 1px solid @inputBorder; border-radius: 4px; padding: 5px 6px; }
QLineEdit#settingsSearch:focus { border: 1px solid @accent; }
QListWidget#settingsCategories { background: transparent; border: none; outline: 0; }
QListWidget#settingsCategories::item { padding: 5px 10px; margin: 1px 0px; border-radius: 5px; color: @dim; }
QListWidget#settingsCategories::item:hover { background: @hover; color: @fg; }
QListWidget#settingsCategories::item:selected { background: @selection; color: @fg; }
QScrollArea#settingsScroll, QWidget#settingsContent { background: transparent; }
QWidget#settingRow { border-radius: 6px; }
QWidget#settingRow[current="true"] { background: @hover; }
QToolButton#openConfig { border: none; border-radius: 4px; padding: 4px 8px; color: @fg; }
QToolButton#openConfig:hover { background: @hover; }
QFrame#restartBar { background: @elevated; border-top: 1px solid @border; }
)");
        const QList<QPair<QString, QString>> values = {
            {QStringLiteral("@inputBorder"), css(c.inputBorder)},
            {QStringLiteral("@input"), css(c.input)},
            {QStringLiteral("@fg"), css(c.fg)},
            {QStringLiteral("@dim"), css(c.dim)},
            {QStringLiteral("@hover"), css(c.hover)},
            {QStringLiteral("@accent"), css(c.accent)},
            {QStringLiteral("@selection"), css(mix(c.chrome, c.accent, c.dark ? 0.30 : 0.16))},
            {QStringLiteral("@elevated"), css(c.elevated)},
            {QStringLiteral("@border"), css(c.border)},
        };
        for (const auto &[name, color] : values) {
            sheet.replace(name, color);
        }
        setStyleSheet(sheet);
    }

    connect(m_search, &QLineEdit::textChanged, this, &SettingsDialog::applyFilter);
    connect(m_categoryList, &QListWidget::currentRowChanged, this, [this](int row) {
        for (qsizetype i = 0; i < m_categories.size(); ++i) {
            if (m_categories.at(i).item == row) {
                scrollToCategory(int(i));
            }
        }
    });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, &SettingsDialog::syncCategory);
    connect(file, &QToolButton::clicked, this, &SettingsDialog::openConfigFile);
    connect(restart, &QPushButton::clicked, this, &SettingsDialog::restartLieve);
    // The row with the focus is highlighted, like VS Code's.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (Setting &s : m_settings) {
            const bool current = now && s.row->isAncestorOf(now);
            if (s.row->property("current").toBool() != current) {
                s.row->setProperty("current", current);
                s.row->style()->unpolish(s.row);
                s.row->style()->polish(s.row);
            }
        }
    });
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    reload();
}

void SettingsDialog::addCategory(const QString &id, const QString &title)
{
    Category category;
    category.id = id;
    category.header = new QLabel(title, m_content);
    category.header->setObjectName(QStringLiteral("settingsCategory"));
    QFont font = category.header->font();
    font.setPointSizeF(font.pointSizeF() * 1.3);
    font.setWeight(QFont::DemiBold);
    category.header->setFont(font);
    category.header->setContentsMargins(12, m_categories.isEmpty() ? 4 : 22, 12, 4);
    m_rows->addWidget(category.header);
    auto *item = new QListWidgetItem(title, m_categoryList);
    category.item = m_categoryList->row(item);
    m_categories.append(category);
}

SettingsDialog::Setting &SettingsDialog::addRow(const QString &path,
                                                const QString &fallback,
                                                Apply apply,
                                                const QString &title,
                                                const QString &description,
                                                QWidget *control,
                                                const QStringList &values)
{
    Setting &s = m_settings.emplace_back();
    s.section = path.section(QLatin1Char('/'), 0, 0);
    s.key = path.section(QLatin1Char('/'), 1);
    s.fallback = fallback;
    s.apply = apply;
    s.category = int(m_categories.size()) - 1;
    s.control = control;
    s.comment = description;
    if (!values.isEmpty()) {
        s.comment += QLatin1Char(' ') + tr("Values: %1.").arg(values.join(QStringLiteral(", ")));
    }
    s.haystack = QStringList{title, description, s.section + QLatin1Char('.') + s.key, s.key}.join(QLatin1Char(' ')).toLower();

    s.row = new QWidget(m_content);
    s.row->setObjectName(QStringLiteral("settingRow"));
    s.row->setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(s.row);
    v->setContentsMargins(12, 8, 12, 10);
    v->setSpacing(4);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(10);
    auto *titleLabel = new QLabel(title, s.row);
    QFont bold = titleLabel->font();
    bold.setWeight(QFont::DemiBold);
    titleLabel->setFont(bold);
    heading->addWidget(titleLabel);
    if (apply == Apply::Restart) {
        s.restartNote = new QLabel(tr("Restart Lieve to apply"), s.row);
        s.restartNote->setForegroundRole(QPalette::Link);
        s.restartNote->hide();
        heading->addWidget(s.restartNote);
    }
    heading->addStretch(1);
    v->addLayout(heading);

    auto *descriptionLabel = new QLabel(description, s.row);
    descriptionLabel->setWordWrap(true);
    descriptionLabel->setForegroundRole(QPalette::PlaceholderText);
    control->setParent(s.row);
    if (control->objectName().isEmpty()) {
        control->setObjectName(path);
    }
    control->setAccessibleName(title);
    control->setAccessibleDescription(description);
    // Scrolling the list over a combo or spin box must not change it.
    QList<QWidget *> wheelers = control->findChildren<QWidget *>();
    wheelers << control;
    for (QWidget *w : std::as_const(wheelers)) {
        if (qobject_cast<QComboBox *>(w) || qobject_cast<QAbstractSpinBox *>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(this);
        }
    }
    if (auto *check = qobject_cast<QCheckBox *>(control)) {
        // VS Code style: the box, then the description as its label.
        auto *line = new QHBoxLayout;
        line->setSpacing(8);
        line->addWidget(check, 0, Qt::AlignTop);
        line->addWidget(descriptionLabel, 1);
        descriptionLabel->setProperty("toggles", QVariant::fromValue<QObject *>(check));
        descriptionLabel->installEventFilter(this);
        v->addLayout(line);
    } else {
        v->addWidget(descriptionLabel);
        auto *line = new QHBoxLayout;
        control->setMinimumWidth(240);
        control->setMaximumWidth(360);
        line->addWidget(control, 1);
        line->addStretch(0);
        line->setContentsMargins(0, 2, 0, 0);
        v->addLayout(line);
    }
    m_rows->addWidget(s.row);
    return s;
}

void SettingsDialog::addCheck(const QString &path, bool fallback, Apply apply, const QString &title, const QString &description)
{
    auto *check = new QCheckBox;
    Setting &s = addRow(path, fallback ? QStringLiteral("true") : QStringLiteral("false"), apply, title, description, check);
    s.show = [check](const QString &value) {
        const QSignalBlocker blocker(check);
        check->setChecked(QVariant(value).toBool());
    };
    connect(check, &QCheckBox::toggled, this, [this, s = &s](bool on) {
        write(*s, on ? QStringLiteral("true") : QStringLiteral("false"));
    });
}

void SettingsDialog::addChoice(const QString &path,
                               const QString &fallback,
                               Apply apply,
                               const QString &title,
                               const QString &description,
                               const QList<QPair<QString, QString>> &choices,
                               bool listValues)
{
    auto *combo = new QComboBox;
    QStringList values;
    for (const auto &[text, value] : choices) {
        combo->addItem(text, value);
        values << value;
    }
    combo->setMaxVisibleItems(16);
    Setting &s = addRow(path, fallback, apply, title, description, combo, listValues ? values : QStringList());
    s.show = [combo](const QString &value) {
        const QSignalBlocker blocker(combo);
        int index = combo->findData(value);
        if (index < 0) {
            index = combo->findData(value.toLower());
        }
        if (index < 0) { // something else written in lieve.conf: keep it
            combo->addItem(value, value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(index);
    };
    connect(combo, &QComboBox::currentIndexChanged, this, [this, s = &s, combo](int index) {
        if (index >= 0) {
            write(*s, combo->itemData(index).toString());
        }
    });
}

void SettingsDialog::addAutoOnOff(const QString &path, Apply apply, const QString &title, const QString &description)
{
    addChoice(path,
              QStringLiteral("auto"),
              apply,
              title,
              description,
              {{tr("Automatic"), QStringLiteral("auto")}, {tr("On"), QStringLiteral("true")}, {tr("Off"), QStringLiteral("false")}});
    Setting &s = m_settings.back();
    const auto show = s.show;
    s.show = [show](const QString &value) { // "on" and "off" mean the same
        const QString v = value.toLower();
        show(v == QLatin1String("on") ? QStringLiteral("true") : v == QLatin1String("off") ? QStringLiteral("false") : v.isEmpty() ? QStringLiteral("auto") : v);
    };
}

void SettingsDialog::addNumber(const QString &path,
                               int fallback,
                               int minimum,
                               int maximum,
                               const QString &zeroText,
                               Apply apply,
                               const QString &title,
                               const QString &description)
{
    auto *spin = new QSpinBox;
    spin->setRange(minimum, maximum);
    spin->setSpecialValueText(zeroText); // shown for the minimum
    spin->setKeyboardTracking(false); // typed digits count on Enter or when leaving
    Setting &s = addRow(path, QString::number(fallback), apply, title, description, spin);
    spin->setMinimumWidth(140);
    spin->setMaximumWidth(180);
    s.show = [spin](const QString &value) {
        const QSignalBlocker blocker(spin);
        spin->setValue(qRound(value.toDouble()));
    };
    connect(spin, &QSpinBox::valueChanged, this, [this, s = &s](int value) {
        write(*s, QString::number(value));
    });
}

QLineEdit *SettingsDialog::addText(const QString &path, Apply apply, const QString &title, const QString &description, const std::function<QString()> &placeholder)
{
    auto *edit = new QLineEdit;
    edit->setClearButtonEnabled(true);
    Setting &s = addRow(path, QString(), apply, title, description, edit);
    // Lists keep the defaults the code uses when the key is missing.
    if (path == QLatin1String("explorer/exclude")) {
        s.fallback = QStringLiteral(".git, .svn, .hg, CVS, .DS_Store, Thumbs.db");
    } else if (path == QLatin1String("search/exclude")) {
        s.fallback = QStringLiteral("**/node_modules, **/bower_components");
    }
    const bool list = path != QLatin1String("format/node");
    edit->setMaximumWidth(560); // lists: room for a few items
    s.show = [edit, placeholder](const QString &value) {
        const QSignalBlocker blocker(edit);
        if (edit->text() != value) {
            edit->setText(value);
            edit->setCursorPosition(0); // the start of a long list
        }
        edit->setPlaceholderText(placeholder());
    };
    connect(edit, &QLineEdit::editingFinished, this, [this, s = &s, edit, list] {
        const QString text = list ? normalizeList(edit->text()) : edit->text().trimmed();
        write(*s, text);
        refresh(*s); // normalized text, new placeholder
    });
    return edit;
}

void SettingsDialog::addAccent()
{
    auto *box = new QWidget;
    box->setObjectName(QStringLiteral("accentBox"));
    auto *h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto *combo = new QComboBox(box);
    combo->setObjectName(QStringLiteral("ui/accent"));
    combo->addItem(tr("System (Plasma's)"), QStringLiteral("system"));
    combo->addItem(tr("Lieve (burgundy)"), QStringLiteral("lieve"));
    combo->addItem(tr("Custom…"), QStringLiteral("custom"));
    h->addWidget(combo, 1);
    auto *pick = new QToolButton(box);
    pick->setObjectName(QStringLiteral("accentColor"));
    pick->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pick->setToolTip(tr("Choose the color"));
    h->addWidget(pick);
    Setting &s = addRow(QStringLiteral("ui/accent"),
                        QStringLiteral("system"),
                        Apply::Restart,
                        tr("Accent Color"),
                        tr("Color of buttons, selections and focus frames."),
                        box,
                        {QStringLiteral("system"), QStringLiteral("lieve"), QStringLiteral("#rrggbb")});
    s.control = combo;
    s.show = [combo, pick](const QString &value) {
        const QSignalBlocker blocker(combo);
        const QColor custom = value.startsWith(QLatin1Char('#')) ? QColor::fromString(value) : QColor();
        combo->setCurrentIndex(custom.isValid() ? 2 : value.toLower() == QLatin1String("lieve") ? 1 : 0);
        pick->setVisible(custom.isValid());
        if (custom.isValid()) {
            pick->setIcon(swatch(custom));
            pick->setText(custom.name());
        }
    };
    const auto choose = [this, s = &s] {
        const QColor current = value(*s).startsWith(QLatin1Char('#')) ? QColor::fromString(value(*s)) : Theme::colors().accent;
        const QColor color = QColorDialog::getColor(current.isValid() ? current : QColor(0x3d, 0xae, 0xe9), this, tr("Accent Color"));
        if (color.isValid()) {
            write(*s, color.name());
        }
        refresh(*s); // cancelled: back to the current choice
    };
    connect(combo, &QComboBox::currentIndexChanged, this, [this, s = &s, combo, choose](int index) {
        const QString choice = combo->itemData(index).toString();
        if (choice == QLatin1String("custom")) {
            choose();
        } else if (index >= 0) {
            write(*s, choice);
        }
    });
    connect(pick, &QToolButton::clicked, this, choose);
}

QString SettingsDialog::value(const Setting &s) const
{
    const QVariant v = config().value(s.section + QLatin1Char('/') + s.key, s.fallback);
    if (v.typeId() == QMetaType::QStringList) { // a, b, c
        return v.toStringList().join(QStringLiteral(", ")).trimmed();
    }
    return v.toString().trimmed();
}

void SettingsDialog::write(Setting &s, const QString &value)
{
    if (value == this->value(s)) {
        return;
    }
    if (!setConfigValue(s.section, s.key, value, s.comment)) {
        QMessageBox::warning(this, tr("Settings"), tr("Could not save the setting in %1.").arg(configFile()));
    }
    refresh(s);
    if (s.apply == Apply::Editors) {
        const auto windows = WindowManager::instance().windows();
        for (MainWindow *window : windows) {
            window->applySettings();
        }
    }
    updateRestartBar();
}

void SettingsDialog::refresh(Setting &s)
{
    const QString current = value(s);
    s.show(current);
    if (s.restartNote) {
        s.restartNote->setVisible(current != g_running.value(s.section + QLatin1Char('/') + s.key));
    }
}

void SettingsDialog::reload()
{
    config().sync(); // lieve.conf may have been edited by hand
    for (Setting &s : m_settings) {
        refresh(s);
    }
    updateRestartBar();
}

void SettingsDialog::updateRestartBar()
{
    const bool needed = std::any_of(m_settings.cbegin(), m_settings.cend(), [](const Setting &s) {
        return s.restartNote && !s.restartNote->isHidden();
    });
    m_restartBar->setVisible(needed);
}

void SettingsDialog::applyFilter(const QString &filter)
{
    const QStringList words = filter.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QList<int> perCategory(m_categories.size(), 0);
    int found = 0;
    for (Setting &s : m_settings) {
        const bool match = std::all_of(words.cbegin(), words.cend(), [&s](const QString &word) {
            return s.haystack.contains(word);
        });
        s.row->setVisible(match);
        if (match) {
            ++perCategory[s.category];
            ++found;
        }
    }
    for (qsizetype i = 0; i < m_categories.size(); ++i) {
        m_categories[i].header->setVisible(perCategory.at(i) > 0);
        m_categoryList->item(m_categories.at(i).item)->setHidden(perCategory.at(i) == 0);
    }
    m_nothing->setVisible(found == 0);
    m_found->setVisible(!words.isEmpty() && found > 0);
    m_found->setText(found == 1 ? tr("1 setting found") : tr("%1 settings found").arg(found));
    m_scroll->verticalScrollBar()->setValue(0);
    syncCategory();
}

void SettingsDialog::scrollToCategory(int index)
{
    const Category &category = m_categories.at(index);
    if (category.header->isHidden()) {
        return;
    }
    m_scrolling = true;
    m_scroll->verticalScrollBar()->setValue(category.header->y());
    m_scrolling = false;
    const QSignalBlocker blocker(m_categoryList);
    m_categoryList->setCurrentRow(category.item);
}

// Selects on the left the category at the top of the list.
void SettingsDialog::syncCategory()
{
    if (m_scrolling) {
        return;
    }
    const int top = m_scroll->verticalScrollBar()->value() + 24;
    int current = -1;
    for (const Category &category : std::as_const(m_categories)) {
        if (category.header->isHidden()) {
            continue;
        }
        if (current < 0 || category.header->y() <= top) {
            current = category.item;
        }
    }
    const QSignalBlocker blocker(m_categoryList);
    m_categoryList->setCurrentRow(current);
}

void SettingsDialog::focusFirstControl()
{
    for (const Setting &s : m_settings) {
        if (!s.row->isHidden()) {
            s.control->setFocus(Qt::TabFocusReason);
            m_scroll->ensureWidgetVisible(s.row);
            return;
        }
    }
}

void SettingsDialog::restartLieve()
{
    hide();
    Updater::instance().restart();
}

void SettingsDialog::openConfigFile()
{
    MainWindow *window = m_owner ? m_owner.data() : WindowManager::instance().windows().value(0);
    hide();
    if (window) {
        ensureConfigFile();
        window->openFile(configFile());
        WindowManager::activate(window);
    }
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    reload();
    QDialog::showEvent(event);
}

void SettingsDialog::changeEvent(QEvent *event)
{
    // Back from another window (where lieve.conf may have changed): show the file's values.
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && isVisible()) {
        reload();
    }
    QDialog::changeEvent(event);
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Down) {
        focusFirstControl();
        return true;
    }
    if (event->type() == QEvent::Wheel && watched->isWidgetType() && !static_cast<QWidget *>(watched)->hasFocus()) {
        QCoreApplication::sendEvent(m_scroll->verticalScrollBar(), event); // scroll the list instead
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease) { // the description of a checkbox toggles it
        if (auto *check = qobject_cast<QCheckBox *>(watched->property("toggles").value<QObject *>())) {
            check->setFocus(Qt::MouseFocusReason);
            check->toggle();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
