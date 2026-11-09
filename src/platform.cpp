#include "platform.h"

#include <breezeicons.h>

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QProcess>
#include <QStandardPaths>
#include <QStyleHints>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>

#include <signal.h>
#include <unistd.h>
#endif

namespace Lieve::Platform
{
bool isKdeSession()
{
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("KDE"), Qt::CaseInsensitive);
}

namespace
{
// The arguments that make `shell` run `command` and stay open afterwards
// (the output stays readable and the command can be run again).
QStringList runAndStay(const QString &shell, const QString &command)
{
    if (command.isEmpty()) {
        return {};
    }
    const QString name = QFileInfo(shell).completeBaseName().toLower();
    if (name == QLatin1String("pwsh") || name == QLatin1String("powershell")) {
        return {QStringLiteral("-NoExit"), QStringLiteral("-Command"), command};
    }
    if (name == QLatin1String("cmd")) {
        return {QStringLiteral("/K"), command};
    }
    if (name == QLatin1String("wsl")) {
        return {QStringLiteral("--"), QStringLiteral("bash"), QStringLiteral("-ic"), command + QStringLiteral("; exec bash")};
    }
#ifdef Q_OS_WIN
    const QString again = QStringLiteral("bash"); // Git Bash
#else
    const QString again = shell;
#endif
    return {QStringLiteral("-ic"), command + QStringLiteral("; exec ") + again};
}
}

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

void openTerminal(const QString &dir, const QString &shell, const QString &title, const QString &command)
{
    const QStringList shellArguments = runAndStay(shell, command);
    const QString wt = QStandardPaths::findExecutable(QStringLiteral("wt"));
    if (!wt.isEmpty()) { // Windows Terminal
        QStringList arguments{QStringLiteral("-d"), QDir::toNativeSeparators(dir)};
        if (!title.isEmpty()) {
            arguments << QStringLiteral("--title") << title;
        }
        arguments << QDir::toNativeSeparators(shell);
        for (const QString &argument : shellArguments) {
            QString escaped = argument;
            arguments << escaped.replace(QLatin1Char(';'), QStringLiteral("\\;")); // ; separates Windows Terminal's own commands
        }
        QProcess::startDetached(wt, arguments, dir);
        return;
    }
    // The shell in a console window of its own.
    QProcess process;
    process.setProgram(shell);
    process.setArguments(shellArguments);
    process.setWorkingDirectory(dir);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags = (args->flags & ~DWORD(CREATE_NO_WINDOW)) | CREATE_NEW_CONSOLE;
    });
    process.startDetached();
}

Appearance desktopAppearance()
{
    Appearance appearance;
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme(); // Settings > Personalization > Colors
    if (scheme != Qt::ColorScheme::Unknown) {
        appearance.dark = scheme == Qt::ColorScheme::Dark;
    }
    return appearance; // the accent is in Qt's palette (Highlight)
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

namespace
{
struct Emulator {
    const char *program;
    QStringList (*arguments)(const QString &dir, const QStringList &command);
};

// Terminal emulators, the user's choice first ($TERMINAL, then the
// xdg-terminal-exec standard), then the usual ones of each desktop.
const Emulator kEmulators[] = {
    {"xdg-terminal-exec", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--dir=") + dir} + command;
     }},
    {"kgx", [](const QString &dir, const QStringList &command) { // GNOME Console
         return QStringList{QStringLiteral("--working-directory=") + dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("--")} + command);
     }},
    {"ptyxis", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--new-window"), QStringLiteral("--working-directory=") + dir}
             + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("--")} + command);
     }},
    {"gnome-terminal", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--working-directory=") + dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("--")} + command);
     }},
    {"konsole", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--workdir"), dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-e")} + command);
     }},
    {"xfce4-terminal", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--working-directory=") + dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-x")} + command);
     }},
    {"mate-terminal", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--working-directory=") + dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-x")} + command);
     }},
    {"alacritty", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--working-directory"), dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-e")} + command);
     }},
    {"kitty", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--directory"), dir} + command;
     }},
    {"foot", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("--working-directory=") + dir} + command;
     }},
    {"wezterm", [](const QString &dir, const QStringList &command) {
         return QStringList{QStringLiteral("start"), QStringLiteral("--cwd"), dir} + (command.isEmpty() ? QStringList() : QStringList{QStringLiteral("--")} + command);
     }},
    {"x-terminal-emulator", [](const QString &, const QStringList &command) { // Debian's choice; opens in the working directory
         return command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-e")} + command;
     }},
    {"xterm", [](const QString &, const QStringList &command) {
         return command.isEmpty() ? QStringList() : QStringList{QStringLiteral("-e")} + command;
     }},
};
}

void openTerminal(const QString &dir, const QString &shell, const QString &, const QString &command)
{
    const QStringList run = command.isEmpty() ? QStringList() : QStringList{shell} + runAndStay(shell, command);
    // $TERMINAL: the user's own terminal, which opens in its working directory.
    const QString own = qEnvironmentVariable("TERMINAL");
    if (!own.isEmpty() && !QStandardPaths::findExecutable(own).isEmpty()) {
        QProcess::startDetached(own, run.isEmpty() ? QStringList() : QStringList{QStringLiteral("-e")} + run, dir);
        return;
    }
    for (const Emulator &emulator : kEmulators) {
        const QString program = QStandardPaths::findExecutable(QString::fromLatin1(emulator.program));
        if (!program.isEmpty()) {
            QProcess::startDetached(program, emulator.arguments(dir, run), dir);
            return;
        }
    }
}

namespace
{
// org.freedesktop.appearance from the desktop portal (GNOME, and every
// desktop with xdg-desktop-portal): a short call, made once at start.
QVariant portalSetting(const QString &key)
{
    for (const QString &method : {QStringLiteral("ReadOne"), QStringLiteral("Read")}) { // Read: older portals
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                                                           QStringLiteral("org.freedesktop.portal.Settings"), method);
        call << QStringLiteral("org.freedesktop.appearance") << key;
        const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 400);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            continue;
        }
        QVariant value = reply.arguments().constFirst();
        while (value.userType() == qMetaTypeId<QDBusVariant>()) { // Read wraps it twice
            value = qvariant_cast<QDBusVariant>(value).variant();
        }
        return value;
    }
    return {};
}
}

Appearance desktopAppearance()
{
    Appearance appearance;
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) {
        appearance.dark = scheme == Qt::ColorScheme::Dark;
    }
    if (isKdeSession() || !QDBusConnection::sessionBus().isConnected()) {
        return appearance; // Plasma's palette has both
    }
    if (!appearance.dark) {
        const uint value = portalSetting(QStringLiteral("color-scheme")).toUInt(); // 1 dark, 2 light
        if (value == 1 || value == 2) {
            appearance.dark = value == 1;
        }
    }
    const QVariant accent = portalSetting(QStringLiteral("accent-color")); // (ddd), out of range when unset
    if (accent.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument argument = accent.value<QDBusArgument>();
        double red = -1;
        double green = -1;
        double blue = -1;
        argument.beginStructure();
        argument >> red >> green >> blue;
        argument.endStructure();
        if (red >= 0 && red <= 1 && green >= 0 && green <= 1 && blue >= 0 && blue <= 1) {
            appearance.accent = QColor::fromRgbF(float(red), float(green), float(blue));
        }
    }
    return appearance;
}

void setupIcons(bool)
{
    // Outside Plasma the desktop's theme (Adwaita…) lacks many of the icons
    // Lieve uses: Breeze, built into its library, fills in for them.
    if (!isKdeSession()) {
        BreezeIcons::initIcons();
    }
}
#endif
}
