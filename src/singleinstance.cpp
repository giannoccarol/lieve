#include "singleinstance.h"

#include "platform.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

#include <memory>

namespace Lieve::SingleInstance
{
namespace
{
constexpr quint32 kMagic = 0x4c696576; // "Liev"
constexpr quint32 kMaxSize = 1 << 20;
constexpr quint32 kMaxArguments = 32768; // a Windows command line has at most 32767 characters
const char kTaken = '+';

// Per user and desktop session, and the same for every copy of Lieve.
QString instanceName()
{
    const QByteArray hash = QCryptographicHash::hash(Platform::sessionKey().toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("lieve-") + QString::fromLatin1(hash);
}

// True once the running Lieve has read the request.
bool send(const QString &name, const QByteArray &request)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(500)) {
        return false;
    }
    Platform::allowForeground(socket.socketDescriptor()); // so that it can bring its window to the front
    socket.write(request);
    socket.waitForBytesWritten(1000);
    // It answers when its event loop reads the request: soon, unless it is still starting.
    const bool taken = socket.waitForReadyRead(3000) && socket.read(1) == QByteArray(1, kTaken);
    socket.disconnectFromServer();
    return taken;
}

void listen(const QString &name, QObject *parent, const Handler &handle)
{
    auto *server = new QLocalServer(parent);
    server->setSocketOptions(QLocalServer::UserAccessOption); // this user's launches only
    if (!server->listen(name)) {
        QLocalServer::removeServer(name); // a socket file left by a crash (Unix)
        server->listen(name);
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server, handle] {
        while (QLocalSocket *socket = server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, server, handle] {
                QStringList arguments;
                QString workingDirectory;
                switch (decode(socket->peek(socket->bytesAvailable()), &arguments, &workingDirectory)) {
                case Decoded::Incomplete:
                    return;
                case Decoded::Invalid:
                    socket->abort();
                    return;
                case Decoded::Complete:
                    socket->readAll();
                    socket->write(QByteArray(1, kTaken));
                    socket->flush();
                    // From the event loop, not inside this socket's signal: opening a
                    // folder shows a dialog, and the socket goes away meanwhile.
                    QMetaObject::invokeMethod(
                        server,
                        [handle, arguments, workingDirectory] {
                            handle(arguments, workingDirectory);
                        },
                        Qt::QueuedConnection);
                    return;
                }
            });
        }
    });
}
}

bool handOver(const QStringList &arguments, const QString &workingDirectory, QObject *parent, const Handler &handle)
{
    const QString name = instanceName();
    // Who listens is settled by a lock file: launches at the same moment
    // (files opened together from Explorer) still end up in one process.
    static std::unique_ptr<QLockFile> lock;
    lock = std::make_unique<QLockFile>(QDir::temp().filePath(name + QStringLiteral(".lock")));
    lock->setStaleLockTime(0); // stale only when the Lieve that took it is gone
    const QByteArray request = encode(arguments, workingDirectory);
    const QDeadlineTimer deadline(10000);
    do {
        if (lock->tryLock(0)) {
            listen(name, parent, handle);
            return false;
        }
        if (lock->error() != QLockFile::LockFailedError) {
            break; // no lock file (a temporary folder that cannot be written)
        }
        if (send(name, request)) {
            return true;
        }
        QThread::msleep(100); // the running Lieve is about to listen, or about to exit
    } while (!deadline.hasExpired());
    lock.reset();
    return false; // a Lieve that does not answer: this one runs on its own
}

QByteArray encode(const QStringList &arguments, const QString &workingDirectory)
{
    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic << workingDirectory << quint32(arguments.size());
    for (const QString &argument : arguments) {
        out << argument;
    }
    QByteArray request;
    QDataStream header(&request, QIODevice::WriteOnly);
    header << quint32(body.size());
    return request + body;
}

Decoded decode(const QByteArray &data, QStringList *arguments, QString *workingDirectory)
{
    if (data.size() < 4) {
        return Decoded::Incomplete;
    }
    QDataStream header(data);
    quint32 size = 0;
    header >> size;
    if (size > kMaxSize) {
        return Decoded::Invalid;
    }
    if (data.size() - 4 < qsizetype(size)) {
        return Decoded::Incomplete;
    }
    QDataStream in(data.mid(4, size));
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 count = 0;
    QString directory;
    in >> magic >> directory >> count;
    if (in.status() != QDataStream::Ok || magic != kMagic || count > kMaxArguments) {
        return Decoded::Invalid;
    }
    QStringList list;
    for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        QString argument;
        in >> argument;
        list << argument;
    }
    if (in.status() != QDataStream::Ok || !in.atEnd()) {
        return Decoded::Invalid;
    }
    *arguments = list;
    *workingDirectory = directory;
    return Decoded::Complete;
}
}
