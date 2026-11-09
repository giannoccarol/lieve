#include "npmpackages.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <string>

namespace Lieve::Npm
{
namespace
{
constexpr qint64 kMaxSize = 4 * 1024 * 1024; // a package.json is a few KB

// QJsonObject sorts its keys; the scripts are listed in the order they are
// written, so their names (and where they are) come from this small scanner.
class KeyScanner
{
public:
    explicit KeyScanner(const QByteArray &text)
        : m_text(text)
    {
    }

    struct Key {
        QString name;
        int line = 0;
        int column = 0; // in characters, of the first one of the name
    };

    // The keys of the top-level object's `member` object, in order.
    QList<Key> keysOf(const QByteArray &member)
    {
        QList<Key> keys;
        skipSpace();
        if (!take('{')) {
            return keys;
        }
        while (skipSpace(), m_pos < m_text.size() && peek() != '}') {
            Key key;
            if (!readKey(&key)) {
                return keys;
            }
            skipSpace();
            if (key.name.toUtf8() == member && peek() == '{') {
                ++m_pos;
                while (skipSpace(), m_pos < m_text.size() && peek() != '}') {
                    Key inner;
                    if (!readKey(&inner)) {
                        return keys;
                    }
                    keys << inner;
                    if (!skipValue() || (skipSpace(), !take(',') && peek() != '}')) {
                        return keys;
                    }
                }
                return keys;
            }
            if (!skipValue() || (skipSpace(), !take(',') && peek() != '}')) {
                return keys;
            }
        }
        return keys;
    }

private:
    char peek() const
    {
        return m_pos < m_text.size() ? m_text.at(m_pos) : '\0';
    }
    bool take(char c)
    {
        if (peek() != c) {
            return false;
        }
        ++m_pos;
        return true;
    }
    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text.at(m_pos);
            if (c == '\n') {
                ++m_line;
                m_lineStart = m_pos + 1;
            } else if (c != ' ' && c != '\t' && c != '\r') {
                return;
            }
            ++m_pos;
        }
    }
    // A string at the cursor: its raw bytes, escapes undone by QJsonDocument.
    bool readString(QString *out)
    {
        if (!take('"')) {
            return false;
        }
        const qsizetype start = m_pos;
        bool escaped = false;
        while (m_pos < m_text.size() && m_text.at(m_pos) != '"') {
            if (m_text.at(m_pos) == '\\') {
                escaped = true;
                ++m_pos;
            }
            ++m_pos;
        }
        if (m_pos >= m_text.size()) {
            return false;
        }
        const QByteArray raw = m_text.mid(start, m_pos - start);
        ++m_pos;
        if (out) {
            *out = escaped ? QJsonDocument::fromJson("[\"" + raw + "\"]").array().at(0).toString() : QString::fromUtf8(raw);
        }
        return true;
    }
    bool readKey(Key *key)
    {
        key->line = m_line;
        key->column = int(QString::fromUtf8(m_text.mid(m_lineStart, m_pos - m_lineStart)).size()) + 1;
        if (!readString(&key->name)) {
            return false;
        }
        skipSpace();
        if (!take(':')) {
            return false;
        }
        skipSpace();
        return true;
    }
    bool skipValue()
    {
        skipSpace();
        const char c = peek();
        if (c == '"') {
            return readString(nullptr);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++m_pos;
            while (skipSpace(), m_pos < m_text.size() && peek() != close) {
                if (c == '{') {
                    Key ignored;
                    if (!readKey(&ignored)) {
                        return false;
                    }
                }
                if (!skipValue()) {
                    return false;
                }
                skipSpace();
                take(',');
            }
            return take(close);
        }
        // number, true, false, null
        const qsizetype start = m_pos;
        static constexpr char delimiters[] = ",}] \t\r\n";
        while (m_pos < m_text.size() && !std::char_traits<char>::find(delimiters, sizeof delimiters - 1, m_text.at(m_pos))) {
            ++m_pos;
        }
        return m_pos > start;
    }

    const QByteArray &m_text;
    qsizetype m_pos = 0;
    int m_line = 0;
    qsizetype m_lineStart = 0;
};

bool isFile(const QString &path)
{
    return QFileInfo(path).isFile();
}

// The package manager whose lock file is in `dir`; "" if there is none.
QString lockFileRunner(const QString &dir)
{
    if (isFile(dir + QStringLiteral("/pnpm-lock.yaml"))) {
        return QStringLiteral("pnpm");
    }
    if (isFile(dir + QStringLiteral("/yarn.lock"))) {
        return QStringLiteral("yarn");
    }
    if (isFile(dir + QStringLiteral("/bun.lockb")) || isFile(dir + QStringLiteral("/bun.lock"))) {
        return QStringLiteral("bun");
    }
    if (isFile(dir + QStringLiteral("/package-lock.json"))) {
        return QStringLiteral("npm");
    }
    return {};
}

bool shellSafe(const QString &word)
{
    return !word.isEmpty() && !word.startsWith(QLatin1Char('-')) && std::all_of(word.cbegin(), word.cend(), [](QChar c) {
        return c.isLetterOrNumber() || QStringView(u"_-:./@+=%,").contains(c);
    });
}

// Also gives the "packageManager" field, if asked.
bool readFile(const QString &file, Package *package, QString *packageManager)
{
    QFile f(file);
    if (f.size() > kMaxSize || !f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray text = f.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(text);
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject object = doc.object();
    package->name = object.value(QStringLiteral("name")).toString();
    if (packageManager) {
        *packageManager = object.value(QStringLiteral("packageManager")).toString();
    }
    package->scripts.clear();
    const QJsonObject scripts = object.value(QStringLiteral("scripts")).toObject();
    if (scripts.isEmpty()) {
        return true;
    }
    const QList<KeyScanner::Key> keys = KeyScanner(text).keysOf("scripts");
    for (const KeyScanner::Key &key : keys) {
        const QJsonValue value = scripts.value(key.name);
        const bool seen = std::any_of(package->scripts.cbegin(), package->scripts.cend(), [&key](const Script &s) {
            return s.name == key.name;
        });
        if (value.isString() && !seen) {
            package->scripts.append({key.name, value.toString(), key.line, key.column});
        }
    }
    // Should the scanner have missed some (it never should), they come last.
    for (auto it = scripts.constBegin(); it != scripts.constEnd(); ++it) {
        const bool listed = std::any_of(package->scripts.cbegin(), package->scripts.cend(), [&it](const Script &s) {
            return s.name == it.key();
        });
        if (!listed && it.value().isString()) {
            package->scripts.append({it.key(), it.value().toString(), -1, 0});
        }
    }
    return true;
}
}

bool readPackage(const QString &file, Package *package)
{
    return readFile(file, package, nullptr);
}

QList<Package> findPackages(const QString &root)
{
    QList<Package> packages;
    if (root.isEmpty()) {
        return packages;
    }
    const auto add = [&](const QString &dir, const QString &relative) {
        Package package;
        QString packageManager;
        const QString file = dir + QStringLiteral("/package.json");
        if (!isFile(file) || !readFile(file, &package, &packageManager)) {
            return;
        }
        package.dir = dir;
        package.relativeDir = relative;
        package.runner = detectRunner(dir, root, packageManager);
        packages << package;
    };
    add(root, QString());

    QStringList folders = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot); // without hidden ones
    folders.removeAll(QStringLiteral("node_modules"));
    // .angular, .vscode, …: hidden on Linux, but not on Windows
    folders.removeIf([](const QString &folder) {
        return folder.startsWith(QLatin1Char('.'));
    });
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(folders.begin(), folders.end(), collator);
    for (const QString &folder : std::as_const(folders)) {
        add(root + QLatin1Char('/') + folder, folder);
    }
    return packages;
}

QString detectRunner(const QString &dir, const QString &root, const QString &packageManager)
{
    QString runner = lockFileRunner(dir);
    if (runner.isEmpty() && !root.isEmpty() && QDir(root) != QDir(dir)) {
        runner = lockFileRunner(root); // workspaces: one lock file for all
    }
    if (runner.isEmpty()) {
        const QString name = packageManager.section(QLatin1Char('@'), 0, 0).trimmed();
        if (name == QLatin1String("pnpm") || name == QLatin1String("yarn") || name == QLatin1String("bun")) {
            runner = name;
        }
    }
    return runner.isEmpty() ? QStringLiteral("npm") : runner;
}

QString commandLine(const QString &runner, const QString &script)
{
    QString name = script;
    if (!shellSafe(name)) {
        // '…' works in bash, zsh and fish; a ' inside becomes '\''.
        name = QLatin1Char('\'') + name.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
    }
    if (runner == QLatin1String("yarn")) {
        return QStringLiteral("yarn ") + name;
    }
    return (runner.isEmpty() ? QStringLiteral("npm") : runner) + QStringLiteral(" run ") + name;
}
}
