#include "vscodeicons.h"

#include "jsonc.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QStandardPaths>

namespace
{
QStringList builtinExtensionRoots()
{
    return {QStringLiteral("/usr/share/code/resources/app/extensions"),
            QStringLiteral("/opt/visual-studio-code/resources/app/extensions"),
            QStringLiteral("/usr/lib/code/extensions"),
            QStringLiteral("/opt/vscodium-bin/resources/app/extensions"),
            QStringLiteral("/usr/share/vscodium/resources/app/extensions"),
            QStringLiteral("/usr/lib/vscodium/resources/app/extensions")};
}

QStringList userExtensionRoots()
{
    return {QDir::homePath() + QStringLiteral("/.vscode/extensions"), QDir::homePath() + QStringLiteral("/.vscode-oss/extensions")};
}

// Extension folders in VS Code's load order: built-in ones first, then the
// ones the user installed (only the versions VS Code currently uses).
QStringList extensionDirs()
{
    QStringList dirs;
    for (const QString &root : builtinExtensionRoots()) {
        if (!QFileInfo::exists(root)) {
            continue;
        }
        const QStringList entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &e : entries) {
            dirs << root + QLatin1Char('/') + e;
        }
        break;
    }

    for (const QString &root : userExtensionRoots()) {
        const QJsonDocument doc = readJsoncDocument(root + QStringLiteral("/extensions.json"));
        if (doc.isArray()) {
            const QJsonArray list = doc.array();
            for (const QJsonValue &v : list) {
                const QJsonObject entry = v.toObject();
                const QJsonObject location = entry.value(QLatin1String("location")).toObject();
                QString path = location.value(QLatin1String("path")).toString();
                if (path.isEmpty()) {
                    path = location.value(QLatin1String("fsPath")).toString();
                }
                if (path.isEmpty()) {
                    const QString rel = entry.value(QLatin1String("relativeLocation")).toString();
                    if (!rel.isEmpty()) {
                        path = root + QLatin1Char('/') + rel;
                    }
                }
                if (!path.isEmpty() && QFileInfo::exists(path + QStringLiteral("/package.json"))) {
                    dirs << path;
                }
            }
        } else if (QFileInfo::exists(root)) {
            const QJsonObject obsolete = readJsonc(root + QStringLiteral("/.obsolete"));
            const QStringList entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &e : entries) {
                if (!obsolete.contains(e)) {
                    dirs << root + QLatin1Char('/') + e;
                }
            }
        }
    }
    return dirs;
}
}

VsCodeIcons &VsCodeIcons::instance()
{
    static VsCodeIcons icons;
    return icons;
}

VsCodeIcons::VsCodeIcons()
{
    const QJsonObject userSettings = Lieve::vscodeUserSettings();

    const QStringList dirs = extensionDirs();
    loadLanguages(dirs, userSettings);

    m_light = !Lieve::isDarkTheme();
    const QString configured = Lieve::config().value(QStringLiteral("icons/theme"), QStringLiteral("auto")).toString().trimmed();
    if (configured == QLatin1String("system")) {
        return;
    }
    if (configured != QLatin1String("auto")) {
        loadIconTheme(configured, dirs);
        return;
    }
    // Same theme as VS Code; if that one is font based (like the default
    // "vs-seti") try the common SVG themes instead.
    const QString fromVsCode = userSettings.value(QLatin1String("workbench.iconTheme")).toString();
    if (!fromVsCode.isEmpty() && loadIconTheme(fromVsCode, dirs)) {
        return;
    }
    if (!loadIconTheme(QStringLiteral("vscode-icons"), dirs)) {
        loadIconTheme(QStringLiteral("material-icon-theme"), dirs);
    }
}

void VsCodeIcons::loadLanguages(const QStringList &dirs, const QJsonObject &userSettings)
{
    for (const QString &dir : dirs) {
        const QJsonObject pkg = readJsonc(dir + QStringLiteral("/package.json"));
        const QJsonArray languages = pkg.value(QLatin1String("contributes")).toObject().value(QLatin1String("languages")).toArray();
        for (const QJsonValue &lv : languages) {
            const QJsonObject lang = lv.toObject();
            const QString id = lang.value(QLatin1String("id")).toString();
            if (id.isEmpty()) {
                continue;
            }
            const QJsonArray exts = lang.value(QLatin1String("extensions")).toArray();
            for (const QJsonValue &e : exts) {
                m_langByExt.insert(e.toString().toLower(), id);
            }
            const QJsonArray names = lang.value(QLatin1String("filenames")).toArray();
            for (const QJsonValue &n : names) {
                m_langByName.insert(n.toString().toLower(), id);
            }
            const QJsonArray patterns = lang.value(QLatin1String("filenamePatterns")).toArray();
            for (const QJsonValue &p : patterns) {
                addPattern(p.toString(), id, false);
            }
        }
    }

    // "files.associations" from the VS Code settings wins over everything.
    const QJsonObject associations = userSettings.value(QLatin1String("files.associations")).toObject();
    for (auto it = associations.begin(); it != associations.end(); ++it) {
        addPattern(it.key(), it.value().toString(), true);
    }
}

void VsCodeIcons::addPattern(QString glob, const QString &languageId, bool highPriority)
{
    if (glob.startsWith(QLatin1String("**/"))) {
        glob = glob.mid(3);
    }
    if (glob.isEmpty() || glob.contains(QLatin1Char('/')) || languageId.isEmpty()) {
        return;
    }
    const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(glob, QRegularExpression::NonPathWildcardConversion),
                                QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) {
        return;
    }
    (highPriority ? m_userPatterns : m_langPatterns).append({re, languageId});
}

// Same order as VS Code: user associations, file names, name patterns,
// extensions.
QString VsCodeIcons::languageOf(const QString &fileName) const
{
    const QString lower = fileName.toLower();
    for (const auto &pattern : m_userPatterns) {
        if (pattern.first.match(fileName).hasMatch()) {
            return pattern.second;
        }
    }
    if (const auto it = m_langByName.constFind(lower); it != m_langByName.cend()) {
        return *it;
    }
    for (const auto &pattern : m_langPatterns) {
        if (pattern.first.match(fileName).hasMatch()) {
            return pattern.second;
        }
    }
    // Longest extension first: "app.component.ts" tries ".component.ts", then ".ts".
    for (qsizetype i = lower.indexOf(QLatin1Char('.')); i >= 0; i = lower.indexOf(QLatin1Char('.'), i + 1)) {
        if (const auto it = m_langByExt.constFind(lower.mid(i)); it != m_langByExt.cend()) {
            return *it;
        }
    }
    return {};
}

bool VsCodeIcons::loadIconTheme(const QString &themeId, const QStringList &dirs)
{
    QString manifestPath;
    for (const QString &dir : dirs) {
        const QJsonObject pkg = readJsonc(dir + QStringLiteral("/package.json"));
        const QJsonArray themes = pkg.value(QLatin1String("contributes")).toObject().value(QLatin1String("iconThemes")).toArray();
        for (const QJsonValue &tv : themes) {
            const QJsonObject theme = tv.toObject();
            if (theme.value(QLatin1String("id")).toString() == themeId) {
                manifestPath = QDir::cleanPath(dir + QLatin1Char('/') + theme.value(QLatin1String("path")).toString());
                m_themeLabel = theme.value(QLatin1String("label")).toString();
                break;
            }
        }
        if (!manifestPath.isEmpty()) {
            break;
        }
    }
    if (manifestPath.isEmpty()) {
        return false;
    }

    const QJsonObject manifest = readJsonc(manifestPath);
    const QDir base = QFileInfo(manifestPath).dir();
    const QJsonObject defs = manifest.value(QLatin1String("iconDefinitions")).toObject();
    QHash<QString, QString> paths;
    for (auto it = defs.begin(); it != defs.end(); ++it) {
        const QString rel = it.value().toObject().value(QLatin1String("iconPath")).toString();
        if (!rel.isEmpty()) {
            paths.insert(it.key(), QDir::cleanPath(base.filePath(rel)));
        }
    }
    // Font based themes (like the built-in Seti) have no iconPath.
    if (paths.isEmpty()) {
        m_themeLabel.clear();
        return false;
    }
    m_iconPaths = paths;
    readMaps(manifest, m_dark);
    readMaps(manifest.value(QLatin1String("light")).toObject(), m_lightMaps);
    m_hasTheme = true;
    return true;
}

void VsCodeIcons::readMaps(const QJsonObject &o, Maps &m)
{
    m.file = o.value(QLatin1String("file")).toString();
    m.folder = o.value(QLatin1String("folder")).toString();
    m.folderExpanded = o.value(QLatin1String("folderExpanded")).toString();
    const auto readMap = [&o](const char *key, QHash<QString, QString> &target) {
        const QJsonObject map = o.value(QLatin1String(key)).toObject();
        for (auto it = map.begin(); it != map.end(); ++it) {
            target.insert(it.key().toLower(), it.value().toString());
        }
    };
    readMap("fileNames", m.fileNames);
    readMap("fileExtensions", m.fileExtensions);
    readMap("folderNames", m.folderNames);
    readMap("folderNamesExpanded", m.folderNamesExpanded);
    readMap("languageIds", m.languageIds);
}

QString VsCodeIcons::lookup(QHash<QString, QString> Maps::*member, const QString &key) const
{
    if (m_light) {
        const auto &lightMap = m_lightMaps.*member;
        if (const auto it = lightMap.constFind(key); it != lightMap.cend()) {
            return *it;
        }
    }
    const auto &darkMap = m_dark.*member;
    if (const auto it = darkMap.constFind(key); it != darkMap.cend()) {
        return *it;
    }
    return {};
}

// Same precedence as VS Code: exact file name, then extensions from the
// longest to the shortest, then the language, then the generic file icon.
QString VsCodeIcons::resolveFile(const QString &fileName) const
{
    const QString lower = fileName.toLower();
    QString id = lookup(&Maps::fileNames, lower);
    if (!id.isEmpty()) {
        return id;
    }
    for (qsizetype i = lower.indexOf(QLatin1Char('.')); i >= 0; i = lower.indexOf(QLatin1Char('.'), i + 1)) {
        id = lookup(&Maps::fileExtensions, lower.mid(i + 1));
        if (!id.isEmpty()) {
            return id;
        }
    }
    const QString lang = languageOf(fileName);
    if (!lang.isEmpty()) {
        id = lookup(&Maps::languageIds, lang);
        if (!id.isEmpty()) {
            return id;
        }
    }
    return (m_light && !m_lightMaps.file.isEmpty()) ? m_lightMaps.file : m_dark.file;
}

QIcon VsCodeIcons::iconForId(const QString &id)
{
    if (id.isEmpty()) {
        return {};
    }
    if (const auto it = m_iconCache.constFind(id); it != m_iconCache.cend()) {
        return *it;
    }
    const QString path = m_iconPaths.value(id);
    const QIcon icon = (!path.isEmpty() && QFileInfo::exists(path)) ? QIcon(path) : QIcon();
    m_iconCache.insert(id, icon);
    return icon;
}

QIcon VsCodeIcons::systemFileIcon(const QString &fileName)
{
    static QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    if (const auto it = m_systemCache.constFind(mime.name()); it != m_systemCache.cend()) {
        return *it;
    }
    const QIcon icon =
        QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName(), QIcon::fromTheme(QStringLiteral("text-x-generic"))));
    m_systemCache.insert(mime.name(), icon);
    return icon;
}

QIcon VsCodeIcons::fileIcon(const QString &fileName)
{
    if (!m_hasTheme) {
        return systemFileIcon(fileName);
    }
    const QString key = fileName.toLower();
    auto it = m_fileIdCache.constFind(key);
    if (it == m_fileIdCache.cend()) {
        it = m_fileIdCache.insert(key, resolveFile(fileName));
    }
    const QIcon icon = iconForId(*it);
    return icon.isNull() ? systemFileIcon(fileName) : icon;
}

QIcon VsCodeIcons::folderIcon(const QString &folderName, bool expanded)
{
    if (!m_hasTheme) {
        static const QIcon closed = QIcon::fromTheme(QStringLiteral("folder"));
        static const QIcon open = QIcon::fromTheme(QStringLiteral("folder-open"), closed);
        return expanded ? open : closed;
    }
    const QString lower = folderName.toLower();
    QString id = lookup(expanded ? &Maps::folderNamesExpanded : &Maps::folderNames, lower);
    if (id.isEmpty()) {
        const Maps &m = (m_light && !m_lightMaps.folder.isEmpty()) ? m_lightMaps : m_dark;
        id = expanded ? m.folderExpanded : m.folder;
    }
    return iconForId(id);
}
