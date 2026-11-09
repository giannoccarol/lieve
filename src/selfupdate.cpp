#include "selfupdate.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace Lieve::SelfUpdate
{
namespace
{
// After the variables that restartScript() writes. Windows PowerShell 5.1:
// nothing newer than that.
const char kRestartBody[] = R"PS(
$log = Join-Path ([System.IO.Path]::GetTempPath()) 'lieve-update.log'
function Note([string]$text) {
    try { Add-Content -LiteralPath $log -Value ((Get-Date -Format s) + ' ' + $text) } catch {}
}
# Windows refuses to move or delete files still in use for a moment (Lieve
# exiting, an antivirus scanning the new files): try again for 10 seconds.
function Retry([scriptblock]$action) {
    $problem = ''
    for ($i = 0; $i -lt 20; $i++) {
        try { & $action; return $true } catch { $problem = $_.Exception.Message; Start-Sleep -Milliseconds 500 }
    }
    Note $problem
    return $false
}
# A program running from Lieve's folder (Lieve started again meanwhile, its
# ripgrep): moving the folder would pull it from under it.
function InUse {
    $inside = $current.TrimEnd([char]'\', [char]'/') + [System.IO.Path]::DirectorySeparatorChar
    foreach ($process in Get-Process) {
        try {
            if ($process.Path -and $process.Path.StartsWith($inside, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
        } catch {}
    }
    return $false
}

$lieve = Get-Process -Id $lievePid -ErrorAction SilentlyContinue
if ($lieve -and -not $lieve.WaitForExit(60000)) {
    Note 'Lieve did not exit within a minute: nothing was changed'
    $exe = ''
    $fresh = ''
}
$updated = $false
if ($fresh) {
    # A folder of that name that is already there is not ours: leave it.
    if (Test-Path -LiteralPath $old) { $old = $old + '-' + (Get-Random) }
    if (Retry { if (InUse) { throw ($current + ' is in use') }; [System.IO.Directory]::Move($current, $old) }) {
        if (Retry { [System.IO.Directory]::Move($fresh, $current) }) {
            $updated = $true
            Note ('Updated ' + $current)
        } elseif (Retry { [System.IO.Directory]::Move($old, $current) }) {
            Note ('The new version could not be moved in: ' + $current + ' is as it was')
        } else {
            Note ('Could not put back ' + $current + ': the previous version is in ' + $old + ', the new one in ' + $fresh)
        }
    } else {
        Note ($current + ' is in use: not updated')
    }
}
if ($exe) {
    try {
        if ($arguments) {
            Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $HOME
        } else {
            Start-Process -FilePath $exe -WorkingDirectory $HOME
        }
    } catch {
        Note $_.Exception.Message
    }
}
# The previous version only once the new one is in place; the download
# (or what is left of it) only if Lieve's folder is there.
if ($updated) {
    Retry { [System.IO.Directory]::Delete($old, $true) } | Out-Null
}
if ($fresh -and (Test-Path -LiteralPath $current) -and (Test-Path -LiteralPath $download)) {
    Retry { [System.IO.Directory]::Delete($download, $true) } | Out-Null
}
Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
)PS";

// What packaging/windows/package.sh puts next to lieve.exe besides DLLs and
// programs: keep in step with it.
const char *const kPackageFolders[] = {"platforms", "styles", "imageformats", "iconengines", "tls", "translations", "data"};
const char *const kPackageFiles[] = {"qt.conf", "LICENSE.txt", "README-Windows.txt"};

bool listed(const QString &name, const auto &names)
{
    for (const char *known : names) {
        if (name.compare(QLatin1String(known), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}
}

Asset windowsAsset(const QJsonObject &release)
{
    const QString name = QStringLiteral("Lieve-%1-windows-x64.zip").arg(release.value(QLatin1String("tag_name")).toString());
    const QJsonArray assets = release.value(QLatin1String("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QUrl url(asset.value(QLatin1String("browser_download_url")).toString());
        const QString state = asset.value(QLatin1String("state")).toString();
        if (asset.value(QLatin1String("name")).toString() == name && url.scheme() == QLatin1String("https")
            && (state.isEmpty() || state == QLatin1String("uploaded"))) {
            return {url, asset.value(QLatin1String("size")).toInteger(-1)};
        }
    }
    return {};
}

Folders folders(const QString &appDir, const QString &version)
{
    const QFileInfo info(QDir::cleanPath(appDir));
    const QString name = info.fileName();
    if (name.isEmpty() || version.isEmpty()) {
        return {};
    }
    const QString parent = info.absolutePath() + QLatin1Char('/');
    return {info.absoluteFilePath(), parent + name + QStringLiteral(".update-") + version, parent + name + QStringLiteral(".old")};
}

bool isPackageFolder(const QString &dir)
{
    const QDir folder(dir);
    if (!QFileInfo(folder.filePath(QStringLiteral("lieve.exe"))).isFile() || !QFileInfo(folder.filePath(QStringLiteral("qt.conf"))).isFile()) {
        return false;
    }
    const QFileInfoList entries = folder.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (entry.isSymLink() || entry.isJunction()) {
            return false; // removing the old folder must not reach elsewhere
        }
        const bool known = entry.isDir() ? listed(name, kPackageFolders)
                                         : (name.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive) || name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
                                            || listed(name, kPackageFiles));
        if (!known) {
            return false;
        }
    }
    return true;
}

QString packageRoot(const QString &dir)
{
    const QDir folder(dir);
    if (QFileInfo(folder.filePath(QStringLiteral("lieve.exe"))).isFile()) {
        return folder.absolutePath();
    }
    const QStringList entries = folder.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (entries.size() == 1 && QFileInfo(folder.filePath(entries.constFirst() + QStringLiteral("/lieve.exe"))).isFile()) {
        return folder.absoluteFilePath(entries.constFirst());
    }
    return {};
}

QString windowsCommandLine(const QStringList &arguments)
{
    QStringList line;
    for (const QString &argument : arguments) {
        if (!argument.isEmpty() && !argument.contains(QLatin1Char(' ')) && !argument.contains(QLatin1Char('\t')) && !argument.contains(QLatin1Char('"'))) {
            line << argument;
            continue;
        }
        // Inside quotes, backslashes are literal except before a quote: those
        // are doubled, and the quote itself gets one more.
        QString quoted = QStringLiteral("\"");
        qsizetype backslashes = 0;
        for (const QChar c : argument) {
            if (c == QLatin1Char('\\')) {
                ++backslashes;
                continue;
            }
            quoted += QString(c == QLatin1Char('"') ? backslashes * 2 + 1 : backslashes, QLatin1Char('\\'));
            quoted += c;
            backslashes = 0;
        }
        quoted += QString(backslashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
        line << quoted;
    }
    return line.join(QLatin1Char(' '));
}

QString powershellString(const QString &text)
{
    QString quoted = QStringLiteral("'");
    for (const QChar c : text) {
        quoted += c;
        if (c == QLatin1Char('\'') || c == QChar(0x2018) || c == QChar(0x2019) || c == QChar(0x201A) || c == QChar(0x201B)) {
            quoted += c;
        }
    }
    return quoted + QLatin1Char('\'');
}

QString powershell()
{
    const QString path = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
        + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    return QFileInfo::exists(path) ? path : QStringLiteral("powershell");
}

QString extractCommand()
{
    return QStringLiteral(
        "try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}; "
        "try { Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "[System.IO.Compression.ZipFile]::ExtractToDirectory($env:LIEVE_ZIP, $env:LIEVE_DEST) } "
        "catch { Write-Output $_.Exception.Message; exit 1 }");
}

QString restartScript(const Restart &restart)
{
    const auto path = [](const QString &p) {
        return powershellString(QDir::toNativeSeparators(p));
    };
    QStringList arguments;
    for (const QString &argument : restart.arguments) {
        arguments << QDir::toNativeSeparators(argument);
    }
    const bool swap = !restart.fresh.isEmpty() && !restart.folders.current.isEmpty();
    return QStringLiteral(
               "# Written by Lieve, which is quitting: once it has exited, puts its new\n"
               "# version in place of the old one (when there is one) and starts it again.\n"
               "$lievePid = %1\n"
               "$exe = %2\n"
               "$arguments = %3\n"
               "$current = %4\n"
               "$fresh = %5\n"
               "$old = %6\n"
               "$download = %7\n")
               .arg(QString::number(restart.pid),
                    path(restart.exe),
                    powershellString(windowsCommandLine(arguments)),
                    path(swap ? restart.folders.current : QString()),
                    path(swap ? restart.fresh : QString()),
                    path(swap ? restart.folders.old : QString()),
                    path(swap ? restart.folders.download : QString()))
        + QString::fromLatin1(kRestartBody);
}
}
