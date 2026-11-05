// setConfigValue() changes lieve.conf line by line: comments and other keys
// stay as they are, and config() sees the new value at once.
#include "settings.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv)
{
    QTemporaryDir dir;
    qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };

    Lieve::ensureConfigFile();
    const auto read = [] {
        QFile file(Lieve::configFile());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QByteArray before = read();
    expect(before.contains("[terminal]") && before.contains("\nshell=auto"), "template has [terminal] shell=auto");

    expect(Lieve::setConfigValue(QStringLiteral("terminal"), QStringLiteral("shell"), QStringLiteral("zsh")), "write existing key");
    expect(Lieve::config().value(QStringLiteral("terminal/shell")).toString() == QLatin1String("zsh"), "config() sees the new value");
    const QByteArray after = read();
    QByteArray expected = before;
    expected.replace("\nshell=auto", "\nshell=zsh");
    expect(after == expected, "only that line changed");

    Lieve::setConfigValue(QStringLiteral("terminal"), QStringLiteral("brandNew"), QStringLiteral("1"), QStringLiteral("A new key."));
    const QByteArray added = read();
    expect(added.contains("; A new key.\nbrandNew=1"), "missing key added with its comment");
    expect(added.indexOf("brandNew=1") > added.indexOf("[terminal]") && added.indexOf("brandNew=1") < added.indexOf("[updates]"),
           "added inside its section");

    Lieve::setConfigValue(QStringLiteral("newsection"), QStringLiteral("key"), QStringLiteral("v"));
    expect(Lieve::config().value(QStringLiteral("newsection/key")).toString() == QLatin1String("v"), "missing section appended");

    std::printf("%s\n", failures ? "settings: FAILED" : "settings: all passed");
    return failures ? 1 : 0;
}
