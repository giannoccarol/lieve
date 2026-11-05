#include "backups.h"

#include "settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace Lieve::Backups
{
namespace
{
// A backup is a one-line JSON header (path, disk time) followed by the text.
constexpr int kVersion = 1;

QString root()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/lieve/backups");
}

// Same hash as the project's session key in state.ini.
QString directory(const QString &project)
{
    return root() + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(project.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

// Empty unless `name` is a plain file name of ours (it is read from state.ini).
QString filePath(const QString &project, const QString &name)
{
    if (project.isEmpty() || !name.endsWith(QLatin1String(".backup")) || name.startsWith(QLatin1Char('.')) || name.contains(QLatin1Char('/'))) {
        return {};
    }
    return directory(project) + QLatin1Char('/') + name;
}
}

bool hotExit()
{
    return config().value(QStringLiteral("files/hotExit"), true).toBool();
}

QString newName(const QString &path)
{
    // Unique, and still readable in the folder: random part, then the file's name.
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString name = path.isEmpty() ? QStringLiteral("untitled") : QFileInfo(path).fileName().left(64);
    return id + QLatin1Char('-') + name + QStringLiteral(".backup");
}

QStringList names(const QString &project)
{
    if (project.isEmpty()) {
        return {};
    }
    return QDir(directory(project)).entryList({QStringLiteral("*.backup")}, QDir::Files, QDir::Time | QDir::Reversed);
}

bool write(const QString &project, const Backup &backup)
{
    const QString file = filePath(project, backup.name);
    if (file.isEmpty()) {
        return false;
    }
    const QString dir = directory(project);
    if (!QFileInfo::exists(dir)) {
        QDir().mkpath(dir);
        QFile::setPermissions(root(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); // unsaved text: only for the user
    }
    const QJsonObject header{
        {QStringLiteral("version"), kVersion},
        {QStringLiteral("path"), backup.path},
        {QStringLiteral("diskTime"), backup.diskTime},
    };
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n');
    out.write(backup.text.toUtf8());
    return out.commit();
}

std::optional<Backup> read(const QString &project, const QString &name)
{
    const QString path = filePath(project, name);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray data = file.readAll();
    const qsizetype newline = data.indexOf('\n');
    const QJsonObject header = QJsonDocument::fromJson(data.left(newline)).object();
    if (newline < 0 || header.value(QLatin1String("version")).toInt() != kVersion) {
        return std::nullopt;
    }
    Backup backup;
    backup.name = name;
    backup.path = header.value(QLatin1String("path")).toString();
    backup.diskTime = header.value(QLatin1String("diskTime")).toInteger();
    backup.text = QString::fromUtf8(data.constData() + newline + 1, data.size() - newline - 1);
    return backup;
}

void remove(const QString &project, const QString &name)
{
    const QString path = filePath(project, name);
    if (!path.isEmpty() && QFile::remove(path)) {
        QDir().rmdir(directory(project)); // only goes when it was the last one
    }
}
}
