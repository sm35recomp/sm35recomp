# Progress and plan

SM35 Recomp is Super Mario Bros. 35 v1.0.2, statically recompiled from the player's own dump. On macOS it signs in to
[Nextendo Network](https://nextendo.network), logs the game in to Nextendo's server, and Course Practice plays
(2026-10-21). The game needs its online service even for Course Practice; online play goes through Nextendo only,
never Nintendo's servers. Bring-up notes are in
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
- ✅ [v0.1.0](https://github.com/sm35recomp/sm35recomp/releases/tag/v0.1.0) (2026-10-20): macOS, Windows and Linux
  packages (launcher plus recompiler, no game code). The macOS package was tested as a player would use it: from the
  owner's two NSPs through setup and a 4-minute recompile to the game at 60 fps. That test found and fixed the update's
  version number and the RomFS patching over the base NCA.
- ✅ The iOS app compiles and is signed for the owner's iPad (not installed yet: the iPad is off Tailscale)

## Phase 1: The online service, through Nextendo 🔄

The game reaches for its online service at boot and again before every course. SMM2 Recomp's `nextendo` branch had
most of the client side; it was ported (branch `nextendo`, merged 2026-10-21) and adapted to SM35.

1. ✅ Ported from SMM2:
   - HTTPS in `http_request`, with system and embedded roots
   - the Nextendo sign-in (OAuth PKCE on a loopback port), with this app's client id `nxc_sdpEwnQyyHwl` (`game.mk`)
   - the token store and the game token. On macOS it's a private file (`net/nextendo.txt`, mode 0600), as on Windows and Linux: with ad-hoc
     signing, the Keychain asked again after every update and the game waited on the dialog. iOS keeps the Keychain.
   - routing (the game's hosts to Nextendo), TLS pinned by SPKI, the RS256 ID token carrying the game token
   - the Online settings (the overlay's tab is "Online" here: the game has no mods), the kernel condition-variable
     timeout fix
   - `RECOMP_NEXTENDO_SIGN_IN=1` starts the sign-in with the game (no F1 over a remote desktop)
2. ✅ Backend Nextendo by default once signed in; "Offline" when signed out (`games/sm35/net.c`, `online.ini`).
3. ✅ `acc`: LoadOpenContext returns the user's IManagerForApplication (its absence crashed the game's online check);
   the network service account, the ID token.
4. ✅ `bcat:u`: an empty delivery cache, as Ryujinx (it was a logging stub: crash at the first course).
5. ✅ NEX login, live (2026-10-21): the auth server (`g21f12900-lp1.s.n.srv.nintendo.net`, TLS WebSocket, pinned),
   then the secure server (port 60014). Fixed on the way: `getaddrinfo` gave the address byte-swapped.
6. ✅ The update is **1.0.2** (title version 0x20000), which Nextendo requires; the recomp called it 1.0.1 before.
7. ✅ iOS sign-in: Nextendo accepts only the loopback redirect for this client, so the iOS sheet runs the same listener
   and closes when the sign-in ends. Signed in on the owner's iPad, and courses start there (2026-10-21). Signing in
   also chooses Nextendo as the Service (a stray Offline choice had sent courses back to the menu).
8. ✅ Special Battle: Nextendo publishes the event as `Dat/sp_battle.dat` in a zip at
   `GET https://nextendo.network/api/bcat/0100277011f1a000`; `games/sm35/events.c` fetches it at start (4 s at most),
   unpacks it (zlib's puff) to `events/romfs/` and lays it over the RomFS. The main menu offers Special Battle.
9. ✅ Offline: the game opens no error dialog of its own (nifm has no applet to give it), so the overlay shows a
   notice when the game asks for the network: not signed in, the Service not Nextendo, or a restart needed.
10. ✅ Online but failing (2026-10-21): a notice over the game says why (runtime/net/nexwatch.c). Nextendo's game
    server unreachable (TCP refused or timed out, or the TLS handshake / pin failed): "Can't reach Nextendo's game
    server (reason)". The login refused (a TicketGranting / SecureConnection reply that is an RMC error, or whose Result
    is one), read from the server's replies on the game's TLS connections: maintenance, the account online elsewhere,
    the account disabled, the sign-in not accepted (sign out and in again), or the NEX error's name and code. Tested
    with synthetic replies (tools/test_net.sh, 4 cases) and a run against a closed port.
11. ✅ Tests: the request, sign-in and token paths against a local mock (`tools/test_net.sh`, 27 cases; one combined
    run in five failed with no output under load, not reproduced in 46 runs since: watch it).

## Phase 2: Single-player gameplay 🔄

1. 🔄 Course Practice: 1-1 starts and plays (run, scroll, Goombas, Game Over, retry). Every course starts, plays and
   clears; saving works (`save/save_0`).
2. 🔄 Rendering in courses. ✅ Bindless textures on Metal (the sprite shader takes each sprite's texture handle from
   its vertices: the course was black). ✅ The same in the Vulkan / GLSL path, run (2026-10-21): the Linux arm64 build
   in Docker under Mesa's lavapipe plays Course Practice 1-1 at 60 fps. That run found and fixed: shader global
   memory went through one storage-buffer binding over the 3.2 GB heap, which reaches only maxStorageBufferRange of
   it (128 MB on lavapipe and Adreno), so the course cards' text was blank; now through the heap's device address
   (bufferDeviceAddress). ⬜ A real GPU on Windows / Linux. ⬜ Go
   through every course theme (only 1-1 is open in Course Practice; the rest open as they are met in battles) (overworld, underground, castle, water, the 8-4 style ones); check the
   35-player screen layout (the other players' small screens around the main one).
3. 🔄 Audio: mixed, not yet confirmed by ear (2026-10-21, Course Practice with `LARECOMP_AUDIO_LOG=1`): the music
   streams as stereo DSP ADPCM at 44.1 kHz (two voices), and up to 18 sound-effect voices play ADPCM at 32 kHz, all
   through `audren` (SMM2's renderer). The game asks `hwopus` only for a work-buffer size at start and never opens a
   decoder, so the stub is enough so far. ✅ The output mix has sound (a 70 s float WAV of Course Practice through
   `LARECOMP_AUDIO_WAV`: silent only through boot and one 2 s fade; music at about 0.03 RMS with sound-effect peaks
   up to 0.27, no clipping). ✅ Confirmed by ear by the owner (2026-10-21, on the Mac: "sound seems OK"). ⏳ The 35-player battle's sounds:
   outside testers.
4. 🔄 Input: ✅ the left stick also presses the d-pad (the game walks only with the d-pad here; the iPad's touch stick
   and controllers did nothing). ⬜ Every button the game uses (item roulette with R, targeting with L or the right stick, touch on handheld
   screens), rumble, gyro (the SDK reads six-axis: check it's harmless).
5. ✅ Menus (2026-10-21): ✅ Stats (records, enemies beaten, recent results; level, coins and courses played kept
   across runs: saving works). ✅ Options is the game's Controls Setup: Movement (d-pad by default; the stick when
   switched, which is why the stick did nothing at first) and Buttons. ✅ The icon picker (X). ✅ Daily Challenges.
   ✅ Changing Controls Setup is kept after a restart (Movement switched to the stick: Option.bin rewritten, the
   screen shows the stick after a restart).
6. ⬜ Reference frames against Ryujinx-Nextendo running the owner's dump (owner OK needed for SM35, as for SMM2).
   Covers the main menu, course select, and a course in each theme.
7. 🔄 Performance. ✅ Shader stutter, measured on the Mac (2026-10-21, Course Practice): with an empty cache the
   whole run compiles about 36 shaders in about 12 ms, so there is no first-play stutter to fix here. Two ~210 ms
   frames remain, with a warm cache too: at the scene change into the battle menus (the game loads Scene_35Battle,
   GameCommon, the course catalog and CourseSelect) and as a course starts: the game's own loading moments, behind its
   transitions. ⬜ Steady 60 fps in a full 35-player scene; memory use; the same on Vulkan and the iPad.

## Phase 3: 35-player battles and Special Battle 🔄 (two-player matches play)

1. ✅ Matchmaking (2026-10-21): the Matching screen, AutoMatchmake (MatchmakeExtension 40), the Eagle handoff
   (Notifications type 200000: `wss://51.178.29.194:30001/<gid>` and a token), the referee's `participants_cnt` /
   `close_participation`, and the round start. Tested with two accounts: the Mac and the owner's iPad (a second
   account with a verified e-mail and a linked Discord; Nextendo refuses accounts without one, `discord_unlinked`).
   - **Two players on one home connection don't work**: the game's secure NEX connection carries no Kerberos ticket
     (an empty CONNECT), so Nextendo identifies the player by address alone and gives both devices one identity;
     only one gets the handoff. With the iPad on another connection (an exit node), both got it. The ticket itself
     is fine: it decrypts with the raw source key Nextendo sends (re-encrypting it with NEX's classic derived key
     makes the game fail with 2306-0304). Why the game then sends no ticket is open; worth raising with Nextendo.
2. 🔄 The relay (`eagle`): TLS + WebSocket to `51.178.29.194:30001/<gid>`, the token, then binary frames.
   - ✅ Fixed: getaddrinfo's port was byte-swapped (dialled 12661); the socket errno numbers were FreeBSD's where the
     Switch uses Linux's (EINPROGRESS 115: the game never saw its non-blocking connect as pending and redialled every
     frame); `Fcntl` O_NONBLOCK is 0x800.
   - ✅ Both devices joined relay room 348 and got each other's player record.
   - ✅ Then both crashed on the relay's first message: the handler's 25-case switch (main+0xb500) had been cut up
     by gen.py (an ADR that is a jump table's base, 8 instructions before its `br`, taken for a function start).
     Fixed: 96 false starts gone; Course Practice re-tested. `RECOMP_BSD_TRACE=1` logs every socket call,
     `RECOMP_NET_HEX=<file>` dumps every TLS payload.
   - ✅ **A two-player match played through** (2026-10-21, relay room 349, Mac + iPad build 31, released as v0.2.1):
     course 1-1, both players, the Ranking screen with the same result on both (coins 200 / 180), Rematch offered.
3. 🔄 A full match: ✅ start to the Ranking screen with two players (personal result, Level Up, Ranking; A moves on).
   ✅ **Cancel on Matching, and Main Menu / Rematch on the Ranking screen, work: they are held, not tapped**
   (2026-10-21). Each button fills a gauge while it is held (1/30 per game step, about a second) and acts only once the
   gauge is full; letting go empties it. The "hang" was the test scripts' 10-20 frame taps. Held for 90 frames, Cancel
   ends the participation (MatchMakingExt.1) and the game goes back to the main menu. The touch controller's buttons
   stay held while the finger is down, so this works on the iPad. Found on the way, and harmless: Nextendo answers
   DeliverMessage (protocol 27, method 1: the host's lobby messages) only by forwarding the message back with the
   sender's call id, where kinnay's server also returns a response; the game carries on either way.
   ⏳ A longer match by hand (outside testers, arranged by the owner): the countdown, sending defeated enemies, the timer, item roulette, KOs, the results screen, coins
   and XP awarded.
4. ⬜ Special Battle (the event rules from BCAT), and Daily Challenges counting in battles.
5. 🔄 Robustness: ✅ a dropped connection in the lobby (2026-10-21, `RECOMP_NET_DROP_AFTER=<s>` shuts down every TCP
   connection to the game server that long after the first one): the game saves and goes back to the main menu
   within a second, no hang. ⬜ A drop mid-match; latency and packet loss; the app going to the background
   mid-match (iOS and Android), sleep and wake.
6. ✅ Friends and presence: not used. Through login, Matching and Cancel the game opens acc, nifm, nsd, sfdnsres,
   bsd, ssl, bcat, prepo (stubbed: play reports go nowhere), pctl, aoc, set, time, hid, irs, audren, hwopus (only a
   size query) and fsp-srv; never `friend` (2026-10-21 audit).
7. ⏳ Live matches need other players on Nextendo at the same time (or Nextendo's bots, if it has them).

## Phase 4: Platforms ⬜

1. ⏳ Windows: run the release package on a real PC (outside testers, arranged by the owner); Vulkan driver checks. `setjmp`/
   `longjmp` across recompiled frames under MinGW's SEH unwinding is untested. Verify it on Windows first.
2. ⏳ Linux x86-64 / arm64: run the packages (outside testers, arranged by the owner); an AppImage with SDL3 bundled.
3. ⬜ macOS: a signed and notarized app (needs a Developer ID); a fallback for the release workflow when GitHub's
   macOS runners have no capacity (v0.1.0's macOS package was built on the Mac when that happened).
4. ⬜ iPad / iPhone (owner's devices): install over Tailscale, the touch controls, the notch-safe layout (SMM2's
   iPhone L/ZL issue applies here too), the background/foreground behaviour.
5. ⬜ Mobile releases with **no game code in them** (owner's choice, 2026-10-20):
   - **Android:** a launcher-only APK. The player's desktop app builds `libsm35game.so` (arm64, Android guest origin)
     from their own dump, and the APK imports it with the game files: a folder or zip picked through the system file
     picker. Check that Android 14+ still allows loading a read-only native library from app storage. Without a
     device here, test on the emulator.
   - **iOS / iPadOS:** 🔄 a "make your own IPA" kit: `recomp/tools/ios_kit.sh --team <Apple team id>` (2026-10-21).
     It takes the game files the Mac app set up, recompiles, builds SDL3 for iOS from its pinned source
     (`tools/fetch_deps.sh ios-arm64`), asks Xcode for a profile with the two entitlements, signs, and leaves
     build/ios-kit/SM35Recomp.ipa plus Documents/ (the game files to copy into the app with Finder). Run here with
     the owner's team: a signed IPA in about 2 minutes. ⬜ Install it from Finder on a device (the owner's iPad was
     offline). Free Apple accounts: untested, and left to players (owner, 2026-10-21): the README and the kit say a paid
     Developer account is known to work, a free one is untested (7-day apps, maybe not the two entitlements), and ask
     for reports.
   - The release workflow attaches the APK and the iOS kit; `check_package.py` covers both.
6. 🔄 Android (owner has no device). ✅ Runs in the emulator (2026-10-21: Android 15 arm64, gfxstream on the Mac's
   GPU; a private debuggable test build with the game code): it boots to the main menu with all text, at about 45-60
   fps. Fixed on the way: the APK build (mbedtls -fPIC, libnativewindow); the app can't read what adb pushes into
   Android/data/<package> on Android 11+, so the data folder is the internal one when the game files are there
   (android_push_data.sh --internal); without host memory import, shader global memory read zero (text missing), now
   per-draw copies of the storage buffers. The emulator lives on the internal disk (~/smm-work/android-sdk, AVD
   sm35test; delete when done). ⬜ Course Practice there (needs Nextendo sign-in in the emulator). ⬜ A real device.
7. ⬜ Web (wasm + WebGPU), for the owner's own use on their machine or LAN only, as decided for SMM2. Feasibility after
   the native ports.

## Phase 5: Parity with SMM2 Recomp and polish ⬜

1. ✅ The app's name, as the player sees it (owner request, 2026-10-20; done 2026-10-21, as SMM2 Recomp did): "Super
   Mario 35" for the window, setup screen, macOS app and Dock ("Super Mario 35.app"), Linux menu entry and package
   README; "SM 35" on the iOS / Android home screen (game.mk GAME_TITLE, GAME_MOBILE_TITLE). The package file names
   (SM35Recomp-<target>.zip, GAME_APP_NAME), executables, bundle id and data folders stay, so downloads, saves and
   sign-ins carry over. (The trademark risk of "Super Mario" in the name was noted; the owner chose it.)
2. ✅ Screenshots for the GitHub README (owner request, 2026-10-21; added the same day: menu, Course Practice, matchmaking, Daily Challenges, setup; a live 35-player match still to add), as SMM2 Recomp's `docs/screenshots/`:
   - the setup screen
   - the main menu
   - Course Practice
   - a 35-player battle, once Phase 3 works
   - the overlay menu
   Taken from the recomp at 1280x720, kept small (resized PNGs or JPEGs), and checked by the guard as SMM2's were.
3. ✅ Overlay menu tabs as in SMM2, without the SMM2-only Mods tab (2026-10-21): Controls, Video, Audio, Online, Quit. The
   Online tab holds the service and the Nextendo account, then the app's updates and game files.
4. ⬜ Frame generation (120 fps output): try it on this game's 2D scenes; off by default, as in SMM2.
5. ✅ Shader prefetch and the pipeline cache: not needed for SM35 (a few dozen small shaders, ~12 ms in all from cold;
   Phase 2 item 7).
6. ⬜ The update check and release notes flow (in place; check it once v0.2.0 exists).
7. ✅ Remove the leftover SMM2 course-server code: `runtime/net/embed_server.*` and its boot call (2026-10-21). The
   settings were already SM35's own (`online.ini`, games/sm35/net.c); netcfg's notes now say so. `runtime/mods.c` stays:
   it is the generic mod folder (RECOMP_MOD_DIR, a romfs/ tree laid over the game's), not SMM2's mods.
8. ✅ `make test-tools` runs (the SIMD lowering, then the lifter on 2000 of the game's own instructions; 2026-10-21).
   ✅ A macOS runtime compile job in CI (tests.yml `macos-runtime`, 2026-10-21).

## Phase 6: Keeping the two recomps in step ⬜

1. ⬜ Port the jump-table fixes (an ADR feeding a nearby `br` is not a function start; also when the `br` is 8+
   instructions on, as in SM35's relay handler) to SMM2 Recomp's `tools/gen.py`. It changes SMM2's generated code, so
   it needs SMM2's own re-test.
   - ⬜ The socket errno numbers (Linux, not FreeBSD) and `Fcntl` O_NONBLOCK 0x800 (`net/hostsock.h`, `services/bsd*.c`):
     SMM2's online code may depend on them too.
2. ⬜ Port `setjmp`/`longjmp`, the `irs` service, the touch and `acc` fixes, and the `svcBreak`/`RECOMP_LOG_IPC`
   diagnostics to SMM2 (harmless there).
3. ⬜ Bring SMM2's runtime fixes over as they land. In the long run, a shared runtime both repos use, to replace the
   copies.

## Platforms

| Platform | Builds | Runs | Package |
|---|---|---|---|
| macOS arm64 (Metal) | ✅ | ✅ Course Practice on Nextendo | ✅ v0.1.0 |
| Windows x86-64 (Vulkan) | ✅ CI | ⬜ | ✅ v0.1.0 (untested) |
| Linux x86-64 / arm64 (Vulkan) | ✅ CI | ✅ arm64 under lavapipe (Docker) | ✅ v0.1.0 (untested on a GPU) |
| iOS / iPadOS (Metal) | ✅ signed for the owner's iPad | ⏳ iPad offline | ⬜ the IPA kit |
| Android arm64 (Vulkan) | ⬜ | ⬜ | ⬜ launcher-only APK |
| Web | ⬜ | ⬜ | (local use only) |
