#!/bin/bash
# usage: tools/android_push_data.sh [--internal] <data dir> [what ...]     (make TARGET=android-arm64 push-data runs it)
# Copies your game files from <data dir> (private/sm35: extracted/ as `make setup` laid it out, fonts/ and sysdata/
# from games/<game>/sysdata.py) to the app's folder on the device adb sees, /sdcard/Android/data/<package>/files.
# The RomFS links are followed (the device gets the files). <what>: which of extracted, fonts, sysdata, save (default:
# the first three; save/ is only copied when named). ADB_SERIAL=<serial> picks the device.
# --internal: into the app's internal folder (/data/data/<package>/files) as the app itself, through run-as - for a
# debuggable test build (RECOMP_ANDROID_DEBUGGABLE=1). On Android 11+ the app can't read what adb puts in the
# external folder (Permission denied); the runtime uses the internal folder when the game files are there.
set -euo pipefail
INTERNAL=0
[ "${1:-}" = --internal ] && { INTERNAL=1; shift; }
DATA=$1
shift
WHAT=("$@")
[ ${#WHAT[@]} -gt 0 ] || WHAT=(extracted fonts sysdata)
PKG=${RECOMP_ANDROID_PACKAGE:-$(sed -n "s/^GAME_BUNDLE_ID *:= *//p" "$(dirname "$0")/../games/${GAME:-sm35}/game.mk")}
ADB=${ADB:-$(command -v adb || echo "${ANDROID_HOME:-}/platform-tools/adb")}
ADB=("$ADB" ${ADB_SERIAL:+-s "$ADB_SERIAL"})
DEST=/sdcard/Android/data/$PKG/files
ITEMS=()
for w in "${WHAT[@]}"; do
    case $w in
        extracted) ITEMS+=(extracted/exefs extracted/romfs) ;;
        fonts|sysdata|save) ITEMS+=("$w") ;;
        *) echo "android_push_data: unknown item $w" >&2; exit 1 ;;
    esac
    [ -e "$DATA/${ITEMS[${#ITEMS[@]}-1]}" ] || { echo "android_push_data: $DATA/${ITEMS[${#ITEMS[@]}-1]} is missing" >&2; exit 1; }
done
if [ $INTERNAL = 1 ]; then
    DEST=/data/data/$PKG/files
    echo "copying ${ITEMS[*]} to $DEST as the app (the RomFS: a few hundred MB)"
    COPYFILE_DISABLE=1 tar -C "$DATA" -chf - --no-xattrs --exclude .DS_Store "${ITEMS[@]}" |
        "${ADB[@]}" exec-in run-as "$PKG" sh -c "mkdir -p '$DEST' && cd '$DEST' && tar -xf -"
    "${ADB[@]}" shell run-as "$PKG" du -sh "$DEST"
    exit 0
fi
"${ADB[@]}" shell mkdir -p "$DEST"
echo "copying ${ITEMS[*]} to $DEST (the RomFS: a few hundred MB)"
# one tar stream, links followed (-h); no macOS extended attributes / AppleDouble files
COPYFILE_DISABLE=1 tar -C "$DATA" -chf - --no-xattrs --exclude .DS_Store "${ITEMS[@]}" |
    "${ADB[@]}" exec-in sh -c "cd '$DEST' && tar -xf -"
# adb's files belong to the shell user, with folders only it and its group may enter: open them to the app (which
# reads them and writes save/; nothing else on the device can reach Android/data/<package>)
"${ADB[@]}" shell "cd '$DEST' && chmod -R a+rwX ${ITEMS[*]}"
"${ADB[@]}" shell du -sh "$DEST"
