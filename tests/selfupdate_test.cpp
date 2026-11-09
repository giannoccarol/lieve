// Updating Lieve on Windows: the zip picked from the release, the folders next
// to Lieve's, which folders may be replaced, command-line and PowerShell
// quoting, the restart script. On Windows the script and the extraction also
// run for real (elsewhere too with LIEVE_TEST_POWERSHELL=<pwsh>).
#include "selfupdate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

using namespace Lieve::SelfUpdate;

namespace
{
int failures = 0;

void expect(bool ok, const char *what)
{
    failures += ok ? 0 : 1;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

void check(const QString &got, const QString &wanted, const char *what)
{
    expect(got == wanted, what);
    if (got != wanted) {
        std::printf("     got:    %s\n     wanted: %s\n", qPrintable(got), qPrintable(wanted));
    }
}

QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

void write(const QString &path, const QByteArray &content = QByteArray())
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(content);
    }
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("(missing)");
}

// A folder as packaging/windows/package.sh makes it.
void makePackage(const QString &dir, const QByteArray &version)
{
    write(dir + QStringLiteral("/lieve.exe"), version);
    write(dir + QStringLiteral("/qt.conf"));
    write(dir + QStringLiteral("/Qt6Core.dll"));
    write(dir + QStringLiteral("/rg.exe"));
    write(dir + QStringLiteral("/LICENSE.txt"));
    write(dir + QStringLiteral("/platforms/qwindows.dll"));
    write(dir + QStringLiteral("/data/locale/it/LC_MESSAGES/ktexteditor6.mo"));
}

void testAsset()
{
    const QJsonObject release = json(R"({"tag_name": "v0.2.0", "assets": [
        {"name": "lieve-0.2.0.tar.gz", "size": 10, "state": "uploaded", "browser_download_url": "https://github.com/x/lieve-0.2.0.tar.gz"},
        {"name": "Lieve-v0.2.0-windows-x64.zip", "size": 1234, "state": "uploaded",
         "browser_download_url": "https://github.com/giannoccarol/lieve/releases/download/v0.2.0/Lieve-v0.2.0-windows-x64.zip"}]})");
    const Asset zip = windowsAsset(release);
    check(zip.url.toString(), QStringLiteral("https://github.com/giannoccarol/lieve/releases/download/v0.2.0/Lieve-v0.2.0-windows-x64.zip"), "asset: the Windows zip");
    expect(zip.size == 1234, "asset: its size");
    expect(windowsAsset(json(R"({"tag_name": "v0.2.0", "assets": []})")).url.isEmpty(), "asset: none attached yet");
    expect(windowsAsset(json(R"({"tag_name": "v0.2.0", "assets": [{"name": "Lieve-v0.1.9-windows-x64.zip", "browser_download_url": "https://h/a.zip"}]})"))
               .url.isEmpty(),
           "asset: another version's zip");
    expect(windowsAsset(json(R"({"tag_name": "v0.2.0", "assets": [{"name": "Lieve-v0.2.0-windows-x64.zip", "browser_download_url": "http://h/a.zip"}]})"))
               .url.isEmpty(),
           "asset: not over https");
    expect(windowsAsset(json(R"({"tag_name": "v0.2.0", "assets": [{"name": "Lieve-v0.2.0-windows-x64.zip", "state": "starter", "browser_download_url": "https://h/a.zip"}]})"))
               .url.isEmpty(),
           "asset: still uploading");
    expect(windowsAsset(json(R"({"tag_name": "v0.2.0", "assets": [{"name": "Lieve-v0.2.0-windows-x64.zip", "browser_download_url": "https://h/a.zip"}]})")).size == -1,
           "asset: size unknown");
}

void testFolders()
{
    const QString apps = QDir::rootPath() + QStringLiteral("apps/");
    const Folders f = folders(apps + QStringLiteral("Lieve"), QStringLiteral("0.2.0"));
    check(f.current, apps + QStringLiteral("Lieve"), "folders: current");
    check(f.download, apps + QStringLiteral("Lieve.update-0.2.0"), "folders: download next to it");
    check(f.old, apps + QStringLiteral("Lieve.old"), "folders: old next to it");
    check(folders(apps + QStringLiteral("Lieve/"), QStringLiteral("0.2.0")).current, apps + QStringLiteral("Lieve"), "folders: trailing slash");
    expect(folders(QDir::rootPath(), QStringLiteral("0.2.0")).current.isEmpty(), "folders: none for the root of a drive");
    expect(folders(apps + QStringLiteral("Lieve"), QString()).current.isEmpty(), "folders: none without a version");
}

void testPackageFolder()
{
    QTemporaryDir tmp;
    const QString dir = tmp.path() + QStringLiteral("/Lieve");
    makePackage(dir, "old");
    expect(isPackageFolder(dir), "package folder: what package.sh makes");
    write(dir + QStringLiteral("/README-Windows.txt"));
    write(dir + QStringLiteral("/KF6Parts.DLL"));
    QDir().mkpath(dir + QStringLiteral("/ImageFormats"));
    expect(isPackageFolder(dir), "package folder: names in any case");
    write(dir + QStringLiteral("/notes.txt"));
    expect(!isPackageFolder(dir), "package folder: not with a file of the user's");
    QFile::remove(dir + QStringLiteral("/notes.txt"));
    QDir().mkpath(dir + QStringLiteral("/src"));
    expect(!isPackageFolder(dir), "package folder: not a build tree");
    QDir(dir + QStringLiteral("/src")).removeRecursively();
    if (QFile::link(tmp.path(), dir + QStringLiteral("/data2"))) { // a symbolic link, or on Windows a shortcut
        expect(!isPackageFolder(dir), "package folder: not with a link");
        QFile::remove(dir + QStringLiteral("/data2"));
    }
    expect(isPackageFolder(dir), "package folder: again once removed");
    QFile::remove(dir + QStringLiteral("/qt.conf"));
    expect(!isPackageFolder(dir), "package folder: not without qt.conf");
    expect(!isPackageFolder(tmp.path() + QStringLiteral("/missing")), "package folder: not a missing folder");
}

void testPackageRoot()
{
    QTemporaryDir tmp;
    const QString nested = tmp.path() + QStringLiteral("/nested");
    makePackage(nested + QStringLiteral("/Lieve"), "new");
    check(packageRoot(nested), nested + QStringLiteral("/Lieve"), "package root: the zip's Lieve folder");
    const QString flat = tmp.path() + QStringLiteral("/flat");
    makePackage(flat, "new");
    check(packageRoot(flat), flat, "package root: a zip without a folder");
    const QString two = tmp.path() + QStringLiteral("/two");
    makePackage(two + QStringLiteral("/Lieve"), "new");
    QDir().mkpath(two + QStringLiteral("/Other"));
    check(packageRoot(two), QString(), "package root: not among two folders");
    QDir().mkpath(tmp.path() + QStringLiteral("/empty"));
    check(packageRoot(tmp.path() + QStringLiteral("/empty")), QString(), "package root: none in an empty folder");
}

void testQuoting()
{
    check(windowsCommandLine({QStringLiteral("C:/My Projects/a"), QStringLiteral("plain"), QString()}),
          QStringLiteral("\"C:/My Projects/a\" plain \"\""),
          "command line: spaces, plain, empty");
    check(windowsCommandLine({QStringLiteral("say \"hi\"")}), QStringLiteral("\"say \\\"hi\\\"\""), "command line: quotes");
    check(windowsCommandLine({QStringLiteral("C:\\dir\\")}), QStringLiteral("C:\\dir\\"), "command line: backslashes left alone");
    check(windowsCommandLine({QStringLiteral("C:\\a b\\")}), QStringLiteral("\"C:\\a b\\\\\""), "command line: backslash before the closing quote");
    check(windowsCommandLine({QStringLiteral("a\\\\\"b c")}), QStringLiteral("\"a\\\\\\\\\\\"b c\""), "command line: backslashes before a quote");
    check(windowsCommandLine({}), QString(), "command line: no arguments");

    check(powershellString(QStringLiteral("C:\\Lieve")), QStringLiteral("'C:\\Lieve'"), "powershell: plain");
    check(powershellString(QStringLiteral("it's $HOME")), QStringLiteral("'it''s $HOME'"), "powershell: quote doubled, $ literal");
    check(powershellString(QStringLiteral("a\u2019b\u2018c")), QStringLiteral("'a\u2019\u2019b\u2018\u2018c'"), "powershell: typographic quotes doubled");
    check(powershellString(QString()), QStringLiteral("''"), "powershell: empty");
}

void testScript()
{
    const QString apps = QDir::rootPath() + QStringLiteral("apps/");
    Restart plan;
    plan.pid = 4242;
    plan.exe = apps + QStringLiteral("Lieve/lieve.exe");
    plan.arguments = {apps + QStringLiteral("My Project"), apps + QStringLiteral("other")};
    plan.fresh = apps + QStringLiteral("Lieve.update-0.2.0/Lieve");
    plan.folders = folders(apps + QStringLiteral("Lieve"), QStringLiteral("0.2.0"));
    const QString script = restartScript(plan);
    const auto native = [](const QString &path) {
        return QDir::toNativeSeparators(path);
    };
    expect(script.contains(QStringLiteral("\n$lievePid = 4242\n")), "script: the pid");
    expect(script.contains(QStringLiteral("\n$exe = '%1'\n").arg(native(plan.exe))), "script: the program");
    expect(script.contains(QStringLiteral("\n$arguments = '\"%1\" %2'\n").arg(native(plan.arguments.at(0)), native(plan.arguments.at(1)))),
           "script: the projects, quoted");
    expect(script.contains(QStringLiteral("\n$current = '%1'\n").arg(native(apps + QStringLiteral("Lieve")))), "script: the current folder");
    expect(script.contains(QStringLiteral("\n$fresh = '%1'\n").arg(native(plan.fresh))), "script: the new version");
    expect(script.contains(QStringLiteral("\n$old = '%1'\n").arg(native(apps + QStringLiteral("Lieve.old")))), "script: the old folder");
    expect(script.contains(QStringLiteral("\n$download = '%1'\n").arg(native(apps + QStringLiteral("Lieve.update-0.2.0")))), "script: the download");

    plan.fresh.clear(); // a plain restart (e.g. after changing the language)
    const QString restart = restartScript(plan);
    expect(restart.contains(QStringLiteral("\n$fresh = ''\n")) && restart.contains(QStringLiteral("\n$current = ''\n")), "script: plain restart moves nothing");
    plan.exe.clear(); // quitting with an update ready
    expect(restartScript(plan).contains(QStringLiteral("\n$exe = ''\n")), "script: nothing started when quitting");
    plan.arguments.clear();
    expect(restartScript(plan).contains(QStringLiteral("\n$arguments = ''\n")), "script: no projects");
}

// The script and the extraction run by PowerShell: Windows PowerShell on
// Windows, or the one in LIEVE_TEST_POWERSHELL.
QString testShell()
{
    const QString shell = qEnvironmentVariable("LIEVE_TEST_POWERSHELL");
#ifdef Q_OS_WIN
    return shell.isEmpty() ? powershell() : shell;
#else
    return shell;
#endif
}

int runShell(const QStringList &arguments, const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    QProcess process;
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(testShell(), QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive")} + arguments);
    if (!process.waitForFinished(120000)) {
        process.kill();
        std::printf("     PowerShell did not finish\n");
        return -1;
    }
    const QByteArray output = process.readAll().trimmed();
    if (!output.isEmpty()) {
        std::printf("     PowerShell: %s\n", output.constData());
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

void testRunScript()
{
    QTemporaryDir tmp;
    // Quotes and accents in the path: escaping and the file's encoding.
    const QString apps = tmp.path() + QStringLiteral("/Apps l'\u00e8 \u2019x");
    const Folders f = folders(apps + QStringLiteral("/Lieve"), QStringLiteral("9.9.9"));
    makePackage(f.current, "old");
    makePackage(f.download + QStringLiteral("/Lieve"), "new");
    write(f.old + QStringLiteral("/keep.txt"), "the user's"); // a Lieve.old already there is not ours

    Restart plan;
    plan.pid = 2147483644; // no such process: nothing to wait for
    plan.fresh = f.download + QStringLiteral("/Lieve");
    plan.folders = f;
    const QString path = tmp.path() + QStringLiteral("/lieve-restart.ps1");
    write(path, QByteArray("\xEF\xBB\xBF") + restartScript(plan).toUtf8());
    const int code = runShell({QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), QDir::toNativeSeparators(path)});
    expect(code == 0, "run script: exits normally");
    check(QString::fromUtf8(read(f.current + QStringLiteral("/lieve.exe"))), QStringLiteral("new"), "run script: the new version in place");
    expect(isPackageFolder(f.current), "run script: the whole package moved");
    expect(!QFileInfo::exists(f.download), "run script: the download removed");
    check(QString::fromUtf8(read(f.old + QStringLiteral("/keep.txt"))), QStringLiteral("the user's"), "run script: an existing Lieve.old left alone");
    expect(QDir(apps).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == QStringList({QStringLiteral("Lieve"), QStringLiteral("Lieve.old")}),
           "run script: the replaced version removed");
    expect(!QFileInfo::exists(path), "run script: removes itself");

    // The zip as the release workflow makes it (Compress-Archive of the Lieve folder).
    const QString zip = tmp.path() + QStringLiteral("/Lieve-v9.9.9-windows-x64.zip");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LIEVE_SRC"), QDir::toNativeSeparators(f.current));
    env.insert(QStringLiteral("LIEVE_ZIP"), QDir::toNativeSeparators(zip));
    expect(runShell({QStringLiteral("-Command"), QStringLiteral("Compress-Archive -LiteralPath $env:LIEVE_SRC -DestinationPath $env:LIEVE_ZIP")}, env) == 0,
           "extract: zip made");
    const QString dest = tmp.path() + QStringLiteral("/Apps \u00e8/Lieve.update-9.9.9");
    env.insert(QStringLiteral("LIEVE_DEST"), QDir::toNativeSeparators(dest));
    expect(runShell({QStringLiteral("-Command"), extractCommand()}, env) == 0, "extract: exits normally");
    check(packageRoot(dest), dest + QStringLiteral("/Lieve"), "extract: lieve.exe in the zip's folder");
    expect(isPackageFolder(dest + QStringLiteral("/Lieve")), "extract: subfolders too");
    env.insert(QStringLiteral("LIEVE_ZIP"), QDir::toNativeSeparators(tmp.path() + QStringLiteral("/missing.zip")));
    env.insert(QStringLiteral("LIEVE_DEST"), QDir::toNativeSeparators(tmp.path() + QStringLiteral("/nothing")));
    expect(runShell({QStringLiteral("-Command"), extractCommand()}, env) == 1, "extract: fails on a missing zip");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testAsset();
    testFolders();
    testPackageFolder();
    testPackageRoot();
    testQuoting();
    testScript();
    if (!testShell().isEmpty()) {
        testRunScript();
    }
    std::printf("%s\n", failures ? "selfupdate_test: FAILED" : "selfupdate_test: all passed");
    return failures ? 1 : 0;
}
