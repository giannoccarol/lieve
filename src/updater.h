#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QProcess;

namespace Lieve::SelfUpdate
{
struct Folders;
}

// Checks GitHub for a newer release and, on request, updates Lieve: on Linux
// by building the new version from source and installing it where the
// running one is (e.g. ~/.local); on Windows by downloading the release's
// zip, which replaces Lieve's folder once Lieve has exited.
class Updater : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Available, // a newer release exists
        Updating,
        Updated, // installed (Windows: ready, put in place on restart or quit): restart to use it
        Failed,
    };

    static Updater &instance();
    void start(); // periodic checks, if enabled in lieve.conf
    void checkNow();
    void update();
    void restart();

    State state() const
    {
        return m_state;
    }
    QString latestVersion() const
    {
        return m_latest;
    }
    QString notes() const
    {
        return m_notes;
    }
    QString releaseUrl() const
    {
        return m_url;
    }
    QString log() const
    {
        return m_log;
    }
    // While Updating: how much of the download has arrived, in percent
    // (Windows); -1 when unknown.
    int progress() const
    {
        return m_progress;
    }
    // False when Lieve is installed somewhere this user cannot write (e.g.
    // /usr, or C:\Program Files), or on Windows in a folder it shares with
    // other files.
    bool canSelfUpdate() const;

Q_SIGNALS:
    void stateChanged(); // also when the progress changes

private:
    using QObject::QObject;
    QString prefix() const;
    QNetworkAccessManager *network();
    void updateFromSource();
    void updateFromZip();
    void extract(const QString &zip, const Lieve::SelfUpdate::Folders &folders);
    void runRestartScript(const QString &exe, const QStringList &arguments);
    void fail(const QString &message);
    void setProgress(int percent);
    void setState(State state);

    QNetworkAccessManager *m_net = nullptr;
    QProcess *m_proc = nullptr;
    QTimer m_timer;
    State m_state = State::Idle;
    QString m_latest;
    QString m_notes;
    QString m_url;
    QString m_log;
    QUrl m_zipUrl; // the release's Windows zip
    qint64 m_zipSize = -1;
    QString m_fresh; // Windows: the new version, extracted next to Lieve's folder
    int m_progress = -1;
    bool m_restarting = false;
};
