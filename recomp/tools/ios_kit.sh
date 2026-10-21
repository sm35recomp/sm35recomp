#!/bin/bash
# usage: tools/ios_kit.sh --team <Apple team id> [--bundle <bundle id>] [--data <folder>] [--identity <name or hash>]
#
# Make your own SM35 Recomp for iPhone / iPad, on a Mac, from your own copy of the game. Nothing of the game is in
# this repository or in any download: the game code is recompiled here from the files the Mac app set up.
#
# Before: the Mac app ("Super Mario 35") set up once ("Set up and play"), so its data folder holds your game files
# (default: ~/Library/Application Support/org.sm35recomp.sm35recomp). Xcode, signed in to your Apple ID (Settings >
# Accounts), with your device connected once so Xcode registers it. Homebrew's cmake and xcodegen.
# The team id is the 10-character one Xcode shows for your account (Settings > Accounts > the team).
#
# Out, in build/ios-kit/: SM35Recomp.ipa, and Documents/ - the game files to copy into the app (Finder: select the
# device > Files > SM 35, drag the contents of Documents/ onto it). Install the .ipa from Finder too (drag it
# onto the device), or with `xcrun devicectl device install app --device <name> build/sm35-ios-arm64/SM35Recomp.app`.
#
# The app needs two entitlements in its provisioning profile: extended virtual addressing (the game's memory window)
# and the increased memory limit. tools/ios_profile.sh asks Xcode for a profile that has them; an account that
# can't get them makes an app that installs but stops at start ("guest memory not reserved").
# A paid Apple Developer account is known to work. A free account is untested: its apps expire after 7 days, and it
# may not get those entitlements. Reports welcome (GitHub Issues).
set -euo pipefail
cd "$(dirname "$0")/.."
TEAM= BUNDLE= IDENTITY=
DATA="$HOME/Library/Application Support/org.sm35recomp.sm35recomp"
while [ $# -gt 0 ]; do
    case "$1" in
    --team) TEAM=$2; shift 2 ;;
    --bundle) BUNDLE=$2; shift 2 ;;
    --data) DATA=$2; shift 2 ;;
    --identity) IDENTITY=$2; shift 2 ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
    esac
done
[ -n "$TEAM" ] || { echo "ios_kit: --team <your Apple team id> is needed (Xcode > Settings > Accounts)"; exit 2; }
BUNDLE=${BUNDLE:-org.sm35recomp.sm35recomp.$(echo "$TEAM" | tr 'A-Z' 'a-z')}
for f in exefs/main romfs; do
    [ -e "$DATA/extracted/$f" ] || { echo "ios_kit: no game files in $DATA/extracted: set the game up in the Mac app first, or pass --data"; exit 1; }
done
command -v xcodebuild >/dev/null || { echo "ios_kit: needs Xcode"; exit 1; }
step() { printf '\n== %s\n' "$*"; }

# The build links into a folder with no spaces (the Makefile's flags don't quote paths)
LINK=build/ios-kit/data
mkdir -p build/ios-kit
rm -f "$LINK" && ln -s "$DATA" "$LINK"

step "Python for the recompiler"
[ -x tools/venv/bin/python ] || make venv
step "Recompiling the game code from $DATA (a few minutes)"
make GAME=sm35 DATA="$LINK" gen
step "SDL3 for iOS"
[ -f build/deps-ios-arm64/lib/libSDL3.a ] || tools/fetch_deps.sh ios-arm64 build/deps-ios-arm64
step "Provisioning profile for $BUNDLE (team $TEAM)"
PROFILE=$(tools/ios_profile.sh "$BUNDLE" "$TEAM" "build/ios-profiles/$BUNDLE.mobileprovision" | tail -1)
PROFILE="$(cd "$(dirname "$PROFILE")" && pwd)/$(basename "$PROFILE")"
if [ -z "$IDENTITY" ]; then
    IDENTITY=$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | awk '{print $2}')
    [ -n "$IDENTITY" ] || { echo "ios_kit: no \"Apple Development\" signing identity: open Xcode once with your account"; exit 1; }
fi
step "Building the app (10-20 minutes the first time)"
make -j"$(sysctl -n hw.ncpu)" TARGET=ios-arm64 GAME=sm35 DEPS=build/deps-ios-arm64 RECOMP_IOS_IDENTITY="$IDENTITY" \
    RECOMP_IOS_PROFILE="$PROFILE" RECOMP_IOS_BUNDLE_ID="$BUNDLE" app
APP=build/sm35-ios-arm64/SM35Recomp.app
SIG=$(codesign -dv "$APP" 2>&1 || true)
case "$SIG" in *"TeamIdentifier=$TEAM"*) ;; *) echo "ios_kit: the app is not signed for team $TEAM"; exit 1 ;; esac

step "Packaging"
rm -rf build/ios-kit/Payload build/ios-kit/SM35Recomp.ipa build/ios-kit/Documents
mkdir -p build/ios-kit/Payload build/ios-kit/Documents
cp -R "$APP" build/ios-kit/Payload/
(cd build/ios-kit && zip -qry SM35Recomp.ipa Payload && rm -rf Payload)
for d in extracted fonts sysdata; do cp -RL "$DATA/$d" build/ios-kit/Documents/; done
rm -f build/ios-kit/Documents/extracted/*.dylib build/ios-kit/Documents/extracted/*.dylib.id  # (the Mac's game code)
rm -f "$LINK"
step "Done"
echo "build/ios-kit/SM35Recomp.ipa: install it (Finder: drag it onto your device)."
echo "build/ios-kit/Documents/: copy its contents into the app (Finder > your device > Files > SM 35)."
echo "These hold your game: keep them to yourself."
