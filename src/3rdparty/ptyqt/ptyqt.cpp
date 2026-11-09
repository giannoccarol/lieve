#include "ptyqt.h"
#include <utility>

// Lieve: without winpty. Windows uses ConPTY only (Windows 10 1903 and later,
// see CONPTY_MINIMAL_WINDOWS_VERSION); createPtyProcess() returns null where
// it is missing.

#ifdef Q_OS_WIN
#include "conptyprocess.h"
#endif

#ifdef Q_OS_UNIX
#include "unixptyprocess.h"
#endif

bool PtyQt::isUsingConPTY()
{
#ifdef Q_OS_WIN
    if (ConPtyProcess::isAvailable())
        return true;
#endif

    return false;
}

IPtyProcess *PtyQt::createPtyProcess(IPtyProcess::PtyType ptyType)
{
    switch (ptyType)
    {
#ifdef Q_OS_WIN
    case IPtyProcess::PtyType::ConPty:
        return new ConPtyProcess();
        break;
#endif
#ifdef Q_OS_UNIX
    case IPtyProcess::PtyType::UnixPty:
        return new UnixPtyProcess();
        break;
#endif
    case IPtyProcess::PtyType::AutoPty:
    default:
        break;
    }

#ifdef Q_OS_WIN
    if (isUsingConPTY())
        return new ConPtyProcess();
    return nullptr;
#endif
#ifdef Q_OS_UNIX
    return new UnixPtyProcess();
#endif
}
