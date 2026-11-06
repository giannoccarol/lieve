#pragma once

#include <QList>
#include <QString>

// The package.json files of a project and their "scripts", for the NPM
// SCRIPTS section of the Explorer (npmscripts.h). No widgets here.
namespace Lieve::Npm
{
struct Script {
    QString name; // "build"
    QString command; // "ng build"
    int line = -1; // of the name in package.json, 0-based
    int column = 0;
    bool operator==(const Script &) const = default;
};

struct Package {
    QString dir; // absolute folder of the package.json
    QString relativeDir; // to the project: "" for the root, "web"
    QString name; // its "name" field, may be empty
    QString runner; // npm, pnpm, yarn or bun
    QList<Script> scripts; // in the order they are written
    QString file() const
    {
        return dir + QStringLiteral("/package.json");
    }
    bool operator==(const Package &) const = default;
};

// Reads a package.json: name and scripts, keeping the scripts' order. False
// if it cannot be read or is not a JSON object.
bool readPackage(const QString &file, Package *package);

// The project's package.json and those of its first-level folders (not
// node_modules nor hidden ones), root first. Packages without scripts are
// listed too (their file is watched for new scripts).
QList<Package> findPackages(const QString &root);

// The package manager for a package: its lock file (pnpm-lock.yaml,
// yarn.lock, bun.lockb or bun.lock, package-lock.json), else the project's,
// else its "packageManager" field ("pnpm@9.1.0"), else npm.
QString detectRunner(const QString &dir, const QString &root, const QString &packageManager);

// What runs a script: "npm run build", "pnpm run build", "yarn build",
// "bun run build". Unusual names are quoted for the shell.
QString commandLine(const QString &runner, const QString &script);
}
