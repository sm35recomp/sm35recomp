#!/bin/bash
# usage: tools/package.sh <target> <build dir> <game id> <deps dir>   (make package TARGET=... does this)
# A player package for windows-x86_64 (a .zip), linux-x86_64 or linux-aarch64 (a .tar.gz): the runtime with its setup
# screen, what recompiling the game on the player's machine needs, and nothing from the game. On first launch the
# setup screen (runtime/ui/launcher.mm) takes the player's own dump and keys, checks and extracts it, recompiles the game
# code (runtime/gamebuild.c: the bundled Python runs the generator, the bundled zig compiles it) and runs the game.
#   <game id>recomp[.exe]   the runtime
#   <game id>recomp.dll.a   (Windows) its import library: the game code DLL links against the exe
#   *.dll                   (Windows) SDL3 + libiconv, shaderc (Vulkan itself comes with the graphics driver)
#   lib/libshaderc.so.1     (Linux) shaderc; SDL3 and the Vulkan loader come from the distribution
#   python/ tools/ gamelib-src/ zig/ hactool/ licenses/ THIRD-PARTY.txt (tools/package_lib.sh)
#   README.txt              what to do; the full text is the repository's README.md
# The package is checked for game content (../scripts/check_package.py) before it is archived.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/package_lib.sh
TARGET=$1 B=$2 ID=$3 DEPS=${4:-}
VERSION=${RECOMP_VERSION:-$(git describe --tags --match 'v[0-9]*' 2>/dev/null || echo dev)}
OS=${TARGET%%-*}
EXE=$B/${ID}recomp$([ "$OS" = windows ] && echo .exe || true)
[ -f "$EXE" ] || { echo "no $EXE: make TARGET=$TARGET all first"; exit 1; }
NAME="$APP_NAME-$TARGET"
OUT=dist/$NAME
rm -rf "$OUT" "dist/$NAME.zip" "dist/$NAME.tar.gz"
mkdir -p "$OUT"
cp "$EXE" "$OUT/"
if [ "$OS" = windows ]; then
    cp "$B/${ID}recomp.dll.a" "$OUT/"
    for d in SDL3.dll libiconv-2.dll libshaderc_shared.dll; do cp "$DEPS/bin/$d" "$OUT/"; done
    cp ../docs/branding/app-icon.ico "$OUT/$APP_NAME.ico"  # (also in the exe: runtime/platform/windows.rc)
else
    mkdir -p "$OUT/lib"
    cp -L "$DEPS/lib/libshaderc.so" "$OUT/lib/libshaderc.so.1"
    # the app icon and a menu entry: install-desktop-entry.sh writes it to ~/.local/share/applications with this
    # folder's absolute paths (a .desktop file can't name files relative to itself)
    X=${ID}recomp
    cp ../docs/branding/app-icon-256.png "$OUT/$X.png"
    printf '%s\n' '[Desktop Entry]' 'Type=Application' "Name=$GAME_TITLE" \
        'Comment=Super Mario Bros. 35, recompiled from your own dump' "Exec=@DIR@/$X" 'Path=@DIR@' \
        "Icon=@DIR@/$X.png" 'Terminal=false' 'Categories=Game;' > "$OUT/$X.desktop"
    printf '%s\n' '#!/bin/sh' "# Adds $GAME_TITLE to the desktop menu (with its icon), pointing at this folder" 'set -e' \
        'dir=$(cd "$(dirname "$0")" && pwd)' 'mkdir -p "$HOME/.local/share/applications"' \
        "sed \"s|@DIR@|\$dir|g\" \"\$dir/$X.desktop\" > \"\$HOME/.local/share/applications/$X.desktop\"" \
        "echo \"Added: \$HOME/.local/share/applications/$X.desktop\"" > "$OUT/install-desktop-entry.sh"
    chmod +x "$OUT/install-desktop-entry.sh"
fi
ZT=$([ "$TARGET" = windows-x86_64 ] && echo x86_64-windows || echo "${TARGET#*-}-linux")
stage_common "$OUT" "$TARGET" "$ZT"
write_readme "$OUT/README.txt" "$TARGET" "$VERSION"
python3 ../scripts/check_package.py "$OUT"
if [ "$OS" = windows ]; then
    (cd dist && zip -qr -9 "$NAME.zip" "$NAME")
    ARCHIVE=dist/$NAME.zip
else
    (cd dist && COPYFILE_DISABLE=1 tar -czf "$NAME.tar.gz" "$NAME")
    ARCHIVE=dist/$NAME.tar.gz
fi
echo "$ARCHIVE: $(du -h "$ARCHIVE" | cut -f1) ($(du -sh "$OUT" | cut -f1) unpacked), $VERSION"
