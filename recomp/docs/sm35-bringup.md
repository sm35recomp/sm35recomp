# SM35 bring-up

What Super Mario Bros. 35 v1.0.2 needed on top of SMM2 Recomp's runtime and recompiler. Names and behaviour only:
no game code, no addresses that only make sense with the dump.

## The game

- Title 0100277011F1A000 (update 0100277011F1A800). The recomp is for **v1.0.2** (title version 0x20000), the last
  update and the one Nextendo Network requires: against 1.0.0 only `main`
  changed (build ids and ExeFS hashes in `games/sm35/versions.py`).
- Engine: Nintendo's **Bezel Engine**. Game logic is Lua (scripts inside `.bea` archives in the RomFS; `boot.lua`
  starts it); physics is PhysX. The RomFS is small (about 370 MB, 172 files with the update: `Archive/`, `Dat/`,
  `stream/`).
- ExeFS modules: `main`, `sdk`, `subsdk0` = **GLSLC** (the runtime GLSL compiler), `subsdk1` = the **movie library**.
  Only `main` and `sdk` are loaded and recompiled, as for SMM2: GLSLC has never been called so far, and the movie
  library neither (the RomFS has no movie files). `GAME_MOVIES := 0`, so no FFmpeg is needed on any platform.
- Online: **NEX** (matchmaking, the referee, the secure connection) plus **`eagle`**, the live in-match relay that
  carries the 35-player battle itself. None of it is implemented yet; it will go through Nextendo Network.

## What it needed (all generic: shared runtime and recompiler)

1. **Jump tables behind an ADR** (`tools/gen.py`). An `ADR` whose register feeds a `br` within 6 instructions is a
   jump table's base, not a function start. Taking it for one split functions in two (`vfprintf`, for one) and cut off
   their table targets ("no function at ..."). With the fix the resolved jump tables went from 111 to 957 (sdk 43 to
   491, main 68 to 466). SMM2's generator has the same heuristic.
2. **setjmp / longjmp** (Lua's error handling: `luaD_throw` / `luaD_rawrunprotected`). `runtime/hle/setjmp.c`, and
   `gen.py` wraps every call to `setjmp` (the module's own, or a PLT stub whose GOT slot is `setjmp`'s JUMP_SLOT) in
   `if (!setjmp(*rt_jb_slot(c, c->x[0])))`. `longjmp` / `_longjmp` are host code: they restore x19-x30, sp and d8-d15
   from the guest's `jmp_buf` and host-longjmp back to that call. `rt_jb_slot` is exported for Windows
   (`runtime/platform/windows_exports.def` / `.c`).
3. **hid**: `GetTouchScreenStates<16>` HLE (`runtime/hle/touch.c`).
4. **irs** (the Joy-Con IR camera service): its shared memory and a camera handle, with no camera connected
   (`runtime/services/hid_set.c`).
5. **acc:u0** 130 `LoadOpenContext` and 131 `ListOpenContextStoredUsers` return the user. Without them fs's
   `EnsureSaveData` asked `GetUserExistence` for an unknown uid and the game aborted with 2002-6465.
6. **Movie HLE**: `FindInt64` and `SetCoreMask` added (unused now: the game plays no movies).
7. **Diagnostics**: `svcBreak` logs the `nn::Result` (module-description); `RECOMP_LOG_IPC=1` logs every IPC request
   with its result.

**System data** from the player's firmware is needed, not only the fonts: `games/sm35/sysdata.py` copies them, and
the word filter (NgWord2, 0100000000000823) is opened at boot.

## Status (2026-10-20)

- **macOS: boots to the main menu at 60 fps.** The Daily Challenges pop-up, the main menu and Course Practice's course
  select render and take input.
- **Starting any course needs a network connection.** Offline, nifm refuses, the game goes to its update check
  (`Scene_UpdateCheck`) and back to the menu: what the real game does offline.
- With `RECOMP_PRETEND_ONLINE=1` it gets as far as **bcat:u** (data delivery: unimplemented, the logging stub returns
  no object) and crashes there.
- Next: the online work, Nextendo sign-in, bcat and NEX.
