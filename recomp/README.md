# SM35 Recomp

A native port of Super Mario Bros. 35 **v1.0.2** built by **static recompilation** of the player's own dump: the
game's AArch64 code is translated to C ahead of time and compiled for the host, its system calls run on a native
runtime, and its NVN graphics run on Metal (macOS, iOS) or Vulkan (Windows, Linux, Android).

It is a fork of [SMM2 Recomp](https://github.com/smm2recomp/smm2recomp), which is itself a fork of
[LAS Recomp](https://github.com/peiikai/LASRecomp) (MIT): the runtime, recompiler, packaging and setup screen are
shared, the game's glue is `games/sm35/`. What was taken, left out and changed is in [UPSTREAM.md](UPSTREAM.md); the
approach is in `../DECISIONS.md`, status in `../PROGRESS.md`, what the bring-up needed in
[docs/sm35-bringup.md](docs/sm35-bringup.md).

**Nothing from the game is in this repository.** You supply your own dump; the recompiled code, translated shaders
and extracted data are generated on your machine into `private/`, `generated/` and `build/`, which git ignores and
the repo's proprietary-content check blocks.

## Layout

| Path | What |
|---|---|
| `tools/` | the recompiler: `gen.py` (a whole NSO module -> C), `lifter.py`, `simd_c.py`, `nso.py`, tests; packaging scripts |
| `runtime/` | the shared runtime: loader, dispatcher, kernel, IPC + services, NVN on Metal / Vulkan, platform layers, setup screen |
| `games/sm35/` | the game's glue: `game.mk` (identity, dump paths), `setup.py` (lays out the dump), `sysdata.py`, `versions.py`, patches, HLE list |
| `third_party/` | Dear ImGui (setup screen, overlay menu), SMAA, bcdec, Mbed TLS (the game's ssl service), puff (inflate: Nextendo's event zip) |
| `docs/` | findings from the bring-up (names and behaviour only, no game code) |
| `private/`, `generated/`, `build/` | made from your dump (ignored; may be symlinks to another disk) |

## Building (macOS, Apple Silicon)

Requirements: Xcode command line tools (clang), Homebrew `sdl3`, Python 3 (3.12+), a few GB of disk for the
generated C and the build. No FFmpeg: like SMM2, the game has the movie library (`subsdk1`) linked but plays no
movies, so `games/sm35/game.mk` says `GAME_MOVIES := 0` and the movie HLE isn't built.

You need your own extracted dump: the base game's (v1.0.0) RomFS and the v1.0.2 update's ExeFS (`main`, `sdk`,
`subsdk0`, `subsdk1`, `rtld`, `main.npdm`) and RomFS. By default `make` looks in `~/smm-work/sm35/` (`base-romfs`,
`v101-exefs`, `v101-romfs`); pass `DUMP_BASE_ROMFS=`, `DUMP_UPDATE_EXEFS=`, `DUMP_UPDATE_ROMFS=` for other places.

    make -C recomp venv        # once: tools/venv with capstone, unicorn, lz4
    make -C recomp sm35        # setup (checks the v1.0.2 build ids and hashes, links the RomFS), recompile, build
    make -C recomp run-sm35    # play: runs build/sm35/sm35recomp from private/sm35 (saves, settings, caches there)

`make sm35` runs three steps you can also run alone (`make -C recomp GAME=sm35 setup | gen | all`): `setup` lays the
files out in `private/sm35/extracted/` (exefs copied, romfs as links: the update's files over the base game's,
nothing duplicated), `gen` recompiles `sdk` and `main` to C in `generated/sm35/`, and `all` builds the runtime
(`build/sm35/sm35recomp`) and the game code library (`build/sm35/sm35game.dylib`). `subsdk0` (GLSLC, the runtime
shader compiler) and `subsdk1` (the movie library) are checked but not recompiled: the game never calls them.

`make -C recomp GAME=sm35 runtime-objs` compiles the runtime alone (no game files needed).

**System data** (all platforms): the Switch's shared fonts and a few system data archives (the word filter among
them, opened at boot), from your own firmware dump and keys, read in place by `hactool`:

    tools/venv/bin/python games/sm35/sysdata.py --firmware <registered NCA folder> --keys <prod.keys> --data private/sm35

## Building (Linux)

Linux runs on Vulkan: x86-64 (with AVX2: Intel Haswell / AMD Zen or newer) or arm64, a Vulkan 1.3 driver, SDL3 3.2 or
newer, the Vulkan loader and shaderc. The game code is portable C: on arm64 it is the same generated code as macOS
(`generated/sm35`, FP/SIMD as inline asm), on x86-64 the portable-C lowering (`generated/sm35-c`, `--simd c`).
Status: builds (cross-built with zig); SM35 hasn't been run on Linux yet.

**On Linux, from source** (as on macOS; Debian 13 / Ubuntu 25.04 package names):

    sudo apt install clang lld make python3-venv libsdl3-dev libvulkan-dev libshaderc-dev \
                     libvulkan-memory-allocator-dev mesa-vulkan-drivers
    make -C recomp venv sm35       # setup, recompile (x86-64: --simd c), build build/sm35/sm35recomp + sm35game.so
    make -C recomp run-sm35

**Cross-built from macOS** (or any host) with zig, the compiler the player packages bring along:

    recomp/tools/fetch_zig.sh                   # build/zig (or ZIG=<zig>)
    make -C recomp TARGET=linux-aarch64 GAME=sm35 DEPS=<deps> all            # generated/sm35 (as macOS)
    make -C recomp TARGET=linux-x86_64  GAME=sm35 DEPS=<deps> gen all        # generated/sm35-c
    make -C recomp TARGET=linux-aarch64 GAME=sm35 DEPS=<deps> package        # dist/SM35Recomp-linux-aarch64.tar.gz

`DEPS` is a folder with `include/` (SDL3, vulkan, vk_video, shaderc, `vk_mem_alloc.h`) and `lib/` (`libSDL3.so`,
`libvulkan.so`, `libshaderc.so`) for the target (`tools/fetch_deps.sh linux-<arch>` makes it). Outputs go to
`build/sm35-<target>/`; the executable looks for its libraries in the system and in `lib/` next to it.

The data folder (saves, settings, shader caches, the set-up game files) is `~/.local/share/sm35recomp`, or the
starting folder when it has `extracted/` (a source checkout).

## Player packages and the setup screen

Releases are a **launcher plus recompiler** (`../DECISIONS.md`): a package is the runtime with its **setup screen**
(`runtime/ui/launcher.mm`) plus what recompiling the game needs, never the game. Shown in place of the game until the
player's files are set up (and after Quit > Quit to Start Screen, the Mods tab's Game files, or `--setup`), the setup
screen takes:

1. the game: NSP / XCI files (the base game and the 1.0.2 update) or folders with the extracted ExeFS / RomFS;
2. `prod.keys` and the firmware's NCA folder (suggested from Ryujinx / yuzu / their forks when they have them);

and then runs, with progress and an error card per step:

1. `games/sm35/setup.py --source ...`: finds the ExeFS / RomFS in what was chosen, extracts NSP / XCI with hactool and
   the player's keys, checks the ExeFS against `games/sm35/versions.py` (the v1.0.2 build ids and the SHA-256 of the
   ExeFS files: hashes of the player's own files, never the files), and lays out `extracted/`;
2. `games/sm35/sysdata.py`: the shared fonts and system data from the firmware;
3. `runtime/gamebuild.c`: the generator (the package's own Python) and the compiler (the package's zig) build the game
   code library;
4. boots the game in the same window.

The scripts print `@step` / `@progress` / `@found` / `@error` lines the screen follows; keys are only handed to
hactool, and hactool's output is never shown (masked if quoted in an error). What was chosen is kept as paths in
`launcher.ini`. The screen also asks GitHub for the latest release (`RECOMP_VERSION`, the tag the build was made from)
and only says so, with a link; the overlay menu's Mods tab has the switch ("Check for updates") and Game files.

| Package | Made by | Contents |
|---|---|---|
| `SM35Recomp-macos-arm64.zip` | `make GAME=sm35 app-macos` (`tools/bundle_macos.sh`) | `Super Mario 35.app`: the runtime (macOS 15+, SDL3 built for it by `tools/build_deps_macos.sh`), Resources: python, tools, gamelib-src, zig, hactool; signed ad hoc or with `RECOMP_MACOS_IDENTITY` |
| `SM35Recomp-windows-x86_64.zip` | `make TARGET=windows-x86_64 GAME=sm35 DEPS=... package` (`tools/package.sh`) | `sm35recomp.exe`, SDL3 / libiconv / shaderc DLLs, the same tools |
| `SM35Recomp-linux-x86_64.tar.gz`, `-linux-aarch64.tar.gz` | `make TARGET=linux-<arch> GAME=sm35 DEPS=... package` | `sm35recomp`, `lib/libshaderc.so.1`, the same tools |

The pieces, each pinned by version and hash and built or fetched for the target, so every package can be made from
one machine: `tools/fetch_python.sh` (python-build-standalone 3.12 + the wheels in `tools/requirements-gen.txt`),
`tools/fetch_zig.sh`, `tools/build_hactool.sh` (hactool 1.4.0 from source with zig; ISC), `tools/fetch_deps.sh` (the
cross-build headers and link libraries: MSYS2 packages for Windows, the distribution's for Linux). Every package is
checked by `../scripts/check_package.py` before it is archived, and again by the release workflow before upload: no
file of a set-up data folder, no game file by name, magic or the known ExeFS hashes, no key-shaped lines, no
recompiled code, no runtime with game functions linked in.

`.github/workflows/release.yml` builds the four packages only when a `v*` tag is pushed (Windows and Linux cross-built
in a Debian 13 container, macOS on a macOS runner) and attaches them to the tag's release, making a draft release if
there is none. Android and iOS have no package: they are built from source (below).

## Building (iOS / iPadOS)

Not tried for SM35 yet; the build is SMM2 Recomp's. iOS runs no code it didn't ship with, and there is no compiler on
the device, so the game code is built on the Mac from your own dump and linked into the app, which is then signed and
installed on your device. The app is made from your dump: it is for your own devices only, never to be shared.

    make -C recomp TARGET=ios-arm64 GAME=sm35 DEPS=<deps>/ios-device app        # build/sm35-ios-arm64/SM35Recomp.app
    make -C recomp TARGET=iossim-arm64 GAME=sm35 DEPS=<deps>/ios-sim app        # the Simulator (ad-hoc signed)

`DEPS`: SDL3 built for iOS as a static library (cmake with `-DCMAKE_SYSTEM_NAME=iOS -DSDL_SHARED=OFF -DSDL_STATIC=ON`).
Signing: `RECOMP_IOS_IDENTITY=<codesigning identity> RECOMP_IOS_PROFILE=<.mobileprovision>` (and
`RECOMP_IOS_BUNDLE_ID`, default `org.sm35recomp.sm35recomp`). The guest's memory window sits at
0x7000000000-0x7320000000, far above iOS's default address space: the profile must carry
`com.apple.developer.kernel.extended-virtual-addressing` and `com.apple.developer.kernel.increased-memory-limit` (an
explicit App ID with both capabilities; `tools/ios_profile.sh <bundle id> <team id>` has Xcode make one).

Game files go in the app's **Documents** folder (Finder's file sharing for the device, or the Files app):
`extracted/` (exefs + romfs, laid out by `games/sm35/setup.py` on the Mac), `fonts/` and `sysdata/` (sysdata.py). The
log is `Documents/sm35recomp.log`. Or `RECOMP_IOS_DATA=private/sm35` puts them inside the app.

## Building (Android)

Not tried for SM35 yet; the build is SMM2 Recomp's. As on iOS, the game code is compiled **on the Mac** from your own
dump and packaged into the APK as a native library, `libsm35game.so`, next to the runtime (`libmain.so`), SDL3 and
SDL's Java activity. The APK is made from your dump: it is for your own devices only.

    ANDROID_HOME=<sdk> recomp/tools/android_deps.sh <deps>    # once: SDL3, shaderc (the NDK's), VMA for arm64-v8a
    make -C recomp TARGET=android-arm64 GAME=sm35 gen          # the game code for Android's guest window
    make -C recomp TARGET=android-arm64 GAME=sm35 ANDROID_HOME=<sdk> DEPS=<deps> -j apk install push-data

The guest's memory window sits at 0x800000000 on Android (39-bit address spaces), so its game code is generated for
that base (`generated/sm35-0x800000000`, `GUEST_ORIGIN`). Game files go to
`/sdcard/Android/data/org.sm35recomp.sm35recomp/files/` (`make ... push-data`: `tools/android_push_data.sh`); the log
is `sm35recomp.log` there and in logcat under the tag `sm35recomp`.

## Running

Keyboard (LAS Recomp's defaults, remappable in the overlay's Controls tab): WASD left stick, arrows d-pad,
K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus. F1 or Esc opens the overlay menu.

The game boots to its main menu. Everything past it (even Course Practice) needs the game's online service: offline
it shows Loading and goes back to the menu, as on a console without a connection. Online play will go through
Nextendo Network; it isn't implemented yet.

Useful environment variables (inherited from LAS Recomp and SMM2 Recomp): `LARECOMP_WINDOW_SHOT=<frame>` saves the
window at that frame (`LARECOMP_SHOT_PATH=<png>`, `LARECOMP_EXIT_AFTER_SHOT=1`), `LARECOMP_SCREENSHOT_EVERY=<n>`,
`LARECOMP_TRACE_SVC=1`, `LARECOMP_LOG_FILES=1` (RomFS files as they are read), `LARECOMP_NO_AUDIO=1`.
`RECOMP_MOD_DIR=<dir>` serves files from `<dir>/romfs/` over the game's.

| Variable | Effect |
|---|---|
| `RECOMP_LOG_IPC=1` | log every IPC request with its result |
| `RECOMP_PRETEND_ONLINE=1` | (bring-up) report a network connection to the game; it then asks for services that aren't there yet |
| `RECOMP_USER_NAME=<name>` | the account nickname (default "Player") |
| `LARECOMP_CG_SHOT=<frame>` or `<first>:<last>:<step>` | (Metal) capture the window through the window server (colour-managed); with a range, `LARECOMP_SHOT_PATH` is a printf pattern |
| `LARECOMP_NO_SHADER_PREFETCH=1` | build shaders and pipelines on first use, inline |
| `LARECOMP_NO_PIPELINE_CACHE=1` | (Metal) don't use the pipeline cache `cache/pipelines/` in the data folder |
| `LARECOMP_NO_PRECOMPILED=1` | (Metal) don't load precompiled shaders (`cache/metallib/`, `tools/precompile_shaders.sh`) |
| `LARECOMP_DEBUG_FRAME=<n>` | log every draw of frame n and save each render pass to `build/pass_NNN.png` |
| `RECOMP_NO_MOUSE_TOUCH=1` | the left mouse button no longer touches the console's touch screen (desktop) |
| `LARECOMP_HOST_INPUT="<frame>:tap:<x>/<y>:<frames>"` | (tests) a finger on the touch screen at game pixel x, y (1280x720) for that many frames |
| `RECOMP_PAD_SCRIPT=<file>` | play an input script instead of the keyboard / controllers: `N BUTTONS` per line; `RECOMP_PAD_CLOCK=frame` (one line per presented frame) |
| `RECOMP_BC_SOFT=1` | decode BC textures on the CPU, as on GPUs without BC support |
| `RECOMP_NO_CORES=1` | run guest threads without per-core exclusivity |
| `RECOMP_BUILD_JOBS=<n>` | compiler jobs for the setup screen's first-launch recompile |
