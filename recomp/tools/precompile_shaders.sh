#!/bin/bash
# usage: tools/precompile_shaders.sh <out dir> <macos | ios> <shaders dir>...
# Compiles the translated shaders runs saved (<data folder>/cache/shaders/*.metal) with Metal's offline compiler into
# one .metallib each, which the Metal back-end loads instead of compiling the source on first use - the front-end
# compile, most of a first use's cost (runtime/nvn/renderer.m precompiled_library). macOS: out dir
# <data folder>/cache/metallib. iOS: tools/make_ios_app.sh does it and puts them in the app (data/metallib).
# Shaders are the game's, translated: like the rest of the data folder they stay on your machine and devices.
# Only shaders not compiled yet are compiled; files are named by the source's hash, so the ones a newer translator no
# longer makes are just never used.
set -euo pipefail
OUT=$1 PLAT=$2
shift 2
case "$PLAT" in
    macos) sdk=macosx; min=${MACOS_MIN:+-mmacosx-version-min=$MACOS_MIN} ;;
    ios) sdk=iphoneos; min=-mios-version-min=${IOS_MIN:-17.0} ;;
    *) echo "precompile_shaders: platform must be macos or ios" >&2; exit 2 ;;
esac
metal=$(xcrun -sdk "$sdk" -f metal 2>/dev/null) || {
    echo "precompile_shaders: no Metal compiler (xcodebuild -downloadComponent MetalToolchain)" >&2; exit 1; }
mkdir -p "$OUT"
todo=()
for src in "$@"; do
    [ -d "$src" ] || { echo "precompile_shaders: $src: no such folder" >&2; continue; }
    for f in "$src"/*.metal; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .metal)
        [ -s "$OUT/$b.metallib" ] || todo+=("$f")
    done
done
echo "precompile_shaders: ${#todo[@]} to compile ($PLAT) into $OUT"
if [ ${#todo[@]} -gt 0 ]; then
    export metal min OUT
    # As the runtime compiles them: the entry point main0 renamed after the file (v_/f_/c_<hash>), and vertex
    # shaders with preserveInvariance (position invariance across passes). (A shader in two folders: same name, same
    # source, compiled twice at worst.)
    printf '%s\0' "${todo[@]}" | xargs -0 -n 1 -P "$(sysctl -n hw.ncpu)" sh -c '
        b=$(basename "$1" .metal); inv=""
        case "$b" in v_*) inv=-fpreserve-invariance ;; esac
        sed "s/ main0(/ $b(/" "$1" | "$metal" -w $min $inv -x metal -o "$OUT/$b.metallib.$$" - &&
            mv "$OUT/$b.metallib.$$" "$OUT/$b.metallib" ||
            { rm -f "$OUT/$b.metallib.$$"; echo "precompile_shaders: $b failed" >&2; }' sh
fi
echo "precompile_shaders: $(find "$OUT" -name '*.metallib' | wc -l | tr -d ' ') precompiled shaders in $OUT"
