# Upstream projects

SM35 Recomp is forked from **SMM2 Recomp** (https://github.com/smm2recomp/smm2recomp, GPL-3.0-or-later): its shared
runtime, recompiler, packaging and setup screen, retargeted from *Super Mario Maker 2* v3.0.3 to *Super Mario Bros.
35* v1.0.2 (`games/sm35/`). SMM2's game folder, its mods, its course server and its course-format tools were left
behind. SMM2 Recomp itself is a fork of **LAS Recomp**, retargeted from *Link's Awakening* and reorganised toward a
shared runtime plus per-game glue; ideas (and, where noted, files) come from **SuperMarioWonderRecomp**. Both are
MIT; their notices are kept next to this file. The sections below describe the LAS fork as SMM2 Recomp made it.

| Project | Upstream | Commit forked / read | License (here) |
|---|---|---|---|
| SMM2 Recomp | https://github.com/smm2recomp/smm2recomp | main, 2026-10-20 | GPL-3.0-or-later (as this project) |
| LAS Recomp | https://github.com/peiikai/LASRecomp | `e9245915d84df3b43a8502fbc7cf5f2292335c20` (main, 2026-10) | MIT, `LICENSE-LASRecomp` |
| SuperMarioWonderRecomp ("SwitchGamesRecomp") | https://github.com/BelmanteGu/SuperMarioWonderRecomp | `010ed7a596a621c15d3a8dfa5795ca8c3668b066` (main, 2026-10) | MIT; no files taken yet (layout ideas only) |

Third-party code that came with the LAS fork keeps its own license: `third_party/imgui` (MIT, trimmed to the core and
the SDL3 / Metal / Vulkan backends) and `third_party/smaa` (MIT-style, `third_party/smaa/LICENSE.txt`). Added here:
`third_party/bcdec` (bcdec by Sergii Kudlai, MIT or Unlicense, `third_party/bcdec/LICENSE`): BC texture decoding on
the CPU for GPUs without BC support (the iOS Simulator). `third_party/mbedtls` (Mbed TLS 3.6.7 LTS, Apache-2.0 OR
GPL-2.0-or-later, taken under Apache-2.0; `third_party/mbedtls/LICENSE`, `README.recomp`): TLS for the game's own
ssl service (`runtime/services/ssl.c`), sources unmodified.

## What was taken from LAS Recomp

- **Recompiler** (`tools/`): `gen.py` (whole-module AArch64 -> C, function discovery, Unicorn-resolved jump tables),
  `lifter.py`, `analyze.py` (eh_frame), `nso.py`, `simd_c.py` (portable FP/SIMD lowering), `passes.py`, the patch
  mechanism (`patches.py`, now empty: games bring theirs via `--patches`), the game-library entry and the lifter /
  SIMD / shader / ASTC tests, `fetch_zig.sh` / `zig_slim.sh` (the compiler players' machines build with).
- **Runtime** (`runtime/`): loader, dispatcher, kernel (threads, sync, memory, SVCs), HIPC/CMIF IPC, services (acc,
  am/applet, apm, lm, audio, fs, hid, set, time, pctl, aoc), HLE of nn::vi / nv / pl / swkbd / lmem heaps / hid
  input / movie, NVN on the host (`runtime/nvn`: objects, command buffers, Maxwell shader translator, ASTC, Metal and
  Vulkan backends, SMAA, output scaling), platform layers (macOS, Linux, Windows), paths / settings / game library
  loading, the in-game overlay menu (Video, Audio, Controls, Quit) and its on-screen keyboard.
- **Build** (`Makefile`): the same compile rules, now parameterised by `GAME` (games/<game>/game.mk).

## What was left out (Link's Awakening only)

- The randomizer (`third_party/LAS-Randomizer`, `runtime/rando.*`, `tools/rando`, the randomizer / tracker / spoiler /
  warp / pets / save-editor / cheats / difficulty / cosmetics UI) and every gameplay hook that names LA addresses
  (`runtime/hle/`: chamber, digs, doorjump, escape, flags_ext, hookshot, keysanity, miniboss, mirror, pets, pot_glow,
  qol, rescue, shopsanity, souls, warp, widescreen, framepace).
- LA's instruction patches (`tools/patches.py`'s widescreen / gameplay sites) and LA's texture edits
  (`runtime/nvn/texpatch.c` is now a neutral implementation of the same interface).
- The LA start screen (`runtime/ui/launcher.mm`: PLAY, seeds, settings, LA's background movie frame). SMM2 Recomp
  put its own setup screen in that file (below), and SM35 Recomp's is that one, retargeted.
- Packaging helpers as LAS had them (`.dmg`, AppImage, the PyInstaller-frozen generator `tools/lasgen.spec`, kept
  for reference but unused), LA docs, fonts and screenshots.

## What changed in the shared parts

- `runtime/game.h`: the game's identity (`GAME_ID`, `GAME_TITLE`, `GAME_TITLE_ID`, `GAME_BUNDLE_ID`) from game.mk,
  used for the window title, data folder, settings file, game library name (`<id>game.dylib`) and the program id
  the kernel / aoc report (LAS hard-coded LA's).
- `runtime/game_hooks.c`: the call sites LAS's renderers / input / fs had into LA features stay, and resolve to
  weak, neutral defaults here (a game can override any of them).
- `runtime/mods.*` replaces `rando.*`: the same RomFS-override and save-slot hooks, driven by `RECOMP_MOD_DIR`.
- `runtime/romfs_build.*` + `runtime/services/fs.c`: the RomFS can be served from a folder through an in-memory
  index (tables in memory, file data read from the host files), so a 2.7 GB image needn't be built and an update's
  files can be links over the base game's.
- The game library's entry is `recomp_game_tables`.
- Cross builds with zig (`make TARGET=...`) and player packages (`tools/package.sh`, `tools/bundle_macos.sh`, written
  here: runtime + Python + recompiler + zig + hactool, no game files; README "Player packages and the setup screen").
- The start screen is a setup screen (`runtime/ui/launcher.mm`, rewritten; LAS's window, render loop and update check
  kept): it takes the player's dump (NSP / XCI or extracted folders), keys and firmware, runs the
  game's setup scripts and LAS's `gamebuild.c` with progress, then boots the game. Without it (`RECOMP_NO_LAUNCHER=1`)
  the runtime still runs `gamebuild.c` itself on first launch when the game code library is missing
  (`runtime/main.c`). The generator runs with the package's own Python (python-build-standalone) rather than LAS's
  PyInstaller-frozen `lasgen` (which `gamebuild.c` still accepts): one machine can then make every platform's package.

Environment variables keep LAS's `LARECOMP_*` names for now (they're documented in LAS's sources); new ones are
`RECOMP_*`.
