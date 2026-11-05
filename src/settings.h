#pragma once

#include <QJsonObject>
#include <QSettings>
#include <QString>

namespace Lieve
{
// User preferences: ~/.config/lieve/lieve.conf. QSettings only reads it, so
// the comments in it survive.
QString configFile();
QSettings &config();
// Changes one line of lieve.conf in place (adds it, with `comment` above it,
// when missing) and reloads config().
bool setConfigValue(const QString &section, const QString &key, const QString &value, const QString &comment = QString());

// Window layout and open files: ~/.local/state/lieve/state.ini
QSettings &state();

// Creates lieve.conf with commented defaults if it does not exist yet.
void ensureConfigFile();

bool isDarkTheme();

// Languages Lieve's interface is translated into ("en" is the original).
QStringList availableLanguages();
// The interface language: [ui] language= ("auto" = the system's, the first
// of its preferred languages Lieve has), e.g. "it"; "en" when none matches.
QString uiLanguage();

// The user's VS Code settings.json (Code, Code - OSS or VSCodium), read once.
QJsonObject vscodeUserSettings();
// A VS Code setting: the project's .vscode/settings.json first, then the user's.
QJsonValue vscodeSetting(const QString &projectRoot, const QString &key);
// "auto" (or empty) follows the VS Code setting; "true"/"false" override it.
bool configOrVscode(const QString &configKey, const QString &projectRoot, const QString &vscodeKey, bool fallback);
}
