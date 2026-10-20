# Progress and plan

SM35 Recomp is Super Mario Bros. 35 v1.0.1, statically recompiled from the player's own dump. It boots to the main
menu on macOS and the menus work. Gameplay is blocked on the online work: the game needs its online service even
for Course Practice. Offline it shows Loading and then goes back to the menu. Online play goes through
[Nextendo Network](https://nextendo.network) only, never Nintendo's servers. Bring-up notes are in
[recomp/docs/sm35-bringup.md](recomp/docs/sm35-bringup.md).

✅ done · 🔄 in progress · ⬜ not started · ⏳ waiting on the owner

## Phase 0: Bring-up and first release ✅

- ✅ The repository: the SMM2 Recomp runtime and recompiler, with `games/sm35/`, the version checks, the setup
  screen, the proprietary-content guard and the package checks
- ✅ Recompiler fixes the game needed: jump-table bases are no longer split into functions (957 tables resolved
  instead of 111); `setjmp`/`longjmp` work across recompiled code (Lua errors)
- ✅ Runtime fixes:
  - touch: `GetTouchScreenStates<16>`
  - the `irs` (IR camera) service
  - the `acc` open-context users
  - `svcBreak` result codes; `RECOMP_LOG_IPC`
- ✅ Boots to the main menu on macOS at 60 fps: Daily Challenges, the main menu, Course Practice's course select
- ✅ CI: the guard, the package-check tests, the runtime compile on Linux, the recomp unit tests
- ✅ v0.1.0 release: macOS, Windows and Linux packages (launcher plus recompiler, no game code)
- ✅ The iOS app compiles and is signed for the owner's iPad (not installed yet: the iPad is off Tailscale)

## Phase 1: The online service, through Nextendo ⬜

The game reaches for its online service at boot and again before every course. The SMM2 `nextendo` branch already has
most of the client side; it gets ported here and adapted to SM35.

1. ⬜ Port from SMM2's `nextendo` branch:
   - HTTPS in `http_request`, with system and embedded roots
   - Nextendo sign-in (OAuth PKCE on a loopback port; iOS sign-in sheet), with the client id `nxc_sdpEwnQyyHwl`
   - the token store (Keychain or a private file) and the game token
   - routing, and TLS pinned by SPKI
   - the Online section of the overlay (Sign in / Sign out)
   - the kernel condition-variable timeout fix (the Co-op freeze in SMM2)
   - the null guard at `kernel.c:428`
2. ⬜ DNS and routing: send SM35's online hostnames (nsd, dauth, aauth, BAAS, the NEX auth server, BCAT) to Nextendo;
   anything else stays unreachable. List the hostnames from the binary.
3. ⬜ Device and application auth against `nx-dauth`, and the NSA ID token with `acc`'s IManagerForApplication (the
   path SMM2's branch drives).
4. ⬜ Nintendo Switch Online membership check (the game requires it): answer it as a member when signed in to
   Nextendo.
5. ⬜ BCAT, `bcat:u`: a real service in place of today's logging stub (with pretend-online the game crashes there).
   The delivery-cache storage and its files come from Nextendo's account server (BCAT schedule/data), with an empty
   cache when offline. This feeds Daily Challenges and Special Battle events.
6. ⬜ NEX login to Nextendo's SMB35 server: authentication, then secure connection; PRUDP over UDP.
7. ⬜ The error applet: when a network step fails, the game opens the system error dialog. `am` has to run the
   library-applet flow (create, push the argument, start, signal the end) and show the message itself, rather than
   send the player back to the menu without a word. SMM2's branch hits the same path.
8. ⬜ An offline mode that's honest: when the player isn't signed in, the game says so (through 7) instead of looping
   between Loading and the menu.
9. ⬜ Tests: the request and token paths against a local mock (as `tools/net_mock.py` on SMM2's branch). A live
   check against Nextendo only once the owner signs in.

## Phase 2: Single-player gameplay ⬜ (needs Phase 1)

1. ⬜ Course Practice: every course starts, plays and clears; saving works (`save/save_0`).
2. ⬜ Rendering in courses. Fix the `FS translate failed: bindless tex` fragment shader (seen as a course loads); go
   through every course theme (overworld, underground, castle, water, the 8-4 style ones); check the
   35-player screen layout (the other players' small screens around the main one).
3. ⬜ Audio: BGM streams (`.bfstm`), sound effects, Opus voice/stream decoding. `hwopus` is a logging stub today.
4. ⬜ Input: every button the game uses (item roulette with R, targeting with L or the right stick, touch on handheld
   screens), rumble, gyro (the SDK reads six-axis: check it's harmless).
5. ⬜ Menus: Stats, Options (all settings saved and restored), the icon picker (X: Icons), level and coins, Daily
   Challenges progress.
6. ⬜ Reference frames against Ryujinx-Nextendo running the owner's dump (owner OK needed for SM35, as for SMM2).
   Covers the main menu, course select, and a course in each theme.
7. ⬜ Performance: steady 60 fps in a full 35-player scene; the first-play shader stutter (prefetch and warm-up);
   memory use.

## Phase 3: 35-player battles and Special Battle ⬜ (needs Phase 1)

1. ⬜ Matchmaking: NEX matchmaking and the matchmake referee (the game's `MatchmakeReferee` client) on Nextendo.
2. ⬜ The in-match relay (`eagle`, the game's real-time transport): find what it connects to (a host from the NEX
   session, TCP, WebSocket or UDP), its TLS, and its framing; route it to Nextendo's relay.
3. ⬜ A full match: the countdown, sending defeated enemies, the timer, item roulette, KOs, the results screen, coins
   and XP awarded.
4. ⬜ Special Battle (the event rules from BCAT), and Daily Challenges counting in battles.
5. ⬜ Robustness: latency and packet loss (the SDK's own network emulator can help), a reconnect after a drop, the
   app going to the background mid-match (iOS and Android), sleep and wake.
6. ⬜ Friends and presence if the game uses them (`friend` service, Nextendo account server).
7. ⏳ Live matches need other players on Nextendo at the same time (or Nextendo's bots, if it has them).

## Phase 4: Platforms ⬜

1. ⬜ Windows: run the release package on a real PC (owner's call on when); Vulkan driver checks. `setjmp`/
   `longjmp` across recompiled frames under MinGW's SEH unwinding is untested. Verify it on Windows first.
2. ⬜ Linux x86-64 / arm64: run the packages (owner's call); an AppImage with SDL3 bundled.
3. ⬜ macOS: a signed and notarized app (needs a Developer ID); a fallback for the release workflow when GitHub's
   macOS runners have no capacity (v0.1.0's macOS package was built on the Mac when that happened).
4. ⬜ iPad / iPhone (owner's devices): install over Tailscale, the touch controls, the notch-safe layout (SMM2's
   iPhone L/ZL issue applies here too), the background/foreground behaviour.
5. ⬜ Mobile releases with **no game code in them** (owner's choice, 2026-10-20):
   - **Android:** a launcher-only APK. The player's desktop app builds `libsm35game.so` (arm64, Android guest origin)
     from their own dump, and the APK imports it with the game files: a folder or zip picked through the system file
     picker. Check that Android 14+ still allows loading a read-only native library from app storage. Without a
     device here, test on the emulator.
   - **iOS / iPadOS:** a "make your own IPA" kit. One command on a Mac with Xcode takes the player's dump, recompiles,
     builds and signs the IPA with the player's own Apple ID. Check which entitlements a free Apple account can get:
     extended virtual addressing and the increased memory limit. If a free account can't, the kit needs a paid
     developer account, or a lower guest window and less memory.
   - The release workflow attaches the APK and the iOS kit; `check_package.py` covers both.
6. ⬜ Android (owner has no device): the emulator first, a real device when one is available.
7. ⬜ Web (wasm + WebGPU), for the owner's own use on their machine or LAN only, as decided for SMM2. Feasibility after
   the native ports.

## Phase 5: Parity with SMM2 Recomp and polish ⬜

1. ⬜ The app's name, as the player sees it (owner request, 2026-10-20): "Super Mario 35", or "SM 35" where that is
   too long (the iOS home screen fits about 12 characters). This covers the macOS app and Dock, the iOS/iPadOS home
   screen, the Android launcher, the Windows exe and window title, the Linux desktop entry, and the setup screen.
   The repo, executable and ids can stay `sm35recomp`. One thing to weigh: "Super Mario" is Nintendo's trademark, and
   an unofficial app under that name is more likely to draw a takedown than "SM 35".
2. ⬜ Overlay menu tabs as in SMM2 (Video, Audio, Controls, Online, Quit), without the SMM2-only Mods tab.
3. ⬜ Frame generation (120 fps output): try it on this game's 2D scenes; off by default, as in SMM2.
4. ⬜ Shader prefetch and the pipeline cache tuned for SM35's shaders.
5. ⬜ The update check and release notes flow (in place; check it once v0.2.0 exists).
6. ⬜ Remove the leftover SMM2 course-server code (`runtime/net/embed_server.*`, `mods.ini` settings in `netcfg`)
   or reuse its routing for Nextendo.
7. ⬜ `make test-tools` (it needs arguments: inherited from SMM2) and a macOS runtime compile job in CI.

## Phase 6: Keeping the two recomps in step ⬜

1. ⬜ Port the jump-table fix (an ADR feeding a nearby `br` is not a function start) to SMM2 Recomp's `tools/gen.py`.
   It changes SMM2's generated code, so it needs SMM2's own re-test.
2. ⬜ Port `setjmp`/`longjmp`, the `irs` service, the touch and `acc` fixes, and the `svcBreak`/`RECOMP_LOG_IPC`
   diagnostics to SMM2 (harmless there).
3. ⬜ Bring SMM2's runtime fixes over as they land. In the long run, a shared runtime both repos use, to replace the
   copies.

## Platforms

| Platform | Builds | Runs | Package |
|---|---|---|---|
| macOS arm64 (Metal) | ✅ | ✅ main menu | ✅ v0.1.0 |
| Windows x86-64 (Vulkan) | ✅ CI | ⬜ | ✅ v0.1.0 (untested) |
| Linux x86-64 / arm64 (Vulkan) | ✅ CI | ⬜ | ✅ v0.1.0 (untested) |
| iOS / iPadOS (Metal) | ✅ signed for the owner's iPad | ⏳ iPad offline | ⬜ the IPA kit |
| Android arm64 (Vulkan) | ⬜ | ⬜ | ⬜ launcher-only APK |
| Web | ⬜ | ⬜ | (local use only) |
