#!/bin/bash
# usage: tools/android_deps.sh <deps dir> [work dir]
# Builds what the Android runtime links against, for arm64-v8a, into <deps dir> (the DEPS of
# `make TARGET=android-arm64`), from sources only (nothing from the game):
#   include/SDL3, lib/libSDL3.so    SDL3 (zlib license), CMake + the NDK's toolchain file
#   java/org/libsdl/app/*.java      SDL's Java activity glue (SDLActivity: loads libSDL3.so + libmain.so)
#   include/shaderc, lib/libshaderc_combined.a   shaderc (Apache-2.0) with glslang + SPIRV-Tools, CMake
#   include/vk_mem_alloc.h          Vulkan Memory Allocator (MIT), header only
# Vulkan's headers and libvulkan.so come with the NDK. Needs ANDROID_HOME (or ANDROID_NDK), cmake, ninja, curl, git, python3.
set -euo pipefail
DEPS=$1
WORK=${2:-$DEPS/src}
SDL_VERSION=${SDL_VERSION:-3.4.18}
VMA_VERSION=${VMA_VERSION:-3.3.0}
SHADERC_VERSION=${SHADERC_VERSION:-v2026.1}
API=${ANDROID_API:-33}
NDK=${ANDROID_NDK:-$(ls -d "${ANDROID_HOME:?set ANDROID_HOME (the Android SDK) or ANDROID_NDK}"/ndk/* | sort -V | tail -1)}
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mkdir -p "$DEPS/include" "$DEPS/lib" "$DEPS/java" "$WORK"
DEPS=$(cd "$DEPS" && pwd) WORK=$(cd "$WORK" && pwd)
echo "NDK: $NDK"

# ---- SDL3
if [ ! -f "$DEPS/lib/libSDL3.so" ]; then
    tgz="$WORK/SDL3-$SDL_VERSION.tar.gz"
    [ -f "$tgz" ] || curl -fL -o "$tgz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    rm -rf "$WORK/SDL3-$SDL_VERSION" "$WORK/sdl-build"
    tar -xzf "$tgz" -C "$WORK"
    cmake -S "$WORK/SDL3-$SDL_VERSION" -B "$WORK/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM="android-$API" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_ANDROID_JAR=OFF -DCMAKE_INSTALL_PREFIX="$DEPS" > "$WORK/sdl-cmake.log"
    cmake --build "$WORK/sdl-build" -j"$JOBS" > "$WORK/sdl-build.log"
    cmake --install "$WORK/sdl-build" > "$WORK/sdl-install.log"
    rm -rf "$DEPS/java/org"
    cp -R "$WORK/SDL3-$SDL_VERSION/android-project/app/src/main/java/org" "$DEPS/java/"
fi

# ---- shaderc (GLSL -> SPIR-V at run time, as on Linux / Windows). Built from Google's sources with the glslang /
# SPIRV-Tools its DEPS pins: the NDK's own copy (2022) lacks extensions the shader translator uses
# (GL_EXT_texture_shadow_lod).
if [ ! -f "$DEPS/lib/libshaderc_combined.a" ]; then
    src="$WORK/shaderc-$SHADERC_VERSION"
    if [ ! -d "$src/third_party/glslang" ]; then
        rm -rf "$src"
        git clone -q --depth 1 --branch "$SHADERC_VERSION" https://github.com/google/shaderc "$src"
        (cd "$src" && python3 utils/git-sync-deps)
    fi
    cmake -S "$src" -B "$WORK/shaderc-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON \
        -DENABLE_GLSLANG_BINARIES=OFF > "$WORK/shaderc-cmake.log"
    cmake --build "$WORK/shaderc-build" -j"$JOBS" --target shaderc_combined > "$WORK/shaderc-build.log"
    cp "$WORK/shaderc-build/libshaderc/libshaderc_combined.a" "$DEPS/lib/"
    rm -rf "$DEPS/include/shaderc"
    cp -R "$src/libshaderc/include/shaderc" "$DEPS/include/"
fi

# ---- Vulkan Memory Allocator
if [ ! -f "$DEPS/include/vk_mem_alloc.h" ]; then
    curl -fL -o "$DEPS/include/vk_mem_alloc.h" \
        "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v$VMA_VERSION/include/vk_mem_alloc.h"
fi
echo "deps in $DEPS"
