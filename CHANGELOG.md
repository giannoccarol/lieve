# Changelog

Each version has its own section here: its text becomes the GitHub release notes and is what
Lieve shows when it offers the update.

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
