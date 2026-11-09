#!/usr/bin/env bash
# Installs or updates Lieve by building it from source, into ~/.local (no sudo).
#
#   bash install.sh            latest release
#   bash install.sh v0.1.3     a specific release
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
[ -d /usr/include/KF6/KTextEditor ] || missing+=("KDE Frameworks 6 (KTextEditor, KParts)")
[ -d /usr/include/KF6/BreezeIcons ] || missing+=("Breeze icons library (headers)")
command -v rg >/dev/null 2>&1 || missing+=("ripgrep (for search)")

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}"
    echo
    echo "Arch / CachyOS / Manjaro:"
    echo "  sudo pacman -S --needed base-devel git qt6-base qt6-svg qt6-tools ktexteditor kparts kdbusaddons kwindowsystem syntax-highlighting ki18n breeze-icons konsole ripgrep"
    echo "Fedora:"
    echo "  sudo dnf install gcc-c++ make git pkgconf qt6-qtbase-devel qt6-qtsvg-devel qt6-linguist kf6-ktexteditor-devel kf6-kparts-devel kf6-kxmlgui-devel kf6-kdbusaddons-devel kf6-kwindowsystem-devel kf6-syntax-highlighting-devel kf6-ki18n-devel 'cmake(KF6BreezeIcons)' konsole ripgrep"
    echo "Debian 13 / Ubuntu 25.04 and later:"
    echo "  sudo apt install build-essential git pkgconf qt6-base-dev qt6-svg-dev qt6-l10n-tools libkf6texteditor-dev libkf6parts-dev libkf6xmlgui-dev libkf6dbusaddons-dev libkf6windowsystem-dev libkf6syntaxhighlighting-dev libkf6i18n-dev libkf6breezeicons-dev konsole ripgrep"
    echo
    echo "konsole is optional: without it Lieve's terminals open in your terminal app."
    exit 1
fi

qtbin="$(pkg-config --variable=bindir Qt6Core 2>/dev/null || true)"
if ! { [ -x "$qtbin/lrelease" ] || command -v lrelease-qt6 >/dev/null 2>&1 || command -v lrelease >/dev/null 2>&1; }; then
    echo "Note: Qt's lrelease was not found (package qt6-tools, or qt6-linguist on Fedora):"
    echo "Lieve will be built without translations, in English only."
    echo
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
[ -n "$TAG" ] || { echo "No release found."; exit 1; }

echo "Installing Lieve $TAG into $PREFIX ..."
git checkout --quiet --force "$TAG"
make -j"$(nproc)"
make install PREFIX="$PREFIX"

echo
echo "Done. Start Lieve from the application menu or with: $PREFIX/bin/lieve"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "(To use the 'lieve' command, add $PREFIX/bin to your PATH.)" ;;
esac
echo "Later updates come from Lieve itself: an update button appears in the status bar."
