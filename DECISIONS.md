# Decisions

## A static recompilation, forked from SMM2 Recomp

SM35 Recomp is a static recompilation of Super Mario Bros. 35 v1.0.2: the game's AArch64 code is translated to C
ahead of time and compiled for the host, on a native runtime. It is a fork of
[SMM2 Recomp](https://github.com/smm2recomp/smm2recomp) (itself a fork of LAS Recomp): the runtime, recompiler,
packaging and setup screen are shared, and SM35 adds only its game folder (`recomp/games/sm35/`). Fixes the game needs
go into the shared parts as generic fixes, so they can flow back to SMM2 Recomp.

## Releases: a launcher plus recompiler per platform

A release is the runtime with its setup screen, plus what recompiling the game needs (Python, the recompiler, zig,
hactool), never the game. The player points the setup screen at their own dump, update, keys and firmware, and the
game code is built on their machine. Prebuilt game code, translated shaders or any file of the game are never
committed or published; `scripts/check_no_proprietary.py` guards the repository and `scripts/check_package.py`
every package. iOS and Android, which can't compile on the device, are built from source on the player's own Mac
for their own devices.

## Online through Nextendo Network only

The game is online-only in practice (even Course Practice needs its online service). Online play will go through
Nextendo Network (nextendo.network): sign-in, data delivery and the 35-player battles. The recomp doesn't connect to
Nintendo's servers.

## Licence

GPL-3.0-or-later, as SMM2 Recomp. Third-party code keeps its own licence (`recomp/UPSTREAM.md`, `THIRD-PARTY.txt` in
each package).
