// The request a second Lieve launch sends to the running one on Windows:
// encoded and read back whole, in pieces, or refused when malformed.
#include "singleinstance.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <cstdio>

using namespace Lieve::SingleInstance;

namespace
{
int failures = 0;

void expect(bool ok, const char *what)
{
    failures += ok ? 0 : 1;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments{QStringLiteral("C:/Lieve/lieve.exe"), QStringLiteral("--new-window"), QStringLiteral("src/main.cpp:12"),
                                QStringLiteral("C:/Progetti/caff\u00e8 \u2019 \"x\""), QString()};
    const QString cwd = QStringLiteral("C:/Users/Lorenzo/Progetti");
    const QByteArray request = encode(arguments, cwd);

    QStringList gotArguments;
    QString gotCwd;
    expect(decode(request, &gotArguments, &gotCwd) == Decoded::Complete, "whole request read");
    expect(gotArguments == arguments, "arguments as sent (accents, quotes, empty)");
    expect(gotCwd == cwd, "working directory as sent");

    bool partial = true;
    for (qsizetype n = 0; n < request.size(); ++n) {
        partial = partial && decode(request.left(n), &gotArguments, &gotCwd) == Decoded::Incomplete;
    }
    expect(partial, "every shorter piece waits for more");

    QByteArray longer;
    QDataStream(&longer, QIODevice::WriteOnly) << quint32(request.size() - 4 + 1);
    longer += request.mid(4) + "x";
    expect(decode(longer, &gotArguments, &gotCwd) == Decoded::Invalid, "refused: bytes left over");
    QByteArray wrongMagic = request;
    wrongMagic[4] = char(wrongMagic.at(4) ^ 0x55);
    expect(decode(wrongMagic, &gotArguments, &gotCwd) == Decoded::Invalid, "refused: not a Lieve request");
    QByteArray huge;
    QDataStream(&huge, QIODevice::WriteOnly) << quint32(64 << 20);
    expect(decode(huge, &gotArguments, &gotCwd) == Decoded::Invalid, "refused: too big");

    expect(decode(encode({}, QString()), &gotArguments, &gotCwd) == Decoded::Complete && gotArguments.isEmpty() && gotCwd.isEmpty(), "empty request");

    std::printf("%s\n", failures ? "singleinstance_test: FAILED" : "singleinstance_test: all passed");
    return failures ? 1 : 0;
}
