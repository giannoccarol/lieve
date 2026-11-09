#include "platform.h"

#include <QDir>
#include <QFontDatabase>
#include <QHash>
#include <QIcon>
#include <QProcess>

#ifdef Q_OS_WIN
#include <breezeicons.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace Lieve::Platform
{
#ifdef Q_OS_WIN
namespace
{
QHash<qint64, HANDLE> &jobs()
{
    static QHash<qint64, HANDLE> map;
    return map;
}
}

void ownGroup(QProcess *process)
{
    QObject::connect(process, &QProcess::started, process, [process] {
        const qint64 pid = process->processId();
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
        if (!handle || !AssignProcessToJobObject(job, handle)) {
            if (handle) {
                CloseHandle(handle);
            }
            CloseHandle(job);
            return;
        }
        CloseHandle(handle);
        jobs().insert(pid, job);
    });
}

void endGroup(qint64 pid, bool)
{
    if (HANDLE job = jobs().take(pid)) {
        TerminateJobObject(job, 1);
        CloseHandle(job);
    }
}

QStringList nodeBinDirs()
{
    const QString home = QDir::homePath();
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
    QStringList dirs{
        qEnvironmentVariable("NVM_SYMLINK"), // nvm-windows: the active version
        qEnvironmentVariable("FNM_MULTISHELL_PATH"), // fnm, in a shell where it is set up
        programFiles + QStringLiteral("/nodejs"), // the official installer
        appData + QStringLiteral("/npm"), // npm install -g
        localAppData + QStringLiteral("/Volta/bin"),
        programFiles + QStringLiteral("/Volta"),
        localAppData + QStringLiteral("/pnpm"),
        home + QStringLiteral("/scoop/shims"),
        home + QStringLiteral("/.bun/bin"),
        localAppData + QStringLiteral("/mise/shims"),
    };
    dirs.removeAll(QString());
    for (QString &dir : dirs) {
        dir = QDir::fromNativeSeparators(dir);
    }
    return dirs;
}

QFont fixedFont()
{
    const QStringList families = QFontDatabase::families();
    for (const QString &family : {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")}) {
        if (families.contains(family)) {
            QFont font(family);
            font.setPointSize(10);
            font.setStyleHint(QFont::Monospace);
            return font;
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

void startInConsole(const QString &program, const QStringList &arguments, const QString &dir)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(dir);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags = (args->flags & ~DWORD(CREATE_NO_WINDOW)) | CREATE_NEW_CONSOLE;
    });
    process.startDetached();
}

void setupIcons(bool dark)
{
    BreezeIcons::initIcons(); // the icons built into the library, under :/icons
    if (QIcon::themeName().isEmpty() || QIcon::themeName() == QLatin1String("hicolor")) {
        QIcon::setThemeName(dark ? QStringLiteral("breeze-dark") : QStringLiteral("breeze"));
    }
}
#else
void ownGroup(QProcess *process)
{
    process->setChildProcessModifier([] {
        ::setpgid(0, 0);
    });
}

void endGroup(qint64 pid, bool force)
{
    if (pid > 0) {
        ::kill(-pid, force ? SIGKILL : SIGTERM);
    }
}

QStringList nodeBinDirs()
{
    return {};
}

QFont fixedFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

void startInConsole(const QString &program, const QStringList &arguments, const QString &dir)
{
    QProcess::startDetached(program, arguments, dir);
}

void setupIcons(bool)
{
}
#endif
}
