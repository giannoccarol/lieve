#include "projects.h"

#include "settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QStandardPaths>

#include <algorithm>

namespace Lieve
{
namespace
{
constexpr int kMaxRecent = 50;
constexpr int kMaxDiscovered = 300;

const QString kRecentKey = QStringLiteral("projects/recent");
const QString kOpenedKey = QStringLiteral("projects/opened");
const QString kPinnedKey = QStringLiteral("projects/pinned");

QString expandHome(const QString &path)
{
    if (path == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QLatin1String("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

bool isRepository(const QString &dir)
{
    return QFileInfo::exists(dir + QStringLiteral("/.git"));
}

// Every folder inside `root` is a project, except "group" folders that
// contain git repositories (e.g. Projects/phones/app-one): then their
// repositories are the projects.
void discover(const QString &root, QStringList &out)
{
    const QDir dir(root);
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        if (out.size() >= kMaxDiscovered) {
            return;
        }
        const QString path = dir.filePath(entry);
        if (isRepository(path)) {
            out << path;
            continue;
        }
        QStringList repositories;
        const QStringList children = QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (children.size() <= 100) {
            for (const QString &child : children) {
                const QString childPath = path + QLatin1Char('/') + child;
                if (isRepository(childPath)) {
                    repositories << childPath;
                }
            }
        }
        if (repositories.isEmpty()) {
            out << path;
        } else {
            out << repositories;
        }
    }
}

void saveRecent(const QStringList &paths, const QStringList &times)
{
    state().setValue(kRecentKey, paths);
    state().setValue(kOpenedKey, times);
}
}

namespace Projects
{
QStringList roots()
{
    QStringList configured;
    const QStringList values = config().value(QStringLiteral("projects/roots")).toStringList();
    for (const QString &v : values) {
        if (!v.trimmed().isEmpty()) {
            configured << expandHome(v.trimmed());
        }
    }
    if (!configured.isEmpty()) {
        return configured;
    }
    const QString home = QDir::homePath();
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QStringList candidates{documents + QStringLiteral("/Projects"),
                                 documents + QStringLiteral("/Progetti"),
                                 home + QStringLiteral("/Projects"),
                                 home + QStringLiteral("/projects"),
                                 home + QStringLiteral("/Progetti"),
                                 home + QStringLiteral("/src"),
                                 home + QStringLiteral("/code"),
                                 home + QStringLiteral("/dev"),
                                 home + QStringLiteral("/git"),
                                 home + QStringLiteral("/repos"),
                                 home + QStringLiteral("/workspace")};
    QStringList found;
    for (const QString &c : candidates) {
        const QString canonical = QFileInfo(c).canonicalFilePath();
        const bool hasFolders = !canonical.isEmpty() && !QDir(canonical).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
        if (hasFolders && !found.contains(canonical)) {
            found << canonical;
        }
    }
    return found;
}

QList<Project> all()
{
    QList<Project> projects;
    QHash<QString, qsizetype> index;
    const auto add = [&](const QString &path) -> Project * {
        if (const auto it = index.constFind(path); it != index.cend()) {
            return &projects[*it];
        }
        if (!QFileInfo(path).isDir()) {
            return nullptr;
        }
        index.insert(path, projects.size());
        projects.append(Project{path, QFileInfo(path).fileName(), QString(), 0, false});
        return &projects.last();
    };

    const QStringList pinned = state().value(kPinnedKey).toStringList();
    for (const QString &path : pinned) {
        if (Project *p = add(path)) {
            p->pinned = true;
        }
    }
    const QStringList recent = state().value(kRecentKey).toStringList();
    const QStringList times = state().value(kOpenedKey).toStringList();
    for (qsizetype i = 0; i < recent.size(); ++i) {
        if (Project *p = add(recent.at(i))) {
            p->lastOpened = i < times.size() ? times.at(i).toLongLong() : 0;
        }
    }
    QStringList discovered;
    for (const QString &root : roots()) {
        discover(root, discovered);
    }
    for (const QString &path : std::as_const(discovered)) {
        add(QFileInfo(path).canonicalFilePath());
    }

    for (Project &p : projects) {
        p.branch = gitBranch(p.path);
    }
    std::stable_sort(projects.begin(), projects.end(), [](const Project &a, const Project &b) {
        if (a.pinned != b.pinned) {
            return a.pinned;
        }
        if (a.lastOpened != b.lastOpened) {
            return a.lastOpened > b.lastOpened;
        }
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return projects;
}

void touch(const QString &path)
{
    QStringList recent = state().value(kRecentKey).toStringList();
    QStringList times = state().value(kOpenedKey).toStringList();
    while (times.size() < recent.size()) {
        times << QStringLiteral("0");
    }
    const qsizetype at = recent.indexOf(path);
    if (at >= 0) {
        recent.removeAt(at);
        times.removeAt(at);
    }
    recent.prepend(path);
    times.prepend(QString::number(QDateTime::currentSecsSinceEpoch()));
    while (recent.size() > kMaxRecent) {
        recent.removeLast();
        times.removeLast();
    }
    saveRecent(recent, times);
}

void setPinned(const QString &path, bool pin)
{
    QStringList pinned = state().value(kPinnedKey).toStringList();
    pinned.removeAll(path);
    if (pin) {
        pinned.prepend(path);
    }
    state().setValue(kPinnedKey, pinned);
}

void forget(const QString &path)
{
    QStringList recent = state().value(kRecentKey).toStringList();
    QStringList times = state().value(kOpenedKey).toStringList();
    const qsizetype at = recent.indexOf(path);
    if (at >= 0) {
        recent.removeAt(at);
        if (at < times.size()) {
            times.removeAt(at);
        }
        saveRecent(recent, times);
    }
    setPinned(path, false);
}

QString lastOpened()
{
    const QStringList recent = state().value(kRecentKey).toStringList();
    for (const QString &path : recent) {
        if (QFileInfo(path).isDir()) {
            return path;
        }
    }
    return {};
}
}

QString gitDirFor(const QString &start)
{
    if (start.isEmpty()) {
        return {};
    }
    QDir dir(start);
    do {
        const QString dotGit = dir.filePath(QStringLiteral(".git"));
        const QFileInfo info(dotGit);
        if (info.isDir()) {
            return dotGit;
        }
        if (info.isFile()) { // worktree or submodule: "gitdir: <path>"
            QFile f(dotGit);
            if (f.open(QIODevice::ReadOnly)) {
                const QString line = QString::fromUtf8(f.readLine()).trimmed();
                if (line.startsWith(QLatin1String("gitdir:"))) {
                    const QString path = line.mid(7).trimmed();
                    return QDir::cleanPath(QDir::isRelativePath(path) ? dir.absoluteFilePath(path) : path);
                }
            }
            return {};
        }
    } while (dir.cdUp());
    return {};
}

QString gitBranch(const QString &dir)
{
    const QString git = gitDirFor(dir);
    if (git.isEmpty()) {
        return {};
    }
    QFile head(git + QStringLiteral("/HEAD"));
    if (!head.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString text = QString::fromUtf8(head.readAll()).trimmed();
    if (text.startsWith(QLatin1String("ref: refs/heads/"))) {
        return text.mid(16);
    }
    if (text.startsWith(QLatin1String("ref: "))) {
        return text.mid(5);
    }
    return text.left(8);
}

QString projectRootFor(const QString &start)
{
    QDir dir(start);
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral(".git")))) {
            return dir.absolutePath();
        }
    } while (dir.cdUp());
    return start;
}

QString prettyPath(const QString &path)
{
    const QString home = QDir::homePath();
    return path.startsWith(home + QLatin1Char('/')) ? QStringLiteral("~") + path.mid(home.size()) : path;
}

QString relativeAge(qint64 seconds)
{
    const qint64 ago = QDateTime::currentSecsSinceEpoch() - seconds;
    if (ago < 60) {
        return QCoreApplication::translate("Projects", "now");
    }
    if (ago < 3600) {
        const qint64 n = ago / 60;
        return n == 1 ? QCoreApplication::translate("Projects", "1 minute ago") : QCoreApplication::translate("Projects", "%1 minutes ago").arg(n);
    }
    if (ago < 86400) {
        const qint64 n = ago / 3600;
        return n == 1 ? QCoreApplication::translate("Projects", "1 hour ago") : QCoreApplication::translate("Projects", "%1 hours ago").arg(n);
    }
    if (ago < 7 * 86400) {
        return ago < 2 * 86400 ? QCoreApplication::translate("Projects", "yesterday")
                               : QCoreApplication::translate("Projects", "%1 days ago").arg(ago / 86400);
    }
    if (ago < 30 * 86400) {
        const qint64 n = ago / (7 * 86400);
        return n == 1 ? QCoreApplication::translate("Projects", "1 week ago") : QCoreApplication::translate("Projects", "%1 weeks ago").arg(n);
    }
    if (ago < 365 * 86400) {
        const qint64 n = ago / (30 * 86400);
        return n == 1 ? QCoreApplication::translate("Projects", "1 month ago") : QCoreApplication::translate("Projects", "%1 months ago").arg(n);
    }
    const qint64 n = ago / (365 * 86400);
    return n == 1 ? QCoreApplication::translate("Projects", "1 year ago") : QCoreApplication::translate("Projects", "%1 years ago").arg(n);
}

QString relativeTime(qint64 seconds)
{
    if (seconds <= 0) {
        return {};
    }
    const QDateTime then = QDateTime::fromSecsSinceEpoch(seconds);
    const qint64 ago = QDateTime::currentSecsSinceEpoch() - seconds;
    if (ago < 60) {
        return QCoreApplication::translate("Projects", "now");
    }
    if (ago < 3600) {
        const qint64 m = ago / 60;
        return m == 1 ? QCoreApplication::translate("Projects", "1 minute ago") : QCoreApplication::translate("Projects", "%1 minutes ago").arg(m);
    }
    const qint64 days = then.date().daysTo(QDate::currentDate());
    if (days == 0) {
        const qint64 h = ago / 3600;
        return h == 1 ? QCoreApplication::translate("Projects", "1 hour ago") : QCoreApplication::translate("Projects", "%1 hours ago").arg(h);
    }
    if (days == 1) {
        return QCoreApplication::translate("Projects", "yesterday");
    }
    if (days < 7) {
        return QCoreApplication::translate("Projects", "%1 days ago").arg(days);
    }
    return QLocale().toString(then.date(), QLocale::ShortFormat);
}
}
