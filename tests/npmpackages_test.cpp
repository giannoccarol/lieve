// The NPM SCRIPTS section: which package.json files are found, their
// scripts in the written order, the package manager and the command line.
#include "npmpackages.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace Lieve::Npm;

namespace
{
void write(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString root = dir.path();
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        failures += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };

    write(root + QStringLiteral("/package.json"),
          "{\n"
          "  \"name\": \"demo\",\n"
          "  \"config\": { \"scripts\": { \"fake\": \"no\" }, \"list\": [1, {\"a\": \"}\"}, true, null] },\n"
          "  \"scripts\": {\n"
          "    \"start\": \"ng serve\",\n"
          "    \"build\": \"ng build\",\n"
          "    \"build:prod\": \"ng build -c production\",\n"
          "    \"a\\\"quote\": \"echo \\\"hi\\\"\",\n"
          "    \"àccent\": \"echo è\",\n"
          "    \"broken\": 42\n"
          "  },\n"
          "  \"devDependencies\": {}\n"
          "}\n");
    write(root + QStringLiteral("/yarn.lock"), "");
    write(root + QStringLiteral("/web/package.json"), R"({"scripts": {"dev": "vite", "test": "vitest"}, "packageManager": "pnpm@9.1.0"})");
    write(root + QStringLiteral("/web/pnpm-lock.yaml"), "");
    write(root + QStringLiteral("/api/package.json"), R"({"scripts": {"serve": "node index.js"}})");
    write(root + QStringLiteral("/docs/package.json"), R"({"name": "docs"})");
    write(root + QStringLiteral("/tools/package.json"), R"({"scripts": {"x": "y"}, "packageManager": "bun@1.1.0"})");
    write(root + QStringLiteral("/tools/package-lock.json"), "{}");
    write(root + QStringLiteral("/bad/package.json"), "{ not json");
    write(root + QStringLiteral("/node_modules/dep/package.json"), R"({"scripts": {"no": "no"}})");
    write(root + QStringLiteral("/node_modules/package.json"), R"({"scripts": {"no": "no"}})");
    write(root + QStringLiteral("/.hidden/package.json"), R"({"scripts": {"no": "no"}})");
    write(root + QStringLiteral("/src/main.ts"), "");

    const QList<Package> packages = findPackages(root);
    QStringList found;
    for (const Package &p : packages) {
        found << p.relativeDir;
    }
    expect(found == QStringList({QString(), QStringLiteral("api"), QStringLiteral("docs"), QStringLiteral("tools"), QStringLiteral("web")}),
           "root first, then first-level folders; not node_modules, hidden or broken ones");

    const Package &main = packages.value(0);
    QStringList names;
    for (const Script &s : main.scripts) {
        names << s.name;
    }
    expect(main.name == QLatin1String("demo"), "package name");
    expect(names == QStringList({QStringLiteral("start"), QStringLiteral("build"), QStringLiteral("build:prod"), QStringLiteral("a\"quote"), QStringLiteral("àccent")}),
           "scripts in written order, escapes decoded, non-strings left out");
    expect(main.scripts.value(1).command == QLatin1String("ng build"), "script command");
    expect(main.scripts.value(0).line == 4 && main.scripts.value(0).column == 5, "position of a script name");
    expect(main.scripts.value(4).line == 8 && main.scripts.value(4).column == 5, "position after a line with escapes");
    expect(main.dir == root && main.file() == root + QStringLiteral("/package.json"), "package paths");

    expect(main.runner == QLatin1String("yarn"), "yarn.lock: yarn");
    expect(packages.value(4).runner == QLatin1String("pnpm"), "pnpm-lock.yaml: pnpm");
    expect(packages.value(1).runner == QLatin1String("yarn"), "no lock file of its own: the project's");
    expect(packages.value(3).runner == QLatin1String("npm"), "package-lock.json wins over packageManager");
    expect(packages.value(2).scripts.isEmpty(), "package without scripts is listed, empty");
    expect(detectRunner(root + QStringLiteral("/src"), QString(), QStringLiteral("bun@1.1.0")) == QLatin1String("bun"), "packageManager field");
    expect(detectRunner(root + QStringLiteral("/src"), QString(), QString()) == QLatin1String("npm"), "default: npm");
    write(root + QStringLiteral("/src/bun.lock"), "");
    expect(detectRunner(root + QStringLiteral("/src"), QString(), QString()) == QLatin1String("bun"), "bun.lock: bun");

    expect(commandLine(QStringLiteral("npm"), QStringLiteral("build:prod")) == QLatin1String("npm run build:prod"), "npm run");
    expect(commandLine(QStringLiteral("pnpm"), QStringLiteral("dev")) == QLatin1String("pnpm run dev"), "pnpm run");
    expect(commandLine(QStringLiteral("yarn"), QStringLiteral("start")) == QLatin1String("yarn start"), "yarn");
    expect(commandLine(QStringLiteral("bun"), QStringLiteral("x")) == QLatin1String("bun run x"), "bun run");
    expect(commandLine(QStringLiteral("npm"), QStringLiteral("it's mine")) == QLatin1String("npm run 'it'\\''s mine'"), "quoted name");

    write(root + QStringLiteral("/empty/package.json"), "{}");
    Package empty;
    expect(readPackage(root + QStringLiteral("/empty/package.json"), &empty) && empty.scripts.isEmpty(), "no scripts");
    expect(!readPackage(root + QStringLiteral("/bad/package.json"), &empty), "invalid JSON");
    expect(findPackages(QString()).isEmpty(), "no project");

    std::printf("%s\n", failures ? "npmpackages: FAILED" : "npmpackages: all passed");
    return failures ? 1 : 0;
}
