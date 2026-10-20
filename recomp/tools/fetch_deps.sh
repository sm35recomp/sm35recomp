#!/bin/bash
# usage: tools/fetch_deps.sh <target> [out]   (windows-x86_64 | linux-x86_64 | linux-aarch64; out: build/deps-<target>)
# The headers and link libraries a cross build of the runtime needs (make TARGET=<target> DEPS=<out>), arranged as
# docs/windows.md and recomp/README.md describe: include/ (SDL3, vulkan, vk_video, shaderc, vk_mem_alloc.h), lib/, and
# for Windows bin/ (the DLLs the package ships).
#   windows-x86_64: MSYS2 UCRT64 packages, pinned by version and sha256 (repo.msys2.org)
#   linux-*:        from this machine's Debian / Ubuntu multiarch packages (the release workflow runs in a Debian 13
#                   container: apt install libsdl3-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev,
#                   with :arm64 for linux-aarch64, libshaderc1:arm64 in place of libshaderc-dev:arm64)
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=$1
OUT=${2:-build/deps-$TARGET}
rm -rf "$OUT" && mkdir -p "$OUT/include" "$OUT/lib"
case "$TARGET" in
windows-x86_64)
    MSYS=https://repo.msys2.org/mingw/ucrt64
    DL=build/dl/msys2
    mkdir -p "$DL" "$OUT/bin"
    X=$(mktemp -d)
    while read -r sha pkg; do
        [ -n "$pkg" ] || continue
        f=mingw-w64-ucrt-x86_64-$pkg-any.pkg.tar.zst
        [ -f "$DL/$f" ] || curl -fsSL --proto '=https' -o "$DL/$f.part" "$MSYS/$f" && { [ -f "$DL/$f" ] || mv "$DL/$f.part" "$DL/$f"; }
        got=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/$f" | cut -d' ' -f1)
        [ "$got" = "$sha" ] || { echo "$f: sha256 $got, expected $sha"; exit 1; }
        if command -v zstd >/dev/null; then zstd -dcq "$DL/$f" | tar -x -C "$X"; else tar -xf "$DL/$f" -C "$X"; fi
    done <<'EOF'
f9f8e2fe9858868e7653b24c973bcc8c19dea1096afc8c905bf1cd1a87960442 sdl3-3.4.18-1
9a500f38c2b91808741c62fae746b3e9110b33a1ecf5c30fa0c66dbedddf7e16 libiconv-1.19-1
a4ad271f76acd2307ed67738770b30d6486bd3c7acff63ba83dd13e56ce9ab82 shaderc-2026.4-1
be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a vulkan-headers-1~1.4.363.0-1
065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe vulkan-loader-1~1.4.363.0-1
44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781 vulkan-memory-allocator-3.4.0-1
EOF
    U=$X/ucrt64
    cp -R "$U/include/SDL3" "$U/include/vulkan" "$U/include/vk_video" "$U/include/shaderc" "$U/include/vma/vk_mem_alloc.h" "$OUT/include/"
    cp "$U/lib/libSDL3.dll.a" "$U/lib/libvulkan-1.dll.a" "$U/lib/libshaderc_shared.dll.a" "$OUT/lib/"
    cp "$U/lib/libSDL3.dll.a" "$OUT/lib/SDL3.lib"  # (zig looks for these names)
    cp "$U/lib/libvulkan-1.dll.a" "$OUT/lib/vulkan-1.lib"
    cp "$U/lib/libshaderc_shared.dll.a" "$OUT/lib/shaderc_shared.lib"
    cp "$U/bin/SDL3.dll" "$U/bin/libiconv-2.dll" "$U/bin/libshaderc_shared.dll" "$OUT/bin/"
    rm -rf "$X"
    ;;
linux-x86_64|linux-aarch64)
    T=$([ "$TARGET" = linux-x86_64 ] && echo x86_64-linux-gnu || echo aarch64-linux-gnu)
    for h in SDL3 vulkan vk_video shaderc vk_mem_alloc.h; do
        [ -e "/usr/include/$h" ] || { echo "/usr/include/$h is missing: apt install libsdl3-dev libvulkan-dev libshaderc-dev libvulkan-memory-allocator-dev"; exit 1; }
        cp -R "/usr/include/$h" "$OUT/include/"
    done
    for l in libSDL3.so libvulkan.so libshaderc.so; do
        # libshaderc.so.1 alone will do (libshaderc1:<arch>): libshaderc-dev can't be installed for two architectures
        s=/usr/lib/$T/$l; [ -e "$s" ] || [ "$l" != libshaderc.so ] || s=$s.1
        [ -e "$s" ] || { echo "/usr/lib/$T/$l is missing (apt install the -dev packages for ${T%%-*})"; exit 1; }
        cp -L "$s" "$OUT/lib/$l"
    done
    ;;
*) echo "fetch_deps.sh: no target $TARGET"; exit 1 ;;
esac
echo "$OUT: $(du -sh "$OUT" | cut -f1)"
