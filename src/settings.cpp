#include "settings.h"

#include "jsonc.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPalette>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace Lieve
{
QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lieve/lieve.conf");
}

QSettings &config()
{
    static QSettings settings(configFile(), QSettings::IniFormat);
    return settings;
}

QStringList availableLanguages()
{
    QStringList languages{QStringLiteral("en")};
    const QStringList files = QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("lieve_*.qm")}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        languages << file.mid(6, file.size() - 9); // lieve_it.qm -> it
    }
    return languages;
}

QString uiLanguage()
{
    const QString setting = config().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString().trimmed();
    const QStringList wanted = setting.isEmpty() || setting == QLatin1String("auto") ? QLocale::system().uiLanguages() : QStringList{setting};
    const QStringList available = availableLanguages();
    for (const QString &language : wanted) {
        const QString base = QLocale(language).name().section(QLatin1Char('_'), 0, 0);
        if (available.contains(base)) {
            return base;
        }
    }
    return QStringLiteral("en");
}

bool setConfigValue(const QString &section, const QString &key, const QString &value, const QString &comment)
{
    ensureConfigFile();
    QStringList lines;
    {
        QFile file(configFile());
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        }
    }
    const QString header = QLatin1Char('[') + section + QLatin1Char(']');
    const QRegularExpression keyLine(QStringLiteral("^\\s*%1\\s*=").arg(QRegularExpression::escape(key)));
    const QString entry = key + QLatin1Char('=') + value;
    QStringList added;
    if (!comment.isEmpty()) {
        added << QStringLiteral("; ") + comment;
    }
    added << entry;

    const qsizetype start = lines.indexOf(header);
    if (start < 0) {
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
            lines.removeLast();
        }
        lines << QString() << header << added << QString();
    } else {
        qsizetype end = start + 1;
        while (end < lines.size() && !lines.at(end).trimmed().startsWith(QLatin1Char('['))) {
            ++end;
        }
        qsizetype found = -1;
        qsizetype last = start; // last non-empty line of the section
        for (qsizetype i = start + 1; i < end; ++i) {
            if (keyLine.match(lines.at(i)).hasMatch()) {
                found = i;
                break;
            }
            if (!lines.at(i).trimmed().isEmpty()) {
                last = i;
            }
        }
        if (found >= 0) {
            lines[found] = entry;
        } else {
            for (qsizetype k = 0; k < added.size(); ++k) {
                lines.insert(last + 1 + k, added.at(k));
            }
        }
    }

    QSaveFile out(configFile());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    out.write(lines.join(QLatin1Char('\n')).toUtf8());
    if (!out.commit()) {
        return false;
    }
    config().sync();
    return true;
}

QSettings &state()
{
    static QSettings settings(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/lieve/state.ini"),
                              QSettings::IniFormat);
    return settings;
}

namespace
{
// The commented template of lieve.conf, written in the interface language
// (the Italian translation of each chunk has the same keys, values and
// section headers: only the "; ..." comments change).
const char kBaseSections[] = QT_TRANSLATE_NOOP("Settings", R"(; Lieve settings. Changes take effect the next time Lieve starts.

[editor]
; auto = "VSCodium Dark" with the dark system theme, "Breeze Light" with the light one.
; List of the available themes: ksyntaxhighlighter6 --list-themes
theme=auto
; Empty = the system fixed-width font (System Settings > Fonts).
fontFamily=
fontSize=0
minimap=false
wordWrap=false
; VS Code-style shortcuts (Ctrl+D, Alt+Up/Down, Ctrl+Shift+K, Ctrl+H, ...).
vscodeKeys=true

[explorer]
; Names hidden in the Explorer.
exclude=.git, .svn, .hg, CVS, .DS_Store, Thumbs.db

[search]
; Globs excluded from search and Quick Open (files in .gitignore are already excluded).
exclude=**/node_modules, **/bower_components, **/*.code-search
maxResults=5000

[icons]
; auto = the same icon theme set in VS Code (workbench.iconTheme).
; Other values: vscode-icons, material-icon-theme, system
theme=auto
)");

// Sections added in later versions: appended to an existing lieve.conf.
const char kProjectsSection[] = QT_TRANSLATE_NOOP("Settings", R"(
[projects]
; Folders to look for projects in (git repositories, also one level down).
; Empty = detected automatically (~/Documents/Projects, ~/Projects, ~/src, ...).
; Example: roots=~/Documents/Projects, ~/work
roots=
)");

const char kFormatSection[] = QT_TRANSLATE_NOOP("Settings", R"(
[format]
; Prettier on save: auto = like VS Code (the project's or your editor.formatOnSave),
; or true / false. Needs Prettier installed in the project (node_modules).
onSave=auto
; Minutes of inactivity after which the Prettier process exits.
keepAliveMinutes=10
; Path to node. Empty = found automatically (PATH, mise, volta, nvm, fnm).
node=
)");

const char kGitSection[] = QT_TRANSLATE_NOOP("Settings", R"(
[git]
; auto = like VS Code (git.enableSmartCommit and git.autofetch), or true / false.
; Commit with no staged files: stages and includes all the changes.
smartCommit=auto
; Fetches (git fetch) from the server every few minutes.
autofetch=auto
autofetchMinutes=3
; At the end of the cursor line: author, date and message of the commit (like GitLens).
inlineBlame=true
)");

const char kUiSection[] = QT_TRANSLATE_NOOP("Settings", R"(
[ui]
; editor = the window colors follow the editor theme (like VS Code);
; system = Plasma colors.
theme=editor
; Accent color: system (Plasma's), lieve (the burgundy of the icon) or #rrggbb.
accent=system
; Title bar: lieve (Lieve's own, with the search box) or system (Plasma's).
titleBar=lieve
; Interface language: auto (the system's), en, it.
language=auto

[terminal]
; The terminal uses the same colors as the editor (a Konsole profile of Lieve's own,
; your Konsole profiles are not touched).
matchEditor=true
; Shell for new terminals: auto (your login shell, e.g. fish) or fish, zsh,
; bash or a path. It can also be chosen from the menu next to the terminal's +.
shell=auto
)");

const char kUpdatesSection[] = QT_TRANSLATE_NOOP("Settings", R"(
[updates]
; Checks every now and then whether a new version of Lieve is out on GitHub
; and offers it in the status bar (it only updates when you ask).
check=true
)");

struct Section {
    const char *header;
    const char *text;
};
constexpr Section kLateSections[] = {{"[projects]", kProjectsSection}, {"[format]", kFormatSection}, {"[git]", kGitSection}, {"[ui]", kUiSection}, {"[updates]", kUpdatesSection}};
}

void ensureConfigFile()
{
    const QString path = configFile();
    if (QFileInfo::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite | QIODevice::Text)) {
            const QByteArray current = f.readAll();
            for (const Section &section : kLateSections) {
                if (!current.contains(section.header)) {
                    f.seek(f.size());
                    f.write(QCoreApplication::translate("Settings", section.text).toUtf8());
                }
            }
        }
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    f.write(QCoreApplication::translate("Settings", kBaseSections).toUtf8());
    for (const Section &section : kLateSections) {
        f.write(QCoreApplication::translate("Settings", section.text).toUtf8());
    }
}

bool isDarkTheme()
{
    return qApp->palette().color(QPalette::Window).lightness() < 128;
}

QJsonValue vscodeSetting(const QString &projectRoot, const QString &key)
{
    if (!projectRoot.isEmpty()) {
        const QJsonObject workspace = readJsonc(projectRoot + QStringLiteral("/.vscode/settings.json"));
        if (workspace.contains(key)) {
            return workspace.value(key);
        }
    }
    return vscodeUserSettings().value(key);
}

bool configOrVscode(const QString &configKey, const QString &projectRoot, const QString &vscodeKey, bool fallback)
{
    const QString mode = config().value(configKey, QStringLiteral("auto")).toString().trimmed().toLower();
    if (mode == QLatin1String("true") || mode == QLatin1String("on")) {
        return true;
    }
    if (mode == QLatin1String("false") || mode == QLatin1String("off")) {
        return false;
    }
    const QJsonValue value = vscodeSetting(projectRoot, vscodeKey);
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) { // e.g. git.autofetch: "all"
        return !value.toString().isEmpty() && value.toString() != QLatin1String("false");
    }
    return fallback;
}

QJsonObject vscodeUserSettings()
{
    static const QJsonObject settings = [] {
        const QString c = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        for (const QString &dir : {QStringLiteral("/Code"), QStringLiteral("/Code - OSS"), QStringLiteral("/VSCodium")}) {
            const QString path = c + dir + QStringLiteral("/User/settings.json");
            if (QFileInfo::exists(path)) {
                return readJsonc(path);
            }
        }
        return QJsonObject();
    }();
    return settings;
}
}
