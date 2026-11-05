#!/usr/bin/env bash
# Installa o aggiorna Lieve compilandolo dai sorgenti, in ~/.local (nessun sudo).
#
#   bash install.sh            ultima release
#   bash install.sh v0.1.3     una release precisa
#   PREFIX=/opt/lieve bash install.sh
set -euo pipefail

REPO="https://github.com/giannoccarol/lieve.git"
SRC="${XDG_DATA_HOME:-$HOME/.local/share}/lieve/source"
PREFIX="${PREFIX:-$HOME/.local}"

missing=()
for tool in git make g++ pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
pkg-config --exists Qt6Widgets Qt6DBus Qt6Network 2>/dev/null || missing+=("Qt 6 (widgets, dbus, network)")
[ -d /usr/include/KF6/KTextEditor ] || missing+=("KDE Frameworks 6: KTextEditor e KParts")
command -v konsole >/dev/null 2>&1 || missing+=("konsole (per il terminale integrato)")
command -v rg >/dev/null 2>&1 || missing+=("ripgrep (per la ricerca)")

if [ ${#missing[@]} -gt 0 ]; then
    echo "Mancano: ${missing[*]}"
    echo
    echo "Arch / CachyOS / Manjaro:"
    echo "  sudo pacman -S --needed base-devel git qt6-base qt6-svg ktexteditor kparts kdbusaddons kwindowsystem syntax-highlighting konsole ripgrep"
    echo "Fedora:"
    echo "  sudo dnf install gcc-c++ make git qt6-qtbase-devel qt6-qtsvg-devel kf6-ktexteditor-devel kf6-kparts-devel kf6-kdbusaddons-devel kf6-kwindowsystem-devel kf6-syntax-highlighting-devel konsole ripgrep"
    exit 1
fi

if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone --quiet "$REPO" "$SRC"
fi
cd "$SRC"
git fetch --quiet --tags --force origin

TAG="${1:-}"
if [ -z "$TAG" ]; then
    TAG="$(git tag --list 'v*' --sort=-version:refname | head -n1)"
fi
[ -n "$TAG" ] || { echo "Nessuna release trovata."; exit 1; }

echo "Installo Lieve $TAG in $PREFIX ..."
git checkout --quiet --force "$TAG"
make -j"$(nproc)"
make install PREFIX="$PREFIX"

echo
echo "Fatto. Avvia Lieve dal menu delle applicazioni oppure con: $PREFIX/bin/lieve"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "(Per usare il comando 'lieve' aggiungi $PREFIX/bin al PATH.)" ;;
esac
echo "Gli aggiornamenti successivi arrivano da Lieve stesso: compare \"Aggiornamento\" nella barra di stato."
