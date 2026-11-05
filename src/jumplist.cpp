#include "jumplist.h"

#include "projects.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QStandardPaths>

namespace Lieve::JumpList
{
namespace
{
constexpr int kMaxProjects = 8;

// Quote one argument for the Exec key (Desktop Entry spec): inside double
// quotes ", `, $ and \ need a backslash, and the value itself escapes \ again.
QString execArgument(const QString &arg)
{
    QString quoted;
    for (const QChar c : arg) {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\')) {
            quoted += QLatin1Char('\\');
        }
        quoted += c;
    }
    quoted = QLatin1Char('"') + quoted + QLatin1Char('"');
    quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    quoted.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return quoted;
}

QString singleLine(QString text)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text;
}
}

void update()
{
    if (!qEnvironmentVariableIsEmpty("LIEVE_SCREENSHOT")) {
        return; // automated checks must not touch the real menu entry
    }
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/applications/lieve.desktop");
    QFile current(path);
    if (!current.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray old = current.readAll();
    current.close();
    if (!old.contains("X-Lieve-JumpList=true")) {
        return;
    }

    // Pinned first, then most recently opened.
    QList<Project> chosen;
    const QList<Project> all = Projects::all();
    for (const Project &p : all) {
        if ((p.pinned || p.lastOpened > 0) && chosen.size() < kMaxProjects) {
            chosen << p;
        }
    }
    QHash<QString, int> nameCount;
    for (const Project &p : std::as_const(chosen)) {
        ++nameCount[p.name];
    }

    // Keep the program path written by "make install" (a test build run from
    // elsewhere must not take over the menu entry).
    QString exe;
    QString tryExec;
    const QList<QByteArray> oldLines = old.split('\n');
    for (const QByteArray &line : oldLines) {
        if (line.startsWith("[Desktop Action")) {
            break;
        }
        if (line.startsWith("Exec=") && exe.isEmpty()) {
            exe = QString::fromUtf8(line.mid(5)).trimmed();
            if (exe.endsWith(QLatin1String(" %F"))) {
                exe.chop(3);
            }
        } else if (line.startsWith("TryExec=")) {
            tryExec = QString::fromUtf8(line.mid(8)).trimmed();
        }
    }
    if (exe.isEmpty()) {
        exe = execArgument(QCoreApplication::applicationFilePath());
    }
    if (tryExec.isEmpty()) {
        tryExec = QCoreApplication::applicationFilePath();
    }

    QString text;
    text += QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Lieve\n"
        "GenericName=Editor di codice\n"
        "Comment=Editor leggero con esplora risorse, ricerca e terminale\n");
    text += QStringLiteral("Exec=%1 %F\n").arg(exe);
    text += QStringLiteral("TryExec=%1\n").arg(tryExec);
    text += QStringLiteral(
        "Icon=lieve\n"
        "Terminal=false\n"
        "StartupNotify=true\n"
        "StartupWMClass=lieve\n"
        "Categories=Development;TextEditor;IDE;Qt;KDE;\n"
        "Keywords=editor;code;ide;vscode;\n"
        "X-Lieve-JumpList=true\n");
    QStringList actions;
    for (int i = 0; i < chosen.size(); ++i) {
        actions << QStringLiteral("project-%1").arg(i);
    }
    actions << QStringLiteral("choose") << QStringLiteral("open-folder");
    text += QStringLiteral("Actions=%1;\n").arg(actions.join(QLatin1Char(';')));

    for (int i = 0; i < chosen.size(); ++i) {
        const Project &p = chosen.at(i);
        QString name = p.name;
        if (nameCount.value(p.name) > 1) {
            name += QStringLiteral(" (%1)").arg(QFileInfo(QFileInfo(p.path).absolutePath()).fileName());
        }
        text += QStringLiteral("\n[Desktop Action project-%1]\n").arg(i);
        text += QStringLiteral("Name=%1\n").arg(singleLine(name));
        text += QStringLiteral("Icon=%1\n").arg(p.branch.isEmpty() ? QStringLiteral("folder") : QStringLiteral("folder-git"));
        text += QStringLiteral("Exec=%1 %2\n").arg(exe, execArgument(p.path));
    }
    text += QStringLiteral("\n[Desktop Action choose]\nName=Scegli progetto…\nIcon=view-list-details\nExec=%1 --new-window\n").arg(exe);
    text += QStringLiteral("\n[Desktop Action open-folder]\nName=Apri cartella…\nIcon=folder-open\nExec=%1 --open-folder\n").arg(exe);

    const QByteArray data = text.toUtf8();
    if (data == old) {
        return;
    }
    QSaveFile out(path); // atomic: Plasma never sees a half-written file
    if (out.open(QIODevice::WriteOnly)) {
        out.write(data);
        out.commit();
    }
}
}
