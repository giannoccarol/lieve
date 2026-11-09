# Changelog

Each version has its own section here: its text becomes the GitHub release notes and is what
Lieve shows when it offers the update.

## v0.1.5

- Windows: Lieve runs on Windows 10/11 (64-bit) from a zip attached to each release, built with MSYS2.
  Terminals and npm scripts open in Windows Terminal (PowerShell, cmd, Git Bash, WSL); Node.js, VS Code
  settings and icons are found where Windows keeps them; Breeze icons and ripgrep are bundled.
- Any Linux desktop: light/dark mode and accent colour from the freedesktop portal (GNOME and others),
  Breeze icons where the desktop's theme lacks them, and without Konsole terminals open in the desktop's
  terminal app. `install.sh` knows Debian 13 / Ubuntu 25.04+; CI builds on Arch, Debian and Fedora.
- `[editor] theme=auto` follows the desktop's light or dark mode (also on Windows).

## v0.1.4

- Find All References (`Shift+F12`) in a popup grouped by file, and Rename Symbol (`F2`) across the project,
  through the language server.
- Reference counts at the end of the line of classes, functions, methods and properties, like Visual Studio's
  CodeLens; click one to list the references (`[lsp] codeLens`).
- Side-by-side diff for the files in the Git panel and the change marks: the file itself on the right
  (editable), changed words highlighted, the changes linked in the middle with stage/revert buttons,
  `Alt+F5`/`Shift+Alt+F5`, switch to inline (`[git] sideBySide`).
- Stage, unstage or revert only the selected lines (`Ctrl+K Ctrl+Alt+S`, `Ctrl+K Ctrl+N`, `Ctrl+K Ctrl+R`),
  also from the right-click menu of a change mark.
- Diff tabs tint whole lines, up to the right edge.

## v0.1.3

- Problems panel (`Ctrl+Shift+M`, or click the count in the status bar): a tab next to the terminals with the
  language servers' problems, grouped by file; click one to jump to it.
- Git: switch and create branches from the status bar (`Git: Checkout to…`), stash (also untracked, apply,
  pop, drop), project and file history with each commit's changes in a read-only tab.
- Merge conflicts: current and incoming changes tinted, "Accept Current | Incoming | Both" on each conflict,
  next/previous and accept-all commands.
- NPM SCRIPTS section in the Explorer: run a `package.json` script in its own terminal tab (npm, pnpm, yarn or
  bun), `Tasks: Run npm Script…` and `Tasks: Rerun Last Script`.
- Breadcrumbs above the editor (folders, file, symbols around the cursor, with dropdowns) and sticky scroll
  (the first lines of the blocks you are in stay at the top), from the language server or from the text.
- Markdown preview (`Ctrl+Shift+V`, to the side with `Ctrl+K V`), drawn by Qt without a web engine, updated
  as you type and scrolling with the editor.
- Rounded selection and hover in the side bar lists and pickers.

## v0.1.2

- Command Palette (`Ctrl+Shift+P`, `F1`, or `>` in Quick Open) with VS Code-style categories, shortcuts
  and recently used commands; `Ctrl+G` opens Quick Open with `:` to go to a line.
- Replace in files (`Ctrl+Shift+H`): inline preview, Replace All, per file and per line; open files are
  changed in the editor as one undo step.
- Editor groups side by side (`Ctrl+\`, `Ctrl+1/2/3`).
- Language servers (LSP), when installed: Go to Definition (`F12`, `Ctrl+click`), problems underlined with
  a count in the status bar (`F8`), hover, completion with auto-imports, Go to Symbol in File
  (`Ctrl+Shift+O`). TypeScript via `typescript-language-server`; Angular templates opt-in.
- Git change marks next to the line numbers (added, modified, deleted), compared with the staged version;
  click one for the diff, `Alt+F3`/`Shift+Alt+F3` to move between them.
- ESLint "fix all" on save, before Prettier, when the project's VS Code settings ask for it.
- Hot exit: unsaved files are kept when closing, quitting or restarting, and backed up while typing.
- Settings window (`Ctrl+,`) with search and categories, saving to `lieve.conf` without losing its comments.
- AUR packages (`packaging/aur`), tests (`make check`) run by CI before every build and release.

## v0.1.1

- English and Italian interface: Lieve follows the system language (or `[ui] language=en|it`),
  English when there is no translation. New languages can be added with Qt Linguist (see the README).
- Inline blame shows up at once: git blame runs once per file in the background and edits are
  followed in memory, instead of running git on every line change.
- Rounded window corners, like Plasma's own windows (square when maximised).
- The Prettier helper process uses about half the memory (about 150 MB instead of 300 MB on a
  large project), formatting just as fast.
- Choose the terminal shell: new terminals run your login shell (e.g. fish) instead of `$SHELL`;
  the ⌄ button next to + opens a terminal with any installed shell or sets the default one
  (`[terminal] shell=`).

## v0.1.0

First public release.

- KTextEditor editor (the same as Kate) with the "VSCodium Dark" theme, `.editorconfig` and the main
  VS Code shortcuts.
- Explorer with VS Code file icons (read from the extensions already installed), search in files with
  ripgrep, Quick Open `Ctrl+P`.
- Built-in Konsole terminal with the same colours as the editor.
- Multiple projects: project picker at startup, one window per project in a single process,
  `Ctrl+R` to switch project, recent projects in the taskbar menu.
- Prettier on save following VS Code's rules (`editor.formatOnSave`, `.prettierrc`, plugins).
- Git panel: stage, commit, sync, coloured diffs, inline and status bar blame.
- VS Code-like look: rounded cards, scrolling pill tabs, title bar with search, status bar,
  burgundy accent.
- In-app updates: Lieve tells you when a new version is out and installs it with one click.
