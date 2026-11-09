#!/usr/bin/env bash
# Collects lieve.exe with the DLLs, Qt plugins, translations and ripgrep it
# needs into dist/Lieve: a folder that runs on Windows 10/11 without MSYS2.
# Run in an MSYS2 UCRT64 shell after "make" (see .github/workflows/windows.yml).
set -euo pipefail
cd "$(dirname "$0")/../.."
prefix=${MINGW_PREFIX:-/ucrt64}
out=dist/Lieve
rm -rf dist
mkdir -p "$out"
cp lieve.exe "$out/"
strip "$out/lieve.exe"

# Qt plugins, next to the program (qt.conf below points there).
platforms=$(find "$prefix" -name qwindows.dll -path '*platforms*' | head -n1)
plugins=$(dirname "$(dirname "$platforms")")
echo "Qt plugins: $plugins"
for dir in platforms styles imageformats iconengines tls; do
    [ -d "$plugins/$dir" ] || continue
    mkdir -p "$out/$dir"
    case $dir in
    platforms) cp "$plugins/$dir/qwindows.dll" "$out/$dir/" ;;
    imageformats) cp "$plugins/$dir"/q{svg,ico,jpeg,gif,webp}.dll "$out/$dir/" 2>/dev/null || true ;;
    tls) cp "$plugins/$dir/qschannelbackend.dll" "$out/$dir/" ;; # Windows' own TLS: no OpenSSL to ship
    *) cp "$plugins/$dir"/*.dll "$out/$dir/" ;;
    esac
done

# ripgrep for search and Quick Open: found next to lieve.exe.
cp "$prefix/bin/rg.exe" "$out/"

# Every DLL the program and the plugins load from MSYS2 (ldd lists them with
# their dependencies), copied until nothing new turns up.
while :; do
    added=0
    for dll in $(ldd "$out/lieve.exe" "$out/rg.exe" $(find "$out" -mindepth 2 -name '*.dll') | awk '{print $3}' | grep -i "^$prefix/bin/" | sort -u); do
        name=$(basename "$dll")
        if [ ! -e "$out/$name" ]; then
            cp "$dll" "$out/"
            added=1
        fi
    done
    [ "$added" = 0 ] && break
done

# Translations: Qt's own and the KDE Frameworks' (KI18n looks in data/locale).
qtbase=$(find "$prefix/share" -name 'qtbase_it.qm' | head -n1)
echo "Qt translations: ${qtbase:-not found}"
for lang in it; do
    if [ -n "$qtbase" ] && [ -f "$(dirname "$qtbase")/qtbase_$lang.qm" ]; then
        mkdir -p "$out/translations"
        cp "$(dirname "$qtbase")/qtbase_$lang.qm" "$out/translations/"
    fi
    for domain in ktexteditor6 kxmlgui6 kparts6 kwidgetsaddons6 kconfig6 kcoreaddons6 ki18n6 syntaxhighlighting6 kdbusaddons6 kconfigwidgets6 kcompletion6 sonnet6 kio6; do
        mo=$(find "$prefix/share/locale/$lang" -name "$domain.mo" 2>/dev/null | head -n1)
        if [ -n "$mo" ]; then
            mkdir -p "$out/data/locale/$lang/LC_MESSAGES"
            cp "$mo" "$out/data/locale/$lang/LC_MESSAGES/"
        fi
    done
done
echo "KDE translations: $(ls "$out/data/locale/it/LC_MESSAGES" 2>/dev/null | tr '\n' ' ')"

cat > "$out/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = .
Translations = translations
EOF

cp LICENSE "$out/LICENSE.txt"
echo "Packaged $(du -sh "$out" | cut -f1) in $out"
