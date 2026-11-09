# Third-party code

Lieve's built-in terminal (`src/builtinterminal.cpp`, used where the Konsole
part is missing: Windows, Linux without Konsole, or `[terminal] engine=builtin`)
is made of code taken from [Qt Creator](https://github.com/qt-creator/qt-creator),
commit `9143991`. Only the files the terminal needs are here; the Makefile
compiles them into the program, with their own warnings silenced.

| Folder | From Qt Creator | What it is | Licence |
|---|---|---|---|
| `terminal/` | `src/libs/solutions/terminal` | The terminal view (`TerminalSolution::TerminalView`, a `QAbstractScrollArea`) and its surface over libvterm: drawing, selection, scrollback, keys, box drawing, sixel images. © The Qt Company Ltd. | `GPL-3.0-or-later OR GPL-3.0 WITH Qt-GPL-exception-1.0` (commercial option aside); `terminal_global.h` and `surfaceintegration.h`: `GPL-3.0-only WITH Qt-GPL-exception-1.0`; `scrollback.{h,cpp}`: `BSD-3-Clause`, © 2020 Justin Bronder. See the SPDX header of each file. |
| `ptyqt/` | `src/libs/3rdparty/libptyqt` | [Pty-Qt](https://github.com/kafeg/ptyqt) by Vitaly Petrov: the shell in a pseudo-terminal, a Unix pty or ConPTY on Windows. The ConPTY part (`conptyprocess.cpp`) holds code ported from Microsoft's [Windows Terminal](https://github.com/microsoft/terminal). | MIT (`LICENSE`); the Windows Terminal parts MIT, © Microsoft Corporation (`LICENSE-CONPTY`) |
| `vterm/` | `src/libs/3rdparty/libvterm` | [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3 by Paul Evans, with Qt Creator's changes: the VT220/xterm emulator, in C. | MIT (`LICENSE`) |

The text of the Qt GPL exception is Qt Creator's `LICENSE.GPL3-EXCEPT`, also at
<https://spdx.org/licenses/Qt-GPL-exception-1.0.html>.

## Changes made for Lieve

- `ptyqt/ptyqt.cpp`: without winpty (`winptyprocess.*` is left out). On Windows
  only ConPTY is used (Windows 10 1903, build 18309, or later); where it is
  missing `createPtyProcess()` returns null and Lieve opens its terminals
  outside, as before.
- `ptyqt/conptyprocess.cpp`: the program and its arguments are quoted on the
  command line when they hold spaces (`C:\Program Files\PowerShell\7\pwsh.exe`);
  the console host's command line is formatted with `%ls`, a wide string under
  both Microsoft's and MinGW's (C99) rules.
- `terminal/terminal.qrc`: new, the resource Qt Creator's CMake file builds
  (the lock shown at password prompts).
- Left out: Qt Creator's CMake and qbs files, `winpty`, the terminal plugin
  (shell integration, settings, search).
