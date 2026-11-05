# Lieve

<img src="data/lieve.svg" alt="" width="96" align="right">

A lightweight, native code editor for KDE Plasma (Qt 6 + KDE Frameworks, no Electron).
It does a few things and does them well: **Explorer**, **Search in files**, **Quick Open** (`Ctrl+P`),
**editor** and **terminal**, with several projects open at once (one window per project),
**Prettier on save** and a **Git panel**, as in VS Code.
File icons and the main keyboard shortcuts are the ones from VS Code.

*Lieve* is Italian for "light". The interface is currently in Italian.

How heavy it is, measured with the same medium-sized Angular project open in both:

| | VS Code (with its extensions) | Lieve |
| --- | --- | --- |
| Memory (PSS) | ~1,270 MB | ~110 MB |
| Processes / threads | 24 / 434 | 2 / 14 |
| Disk space | ~2.7 GB | ~1.4 MB (plus the KDE libraries Plasma already ships) |

It starts in about a quarter of a second, and each extra open project costs a few MB.
There is no language server (no TypeScript completion, no "go to definition"):
that is the price of being light.

## How it is built

| Part | Component |
| --- | --- |
| Editor | KTextEditor (the same as Kate): highlighting for hundreds of languages, multiple cursors, find/replace, `.editorconfig` |
| Terminal | embedded Konsole (konsolepart): your shell and your Konsole profile |
| Search and `Ctrl+P` | ripgrep (`rg`): honours `.gitignore`, skips `node_modules` |
| File icons | read from the VS Code extensions already installed (`workbench.iconTheme`, e.g. vscode-icons) |
| Editor theme | "VSCodium Dark" with a dark system theme, "Breeze Light" with a light one |
| Window look | colours derived from the editor theme (Qt palette + style sheet), like VS Code |
| Multiple windows | a single process (KDBusService): running `lieve` again hands the request to the open one |
| Formatting | the project's own Prettier (`node_modules`), kept loaded by a small Node process |
| Git | the `git` command (status with `--porcelain=v2`), one operation at a time in the background |

The code is in `src/` (about 9,900 lines of C++, headers and blank lines included):
`mainwindow` (window, tabs, project session), `windowmanager` (windows and single instance),
`projects` and `projectlist` (project list and picker), `jumplist` (taskbar menu),
`explorer`, `searchpanel`, `quickopen`, `terminalpanel`, `vscodeicons` (VS Code icons and languages),
`prettier` (formatting), `git` and `gitpanel` (source control), `theme` (look, cards),
`titlebar`, `statusbar`, `tabview` (scrolling tabs), `updater`, `fuzzy`, `settings`.

## Install

You need KDE Plasma 6 (or at least KDE Frameworks 6) with `qt6-base`, `qt6-svg`, `ktexteditor`, `kparts`,
`kdbusaddons`, `kwindowsystem`, `syntax-highlighting`, `konsole` and `ripgrep`, plus `git`, `gcc`, `make` and
`pkgconf` to build it. On Arch/CachyOS with Plasma almost everything is already installed. No CMake needed.

The easy way: the script downloads the latest release, builds it and installs it into `~/.local`
(no `sudo`). If something is missing it tells you which packages to install (Arch and Fedora).

```bash
curl -fsSL https://raw.githubusercontent.com/giannoccarol/lieve/main/install.sh | bash
```

Or by hand:

```bash
git clone https://github.com/giannoccarol/lieve.git
cd lieve
make -j$(nproc)
make install        # into ~/.local: bin/lieve, menu entry, icon
```

Uninstall: `make uninstall`.

### Updates

Lieve checks by itself, shortly after starting and then every 6 hours, whether a new version is out on GitHub.
If there is one, the status bar shows **⬆ Aggiornamento** (update): one click shows what's new, and
**Aggiorna ora** (update now) downloads, builds and installs the new version in the background, in the same
folder Lieve is installed in. When it's done, click **Riavvia** (restart). The downloaded sources stay in
`~/.local/share/lieve/source`. Turn the check off with `[updates] check=false` in the settings (`Ctrl+,`).

## Usage

```bash
lieve                      # project picker
lieve ~/Projects/my-site
lieve src/app/app.component.ts:42   # opens the project (folder with .git) and goes to line 42
lieve --open-folder        # asks which folder to open
```

### Projects and windows

- **At startup** you pick a project: *pinned* first, then *recent*, then every folder found in your
  project folders (detected automatically, e.g. `~/Projects` or `~/Documents/Projects`; a "group" folder
  that contains other repositories lists those directly). Type to filter; `Enter` opens the last one used,
  which is already selected.
- **One window per project**, all in the same process. Each project remembers its own tabs, cursor
  positions, sidebar, terminal and window size.
- **`Ctrl+R`** opens the project picker from any window: `Enter` opens the project in its own window
  (or raises it if it's already open), `Ctrl+Enter` replaces the current window's project. The previous
  project is preselected, so `Ctrl+R` `Enter` switches back and forth.
- Running `lieve` again (from the menu, a terminal, even the built-in one) does not start a second
  program: it opens or raises the right window.
- Right-click a project: **pin to top**, **remove from recent**, copy path, show in file manager.

### Prettier on save

It follows the same rules as VS Code, read from your settings: `editor.formatOnSave` and
`prettier.requireConfig` from the project's `.vscode/settings.json` or from your VS Code user settings,
plus `.prettierrc` (with its plugins), `.prettierignore` and `.editorconfig`.

- It uses the Prettier installed in the project (`node_modules/prettier`): projects without Prettier are left alone.
- Formatting happens right before writing: the file is written once (so `ng serve` rebuilds once).
  The cursor stays where it was and a single `Ctrl+Z` undoes it.
- A small Node process keeps Prettier loaded: it starts when you begin editing a file and exits after
  10 minutes of inactivity. Saving takes about 10-50 ms (the very first time about half a second).
- If the file has a syntax error it is saved as it is and a notice appears at the bottom of the editor.
- Node is looked up in `PATH` and in version managers (mise, volta, nvm, fnm), even when Lieve is started
  from the menu; you can set it by hand in `[format] node=`.

### Source control (Git)

`Ctrl+Shift+G`, or the branch icon in the activity bar, which shows the number of changes.

- **Staged changes** and **Changes** sections (plus **Merge conflicts** when there are any), with file icons
  and VS Code's letters: M modified, A added, D deleted (struck through), R renamed, U untracked, ! conflict.
- Hovering a file or a section shows its buttons: **+** stage, **−** unstage, **discard changes** (asks first;
  new files go to the trash), **open file**.
- **Click** a file: its diff in a read-only tab, added lines in green and removed lines in red.
  **Double-click**: open the file.
- Message and **Commit** (`Ctrl+Enter`). The menu next to it has: amend last commit, commit and push,
  commit and sync. If nothing is staged and `git.enableSmartCommit` is on (as in your VS Code settings),
  the commit includes all changes; otherwise it asks.
- With no changes the button becomes **Sync changes ↓ ↑** (pull and push), or **Publish branch** when the
  branch isn't on the server yet. The ⋮ menu has pull, push, fetch, stage/unstage all and **undo last commit**
  (the message goes back into the box).
- It refreshes by itself: on save, when `.git` changes (also for commands typed in the terminal), when you
  come back to the window and every 10 seconds. With `git.autofetch` on it fetches every 3 minutes, never
  asking for a password in the background.
- Git hooks (husky, lint-staged…) find `node` even when Lieve is started from the menu.
- **Inline blame**, like GitLens: at the end of the cursor line, in grey, who changed it, when and the commit
  message ("Jane Doe, 1 year ago • fix: …"); uncommitted lines say "You, now".
  Turn it off with `[git] inlineBlame=false`.

### Look

The whole window takes its colours from the editor theme, as VS Code does. Sidebar, editor and terminal are
**cards with rounded corners**, separated by small gaps on a darker background; file and terminal tabs are
**pills**, and the tab row **scrolls** with the mouse wheel (no arrows).

The **title bar** is Lieve's own: menu on the left, in the middle a box with the project name that opens
`Ctrl+P`, on the right sidebar / terminal toggles and Breeze-style window buttons (a circle on hover, red on
close). Drag it to move the window, double-click to maximise, and use the edges (the gaps around the cards)
to resize. `[ui] titleBar=system` brings back Plasma's title bar.

At the bottom there is a **status bar**: on the left the branch (click: Git panel) and ↓↑ to sync
(click: sync); on the right who wrote the current line and when ("You, 6 months ago", like GitLens, computed on
the text in the editor), line and column (click: go to line), spaces/tabs (click: change), encoding, line ending
and language (click: their menus), ✓ Prettier (click: format now). The × on tabs shows on the active tab and on hover.

The rest: sidebars and tabs a little darker than the editor, thin borders, slim scrollbars without a track,
a single accent colour (active tab, Commit button, focused fields, counters); the active item in the activity
bar sits on a rounded tile. Light themes work too.

- Pick the accent colour with `[ui] accent=`: `system` (Plasma's), `lieve` (the burgundy of the icon)
  or a `#rrggbb` colour.
- `[ui] theme=system` goes back to Plasma's colours for the window.
- The terminal uses the same colours through a private Konsole profile
  (`~/.local/share/lieve/xdg/konsole/`): the Konsole app and your profiles are not touched.
  Turn it off with `[terminal] matchEditor=false`.

### Taskbar menu

Right-click Lieve's icon (taskbar or application launcher) to get your pinned and recent projects (up to 8),
**Scegli progetto…** (choose project) and **Apri cartella…** (open folder). Lieve keeps these entries up to
date in `~/.local/share/applications/lieve.desktop` (only for the one installed by `make install`, which
carries the line `X-Lieve-JumpList=true`).

### Keyboard shortcuts

| Action | Keys |
| --- | --- |
| Quick Open (`name:line` to go to a line) | `Ctrl+P` |
| Search in files (uses the selected text) | `Ctrl+Shift+F` |
| Explorer | `Ctrl+Shift+E` |
| Show/hide sidebar | `Ctrl+B` |
| Show/hide terminal | `Ctrl+J` |
| Focus terminal / back to editor | `` Ctrl+` `` or `Ctrl+ò` |
| New terminal | `` Ctrl+Shift+` `` or the `+` button |
| Switch project | `Ctrl+R` |
| Source control (Git) | `Ctrl+Shift+G` |
| Commit (from the message box) | `Ctrl+Enter` |
| Open folder | `Ctrl+K` then `Ctrl+O` |
| New window (project picker) | `Ctrl+Shift+N` |
| Close project (back to the picker) | `Ctrl+K` then `F` |
| Close window / quit everything | `Ctrl+Shift+W` / `Ctrl+Q` |
| Save / Save all | `Ctrl+S` / `Ctrl+K` then `S` |
| Format document (Prettier) | `Ctrl+Shift+I` or `Shift+Alt+F` |
| Save without formatting | `Ctrl+K` then `Ctrl+Shift+S` |
| Close tab | `Ctrl+W` (or middle-click the tab) |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Find / replace in file | `Ctrl+F` / `Ctrl+H` |
| Go to line | `Ctrl+G` |
| Select next occurrence / all | `Ctrl+D` / `Ctrl+Shift+L` |
| Move / duplicate lines | `Alt+↑↓` / `Alt+Shift+↑↓` |
| Delete line | `Ctrl+Shift+K` |
| Toggle comment | `Ctrl+/` |
| Settings | `Ctrl+,` |

In the terminal Lieve keeps only `Ctrl+P`, `Ctrl+J`, `Ctrl+Shift+E/F` and `` Ctrl+` ``:
every other key goes to the shell (including `Ctrl+R`, history search).

In the Explorer: click to open, `F2` renames, `Delete` moves to the trash,
right-click for new file/folder, copy path, open in terminal.

## Settings

`Ctrl+,` opens `~/.config/lieve/lieve.conf` (commented). You can change editor theme, font, minimap,
word wrap, VS Code shortcuts, search and Explorer exclusions, icon theme, look (`[ui]`, `[terminal]`),
project folders (`[projects] roots=`), Prettier (`[format]`: `onSave=auto|true|false`, minutes to wait
before closing the process, path to node), Git (`[git]`: `smartCommit`, `autofetch`, `autofetchMinutes`,
`inlineBlame`) and updates (`[updates] check`). Changes apply after a restart.

Recent and pinned projects, and each project's session, are stored in `~/.local/state/lieve/state.ini`.

## Releasing a new version

Versions go up one step at a time: 0.1.0, 0.1.1, 0.1.2, …

1. Bump the number in `src/version.h`.
2. Add a `## v0.1.x` section at the top of `CHANGELOG.md` with what's new.
3. Commit, then create the tag and push it:

```bash
git tag v0.1.1
git push origin main v0.1.1
```

GitHub Actions builds Lieve on Arch Linux, checks that the tag matches `src/version.h` and creates the
release with the notes taken from `CHANGELOG.md`. Everyone who has Lieve installed sees it in the status bar.

## License

Lieve is free software, released under the GNU General Public License version 3 (see [LICENSE](LICENSE)).
Copyright © 2026 Lorenzo Giannoccaro.

## Credits

Lieve puts together components written by others; the credit for the heavy lifting is theirs.

- **KDE Community**: [KTextEditor](https://invent.kde.org/frameworks/ktexteditor) and
  [KSyntaxHighlighting](https://invent.kde.org/frameworks/syntax-highlighting) (editor and highlighting,
  including the "VSCodium Dark" theme), [Konsole](https://invent.kde.org/utilities/konsole) (terminal),
  [KParts](https://invent.kde.org/frameworks/kparts), KXmlGui, KCoreAddons, KDBusAddons (single instance),
  KWindowSystem (window activation on Wayland). LGPL/GPL licences.
- **The Qt Company and contributors**: [Qt 6](https://www.qt.io/).
- **Andrew Gallant (BurntSushi)**: [ripgrep](https://github.com/BurntSushi/ripgrep), used for search and `Ctrl+P`.
- **vscode-icons team**: [vscode-icons](https://github.com/vscode-icons/vscode-icons) (MIT code, CC BY-SA icons).
  The icons are not included in Lieve: they are read from the extension installed in VS Code.
  The same goes for [Material Icon Theme](https://github.com/material-extensions/vscode-material-icon-theme)
  by Philipp Kief, if that is the chosen theme.
- **Git** (Linus Torvalds, Junio C Hamano and contributors, GPLv2): Lieve uses the installed `git` command.
- **Prettier** (James Long and contributors, MIT) and **Node.js**: Lieve uses the Prettier already installed
  in the project and does not bundle a copy.
- **Microsoft**: language definitions (file extensions) are read from the built-in extensions of the
  installed [Visual Studio Code](https://github.com/microsoft/vscode).
- Layout and shortcuts are inspired by Visual Studio Code.
