#pragma once

#include <QJsonObject>
#include <QSettings>
#include <QString>

namespace Lieve
{
// User preferences: ~/.config/lieve/lieve.conf (only read, never rewritten,
// so the comments in it survive).
QString configFile();
QSettings &config();

// Window layout and open files: ~/.local/state/lieve/state.ini
QSettings &state();

// Creates lieve.conf with commented defaults if it does not exist yet.
void ensureConfigFile();

bool isDarkTheme();

// The user's VS Code settings.json (Code, Code - OSS or VSCodium), read once.
QJsonObject vscodeUserSettings();
// A VS Code setting: the project's .vscode/settings.json first, then the user's.
QJsonValue vscodeSetting(const QString &projectRoot, const QString &key);
// "auto" (or empty) follows the VS Code setting; "true"/"false" override it.
bool configOrVscode(const QString &configKey, const QString &projectRoot, const QString &vscodeKey, bool fallback);
}
