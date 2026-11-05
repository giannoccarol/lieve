#include "updater.h"

#include "settings.h"
#include "version.h"
#include "windowmanager.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
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
    if (!Lieve::config().value(QStringLiteral("updates/check"), true).toBool() || !qEnvironmentVariableIsEmpty("LIEVE_SCREENSHOT")) {
        return;
    }
    connect(&m_timer, &QTimer::timeout, this, &Updater::checkNow);
    m_timer.start(kCheckEveryMs);
    QTimer::singleShot(8000, this, &Updater::checkNow); // shortly after start, not during it
}

void Updater::checkNow()
{
    if (m_state == State::Updating || m_state == State::Updated) {
        return;
    }
    if (!m_net) {
        m_net = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/" LIEVE_REPOSITORY "/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Lieve/" LIEVE_VERSION);
    QNetworkReply *reply = m_net->get(request);
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
    return QFileInfo(prefix() + QStringLiteral("/bin")).isWritable() && !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

void Updater::update()
{
    if (m_state != State::Available && m_state != State::Failed) {
        return;
    }
    m_log.clear();
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

void Updater::restart()
{
    // Start the new Lieve only once this one has exited, otherwise it would
    // hand its arguments to us (single instance) and quit.
    const QString pid = QString::number(QCoreApplication::applicationPid());
    const QString exe = prefix() + QStringLiteral("/bin/lieve");
    QProcess::startDetached(QStringLiteral("sh"),
                            {QStringLiteral("-c"),
                             QStringLiteral("while kill -0 %1 2>/dev/null; do sleep 0.2; done; exec \"%2\"").arg(pid, exe)});
    WindowManager::instance().closeAll();
}

void Updater::setState(State state)
{
    m_state = state;
    Q_EMIT stateChanged();
}
