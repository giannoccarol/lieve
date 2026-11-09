#include "updater.h"

#include "mainwindow.h"
#include "platform.h"
#include "selfupdate.h"
#include "settings.h"
#include "version.h"
#include "windowmanager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QVersionNumber>

namespace
{
// Fetches the release's sources, builds them and installs over the running
// copy. Same steps as install.sh.
const char kUpdateScript[] = R"SH(
set -euo pipefail
SRC="${XDG_DATA_HOME:-$HOME/.local/share}/lieve/source"
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$(dirname "$SRC")"
  git clone --quiet "https://github.com/$REPOSITORY.git" "$SRC"
fi
cd "$SRC"
git fetch --quiet --tags --force origin
git checkout --quiet --force "$TAG"
make -j"$(nproc)"
make install PREFIX="$PREFIX"
)SH";

constexpr int kCheckEveryMs = 6 * 60 * 60 * 1000;
}

Updater &Updater::instance()
{
    static auto *updater = new Updater(qApp);
    return *updater;
}

void Updater::start()
{
#ifdef LIEVE_PACKAGED
    return; // installed by a package manager, which handles updates
#endif
    if (!Lieve::config().value(QStringLiteral("updates/check"), true).toBool() || !qEnvironmentVariableIsEmpty("LIEVE_SCREENSHOT")) {
        return;
    }
    connect(&m_timer, &QTimer::timeout, this, &Updater::checkNow);
    m_timer.start(kCheckEveryMs);
    QTimer::singleShot(8000, this, &Updater::checkNow); // shortly after start, not during it
}

QNetworkAccessManager *Updater::network()
{
    if (!m_net) {
        m_net = new QNetworkAccessManager(this);
    }
    return m_net;
}

void Updater::checkNow()
{
    if (m_state == State::Updating || m_state == State::Updated) {
        return;
    }
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/" LIEVE_REPOSITORY "/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Lieve/" LIEVE_VERSION);
    QNetworkReply *reply = network()->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return; // offline or rate-limited: try again later, silently
        }
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release.value(QLatin1String("tag_name")).toString();
        const QVersionNumber latest = QVersionNumber::fromString(tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag);
        if (latest.isNull() || latest <= QVersionNumber::fromString(QStringLiteral(LIEVE_VERSION))) {
            return;
        }
        const Lieve::SelfUpdate::Asset zip = Lieve::SelfUpdate::windowsAsset(release);
#ifdef Q_OS_WIN
        if (!zip.url.isValid()) {
            return; // the zip is attached a little after the release: until then it is not out for Windows
        }
#endif
        m_zipUrl = zip.url;
        m_zipSize = zip.size;
        m_latest = latest.toString();
        m_notes = release.value(QLatin1String("body")).toString();
        m_url = release.value(QLatin1String("html_url")).toString();
        setState(State::Available);
    });
}

QString Updater::prefix() const
{
    // .../bin/lieve -> ...
    return QFileInfo(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()).absolutePath();
}

bool Updater::canSelfUpdate() const
{
#if defined(LIEVE_PACKAGED)
    return false; // a package manager brings updates
#elif defined(Q_OS_WIN)
    // The zip replaces the whole folder: only a folder of Lieve's own, where
    // this user can write next to it and inside it (not C:\Program Files).
    // Otherwise the release page brings the update.
    const Lieve::SelfUpdate::Folders folders = Lieve::SelfUpdate::folders(QCoreApplication::applicationDirPath(), m_latest);
    if (!m_zipUrl.isValid() || folders.current.isEmpty() || !Lieve::SelfUpdate::isPackageFolder(folders.current)) {
        return false;
    }
    const QTemporaryDir besides(QFileInfo(folders.current).absolutePath() + QStringLiteral("/.lieve-XXXXXX"));
    QTemporaryFile inside(folders.current + QStringLiteral("/.lieve-XXXXXX"));
    return besides.isValid() && inside.open();
#else
    return QFileInfo(prefix() + QStringLiteral("/bin")).isWritable() && !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
#endif
}

void Updater::update()
{
    if (m_state != State::Available && m_state != State::Failed) {
        return;
    }
    m_log.clear();
    m_progress = -1;
#ifdef Q_OS_WIN
    updateFromZip();
#else
    updateFromSource();
#endif
}

void Updater::updateFromSource()
{
    m_proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TAG"), QStringLiteral("v") + m_latest);
    env.insert(QStringLiteral("PREFIX"), prefix());
    env.insert(QStringLiteral("REPOSITORY"), QStringLiteral(LIEVE_REPOSITORY));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    m_proc->setProcessEnvironment(env);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyRead, this, [this] {
        m_log += QString::fromUtf8(m_proc->readAll());
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_log += QString::fromUtf8(m_proc->readAll());
        m_proc->deleteLater();
        m_proc = nullptr;
        setState(status == QProcess::NormalExit && code == 0 ? State::Updated : State::Failed);
    });
    setState(State::Updating);
    m_proc->start(QStringLiteral("bash"), {QStringLiteral("-c"), QString::fromUtf8(kUpdateScript)});
}

void Updater::updateFromZip()
{
    const Lieve::SelfUpdate::Folders folders = Lieve::SelfUpdate::folders(QCoreApplication::applicationDirPath(), m_latest);
    auto *file = new QFile(QDir::temp().filePath(m_zipUrl.fileName()), this);
    if (m_zipUrl.fileName().isEmpty() || folders.current.isEmpty() || !file->open(QIODevice::WriteOnly)) {
        delete file;
        fail(tr("Could not save the download in %1.").arg(QDir::toNativeSeparators(QDir::tempPath())));
        return;
    }
    setState(State::Updating);
    QNetworkRequest request(m_zipUrl);
    request.setRawHeader("User-Agent", "Lieve/" LIEVE_VERSION);
    // GitHub sends the download on to its file servers.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network()->get(request);
    connect(reply, &QNetworkReply::readyRead, file, [reply, file] {
        file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        setProgress(total > 0 ? int(received * 100 / total) : -1);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, folders] {
        reply->deleteLater();
        file->write(reply->readAll());
        const bool written = file->error() == QFileDevice::NoError;
        file->close();
        const QString zip = file->fileName();
        file->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(zip);
            fail(tr("The download failed: %1").arg(reply->errorString()));
        } else if (!written || (m_zipSize > 0 && QFileInfo(zip).size() != m_zipSize)) {
            QFile::remove(zip);
            fail(tr("The download is incomplete."));
        } else {
            extract(zip, folders);
        }
    });
}

// Into a folder next to Lieve's, on the same drive: on restart (or quit) the
// script of runRestartScript() renames it into place.
void Updater::extract(const QString &zip, const Lieve::SelfUpdate::Folders &folders)
{
    setProgress(-1);
    QDir(folders.download).removeRecursively(); // left by an earlier attempt
    auto *proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LIEVE_ZIP"), QDir::toNativeSeparators(zip));
    env.insert(QStringLiteral("LIEVE_DEST"), QDir::toNativeSeparators(folders.download));
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::finished, this, [this, proc, zip, folders](int code, QProcess::ExitStatus status) {
        m_log += QString::fromUtf8(proc->readAll());
        proc->deleteLater();
        QFile::remove(zip);
        const bool extracted = status == QProcess::NormalExit && code == 0;
        const QString root = extracted ? Lieve::SelfUpdate::packageRoot(folders.download) : QString();
        if (root.isEmpty()) {
            QDir(folders.download).removeRecursively();
            fail(extracted ? tr("The download does not contain lieve.exe.") : tr("The download could not be extracted."));
            return;
        }
        m_fresh = root;
        // Quitting Lieve instead of restarting it also puts the new version in place.
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            if (!m_restarting) {
                runRestartScript(QString(), {});
            }
        });
        setState(State::Updated);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, zip](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { // no finished() then
            m_log += proc->errorString();
            proc->deleteLater();
            QFile::remove(zip);
            fail(tr("The download could not be extracted."));
        }
    });
    proc->start(Lieve::SelfUpdate::powershell(),
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), Lieve::SelfUpdate::extractCommand()});
}

void Updater::restart()
{
    // Close first: unsaved files go into backups (hot exit) or are asked
    // about, and the user can still cancel.
    const QSet<QString> projects = WindowManager::instance().openProjects();
    if (!WindowManager::instance().closeAll()) {
        return; // cancelled on an unsaved file: keep running
    }
#ifdef Q_OS_WIN
    m_restarting = true;
    runRestartScript(QCoreApplication::applicationFilePath(), projects.values());
#else
    // The installed copy (maybe just updated), or this one when run from where it was built.
    QString exe = prefix() + QStringLiteral("/bin/lieve");
    if (!QFileInfo(exe).isExecutable()) {
        exe = QCoreApplication::applicationFilePath();
    }
    // Start the new Lieve, with the same projects, only once this one has
    // exited, otherwise it would hand its arguments to us (single instance) and quit.
    QStringList args{QStringLiteral("-c"),
                     QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; shift; exec \"$@\""),
                     QStringLiteral("lieve-restart"),
                     QString::number(QCoreApplication::applicationPid()),
                     exe};
    args << projects.values();
    QProcess::startDetached(QStringLiteral("sh"), args);
#endif
}

// Windows: Lieve's folder cannot be replaced while Lieve runs, and a Lieve
// started now would hand its arguments to this one. So a PowerShell script
// waits for this process to exit, puts the new version in place (if there is
// one) and starts `exe` again (if given) with `arguments`.
void Updater::runRestartScript(const QString &exe, const QStringList &arguments)
{
    Lieve::SelfUpdate::Restart plan;
    plan.pid = QCoreApplication::applicationPid();
    plan.exe = exe;
    plan.arguments = arguments;
    if (m_state == State::Updated && !m_fresh.isEmpty()) {
        plan.fresh = m_fresh;
        plan.folders = Lieve::SelfUpdate::folders(QCoreApplication::applicationDirPath(), m_latest);
    }
    QFile script(QDir::temp().filePath(QStringLiteral("lieve-restart-%1.ps1").arg(plan.pid)));
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    script.write("\xEF\xBB\xBF"); // UTF-8 with a BOM, or Windows PowerShell reads it in the ANSI code page
    script.write(Lieve::SelfUpdate::restartScript(plan).toUtf8());
    script.close();
    Lieve::Platform::allowForeground(-1); // the new Lieve may bring its window to the front
    QProcess process;
    process.setProgram(Lieve::SelfUpdate::powershell());
    process.setArguments({QStringLiteral("-NoProfile"),
                          QStringLiteral("-NonInteractive"),
                          QStringLiteral("-ExecutionPolicy"),
                          QStringLiteral("Bypass"),
                          QStringLiteral("-WindowStyle"),
                          QStringLiteral("Hidden"),
                          QStringLiteral("-File"),
                          QDir::toNativeSeparators(script.fileName())});
    process.setWorkingDirectory(QDir::tempPath()); // not inside the folder it moves
    process.startDetached();
}

void Updater::fail(const QString &message)
{
    const QString details = m_log.trimmed();
    m_log = details.isEmpty() ? message : message + QStringLiteral("\n\n") + details;
    setState(State::Failed);
}

void Updater::setProgress(int percent)
{
    if (percent != m_progress) {
        m_progress = percent;
        Q_EMIT stateChanged();
    }
}

void Updater::setState(State state)
{
    m_state = state;
    Q_EMIT stateChanged();
}
