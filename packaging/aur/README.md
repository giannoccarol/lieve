# AUR packages

- `lieve/`: the latest release, built from its GitHub tarball.
- `lieve-git/`: the current `main` branch.

Both build with `make PACKAGED=1` (Lieve's own updater is turned off: pacman or the AUR helper
updates it) and install into `/usr`.

## First publication (once, by the maintainer)

1. Create an account on <https://aur.archlinux.org> and add an SSH public key to it.
2. Publish each package:

   ```bash
   git clone ssh://aur@aur.archlinux.org/lieve.git aur-lieve
   cp packaging/aur/lieve/PKGBUILD packaging/aur/lieve/.SRCINFO aur-lieve/
   cd aur-lieve && git add PKGBUILD .SRCINFO && git commit -m "Initial upload" && git push
   ```

   The same with `lieve-git`.

## Every release

The release workflow updates the `lieve` AUR package by itself when the repository has a secret
named `AUR_SSH_PRIVATE_KEY` (the private key whose public half is on the AUR account): it sets
`pkgver`, the tarball checksum and `.SRCINFO`, and pushes. `lieve-git` needs no update.

To check a PKGBUILD locally: `makepkg -f` in a copy of its folder.
