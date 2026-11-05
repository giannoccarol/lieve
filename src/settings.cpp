#include "settings.h"

#include "jsonc.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
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

QSettings &state()
{
    static QSettings settings(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/lieve/state.ini"),
                              QSettings::IniFormat);
    return settings;
}

namespace
{
// Sections added in later versions: appended to an existing lieve.conf.
const char kProjectsSection[] = R"(
[projects]
; Cartelle in cui cercare i progetti (repository git, anche un livello sotto).
; Vuoto = rilevamento automatico (~/Documenti/Projects, ~/Projects, ~/src, ...).
; Esempio: roots=~/Documenti/Projects, ~/lavoro
roots=
)";

const char kFormatSection[] = R"(
[format]
; Prettier al salvataggio: auto = come VS Code (editor.formatOnSave del progetto
; o tuo), oppure true / false. Serve Prettier installato nel progetto (node_modules).
onSave=auto
; Minuti di inattivita dopo cui il processo di Prettier si chiude.
keepAliveMinutes=10
; Percorso di node. Vuoto = cercato da solo (PATH, mise, volta, nvm, fnm).
node=
)";

const char kGitSection[] = R"(
[git]
; auto = come VS Code (git.enableSmartCommit e git.autofetch), oppure true / false.
; Commit senza file preparati: prepara e include tutte le modifiche.
smartCommit=auto
; Recupera (git fetch) dal server ogni pochi minuti.
autofetch=auto
autofetchMinutes=3
; Alla fine della riga del cursore: autore, data e messaggio del commit (come GitLens).
inlineBlame=true
)";

const char kUiSection[] = R"(
[ui]
; editor = i colori della finestra seguono il tema dell'editor (come VS Code);
; system = colori di Plasma.
theme=editor
; Colore d'accento: system (quello di Plasma), lieve (il bordeaux dell'icona) oppure #rrggbb.
accent=system
; Barra del titolo: lieve (quella di Lieve, con la ricerca) oppure system (quella di Plasma).
titleBar=lieve

[terminal]
; Il terminale usa gli stessi colori dell'editor (profilo Konsole privato di Lieve,
; i tuoi profili di Konsole non vengono toccati).
matchEditor=true
)";

const char kUpdatesSection[] = R"(
[updates]
; Controlla una volta ogni tanto se su GitHub c'e una versione nuova di Lieve
; e la propone nella barra di stato (l'aggiornamento parte solo se lo chiedi).
check=true
)";

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
                    f.write(section.text);
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
    f.write(R"(; Impostazioni di Lieve. Le modifiche valgono dal prossimo avvio.

[editor]
; auto = "VSCodium Dark" con il tema scuro di sistema, "Breeze Light" con quello chiaro.
; Elenco dei temi disponibili: ksyntaxhighlighter6 --list-themes
theme=auto
; Vuoto = font a larghezza fissa di sistema (Impostazioni di sistema > Caratteri).
fontFamily=
fontSize=0
minimap=false
wordWrap=false
; Scorciatoie in stile VS Code (Ctrl+D, Alt+Su/Giu, Ctrl+Shift+K, Ctrl+H, ...).
vscodeKeys=true

[explorer]
; Nomi nascosti in Esplora risorse.
exclude=.git, .svn, .hg, CVS, .DS_Store, Thumbs.db

[search]
; Glob esclusi da ricerca e apertura rapida (i file in .gitignore sono gia esclusi).
exclude=**/node_modules, **/bower_components, **/*.code-search
maxResults=5000

[icons]
; auto = lo stesso tema di icone impostato in VS Code (workbench.iconTheme).
; Altri valori: vscode-icons, material-icon-theme, system
theme=auto
)");
    for (const Section &section : kLateSections) {
        f.write(section.text);
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
