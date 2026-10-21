#!/bin/bash
# usage: tools/make_android_apk.sh <build dir> <out .apk> <deps dir> <Android SDK> <min API>
#        tools/make_android_apk.sh --install <apk>          (adb install; ADB_SERIAL=<serial> picks the device)
# (make TARGET=android-arm64 GAME=sm35 apk / install run these.)
#
# The APK around libmain.so (the runtime) and lib<id>game.so (the game code, compiled on this machine from your own
# dump): SDL's Java activity (org.libsdl.app.SDLActivity, from <deps>/java) as the launcher, libSDL3.so, a manifest.
# No Gradle: javac, d8, aapt2, zipalign and apksigner from the SDK's build-tools. Signed with a local debug key
# (build/android-debug.keystore, made on first use). The APK holds code made from your dump: it is for your own
# devices only, never to be shared. Game files are not in it (tools/android_push_data.sh puts them on the device).
set -euo pipefail
# names from games/<game>/game.mk (GAME as the Makefile's): the package id, the app's name, its game library
game_var() { sed -n "s/^$1 *:= *//p" "$(dirname "$0")/../games/${GAME:-sm35}/game.mk"; }
PKG=${RECOMP_ANDROID_PACKAGE:-$(game_var GAME_BUNDLE_ID)}
TITLE=$(game_var GAME_TITLE)
GLIB=lib$(game_var GAME_ID)game.so
ADB=${ADB:-$(command -v adb || echo "${ANDROID_HOME:-}/platform-tools/adb")}
if [ "$1" = --install ]; then
    "$ADB" ${ADB_SERIAL:+-s "$ADB_SERIAL"} install -r "$2"
    exit
fi
B=$1 APK=$2 DEPS=$3 SDK=$4 API=$5
TARGET_API=${RECOMP_ANDROID_TARGET_API:-35}
JAR=$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)/android.jar
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
NDK=${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}
STRIP=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)
JAVAC=${JAVAC:-$(if [ -n "${JAVA_HOME:-}" ]; then echo "$JAVA_HOME/bin/javac"; else echo javac; fi)}
VERSION=$(game_var GAME_DISPLAY_VERSION)
CODE=$(( $(date +%s) / 60 ))  # (minutes since 1970: each build installs over the last, whatever the git history)
for f in "$JAR" "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$DEPS/lib/libSDL3.so" "$B/libmain.so" "$B/$GLIB"; do
    [ -e "$f" ] || { echo "make_android_apk: missing $f" >&2; exit 1; }
done
W=$B/apk
rm -rf "$W"
mkdir -p "$W/classes" "$W/dex" "$W/root/lib/arm64-v8a"

cat > "$W/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$PKG"
    android:versionCode="$CODE" android:versionName="$VERSION" android:installLocation="auto">
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x403000" android:required="true" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <!-- extractNativeLibs false: the 0.5+ GB game code library is mapped from the APK, not copied out of it;
         no heap pointer tagging: host pointers stay plain 64-bit values -->
    <application android:label="$TITLE" android:icon="@mipmap/ic_launcher" android:hasCode="true" android:extractNativeLibs="false"
        android:allowNativeHeapPointerTagging="false" android:largeHeap="true" android:isGame="true"
        android:appCategory="game" android:hardwareAccelerated="true"$( [ "${RECOMP_ANDROID_DEBUGGABLE:-0}" = 1 ] && echo ' android:debuggable="true"')>
        <activity android:name="org.libsdl.app.SDLActivity" android:exported="true"
            android:screenOrientation="sensorLandscape" android:launchMode="singleInstance"
            android:theme="@android:style/Theme.NoTitleBar.Fullscreen" android:preferMinimalPostProcessing="true"
            android:configChanges="layoutDirection|locale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
EOF

# SDL's activity glue (loads libSDL3.so and libmain.so, then calls SDL_main)
find "$DEPS/java" -name '*.java' > "$W/sources.txt"
"$JAVAC" -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$JAR" -d "$W/classes" @"$W/sources.txt"
"$BT/d8" --release --min-api "$API" --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')

# the launcher icon in every density (docs/branding/android, tools/make_icons.sh)
mkdir -p "$W/res"
"$BT/aapt2" compile -o "$W/res" "$(cd "$(dirname "$0")/../.." && pwd)"/docs/branding/android/mipmap-*/ic_launcher.png
"$BT/aapt2" link -o "$W/base.apk" --manifest "$W/AndroidManifest.xml" -I "$JAR" \
    --min-sdk-version "$API" --target-sdk-version "$TARGET_API" "$W"/res/*.flat

# native libraries: stored (not compressed) so they can be mapped from the APK; debug info stripped (the build keeps it)
"$STRIP" --strip-unneeded -o "$W/root/lib/arm64-v8a/libSDL3.so" "$DEPS/lib/libSDL3.so"
"$STRIP" --strip-debug -o "$W/root/lib/arm64-v8a/libmain.so" "$B/libmain.so"
"$STRIP" --strip-debug -o "$W/root/lib/arm64-v8a/$GLIB" "$B/$GLIB"
cp "$W/dex/classes.dex" "$W/root/"
(cd "$W/root" && zip -q -0 "../base.apk" lib/arm64-v8a/*.so && zip -q "../base.apk" classes.dex)

"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"   # (-P 16: .so files on 16 KB pages)
KS=$(dirname "$B")/android-debug.keystore
if [ ! -f "$KS" ]; then
    KEYTOOL=$(if [ -n "${JAVA_HOME:-}" ]; then echo "$JAVA_HOME/bin/keytool"; else echo keytool; fi)
    "$KEYTOOL" -genkeypair -keystore "$KS" -storepass android -keypass android -alias recomp -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=$TITLE local build" > /dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$APK" "$W/aligned.apk"
rm -f "$W/base.apk" "$W/aligned.apk" "$APK.idsig"
ls -l "$APK"
