#!/usr/bin/env bash
# Makes data/lieve.ico, the icon of lieve.exe (data/lieve.rc), from
# data/lieve.svg: rsvg-convert renders each size, ImageMagick puts them in one
# file (the small sizes as bitmaps, 256 as PNG, as Windows expects). Run it
# again after changing the SVG and commit the result: the build uses the .ico.
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for size in 16 24 32 48 64 128 256; do
    rsvg-convert -w "$size" -h "$size" data/lieve.svg -o "$tmp/$size.png"
done
magick "$tmp"/{16,24,32,48,64,128,256}.png -define icon:png-compression-size=256 data/lieve.ico
