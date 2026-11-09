#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

// One Lieve process per user where KDBusService cannot see to it (Windows has
// no session bus): the first Lieve listens on a local socket (a named pipe on
// Windows); a later launch sends it its arguments and working directory and
// exits, as with D-Bus on Linux.
namespace Lieve::SingleInstance
{
using Handler = std::function<void(const QStringList &arguments, const QString &workingDirectory)>;

// True when a running Lieve took the request: this process should exit.
// Otherwise this process now takes the requests of later launches (`handle`
// gets them, from the event loop), or runs on its own if the running Lieve
// does not answer.
bool handOver(const QStringList &arguments, const QString &workingDirectory, QObject *parent, const Handler &handle);

// A request on the socket: its size (quint32), then a magic number, the
// arguments and the working directory (QDataStream).
QByteArray encode(const QStringList &arguments, const QString &workingDirectory);
enum class Decoded {
    Incomplete, // wait for more
    Invalid,
    Complete,
};
Decoded decode(const QByteArray &data, QStringList *arguments, QString *workingDirectory);
}
