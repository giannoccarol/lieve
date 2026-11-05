#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QProcess;

// Checks GitHub for a newer release and, on request, updates Lieve by
// building the new version from source and installing it where the running
// one is (e.g. ~/.local).
class Updater : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Available, // a newer release exists
        Updating,
        Updated, // installed: restart to use it
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
    // False when Lieve is installed somewhere this user cannot write (e.g. /usr).
    bool canSelfUpdate() const;

Q_SIGNALS:
    void stateChanged();

private:
    using QObject::QObject;
    QString prefix() const;
    void setState(State state);

    QNetworkAccessManager *m_net = nullptr;
    QProcess *m_proc = nullptr;
    QTimer m_timer;
    State m_state = State::Idle;
    QString m_latest;
    QString m_notes;
    QString m_url;
    QString m_log;
};
