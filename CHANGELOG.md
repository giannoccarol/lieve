# Changelog

Each version has its own section here: its text becomes the GitHub release notes and is what
Lieve shows when it offers the update.

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
