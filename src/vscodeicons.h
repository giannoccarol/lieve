#pragma once

#include <QHash>
#include <QIcon>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Reads the file icon theme and the language definitions straight from the
// VS Code installation and extensions already on this machine, so files get
// the same icons they have in VS Code. Falls back to system mime icons.
class VsCodeIcons
{
public:
    static VsCodeIcons &instance();

    QIcon fileIcon(const QString &fileName);
    QIcon folderIcon(const QString &folderName, bool expanded);
    QString languageOf(const QString &fileName) const;
    QString themeLabel() const
    {
        return m_themeLabel;
    }

private:
    VsCodeIcons();

    struct Maps {
        QString file;
        QString folder;
        QString folderExpanded;
        QHash<QString, QString> fileNames;
        QHash<QString, QString> fileExtensions;
        QHash<QString, QString> folderNames;
        QHash<QString, QString> folderNamesExpanded;
        QHash<QString, QString> languageIds;
    };

    void loadLanguages(const QStringList &extensionDirs, const QJsonObject &userSettings);
    void addPattern(QString glob, const QString &languageId, bool highPriority);
    bool loadIconTheme(const QString &themeId, const QStringList &extensionDirs);
    static void readMaps(const QJsonObject &object, Maps &maps);
    QString lookup(QHash<QString, QString> Maps::*member, const QString &key) const;
    QString resolveFile(const QString &fileName) const;
    QIcon iconForId(const QString &id);
    QIcon systemFileIcon(const QString &fileName);

    bool m_hasTheme = false;
    bool m_light = false;
    QString m_themeLabel;
    Maps m_dark;
    Maps m_lightMaps;
    QHash<QString, QString> m_iconPaths;
    QHash<QString, QIcon> m_iconCache;
    QHash<QString, QString> m_fileIdCache;
    QHash<QString, QIcon> m_systemCache;

    QHash<QString, QString> m_langByName;
    QHash<QString, QString> m_langByExt;
    QList<QPair<QRegularExpression, QString>> m_userPatterns; // files.associations
    QList<QPair<QRegularExpression, QString>> m_langPatterns;
};
