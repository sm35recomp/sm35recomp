# Progress

Status: boots to the main menu on macOS; the menus work. Gameplay is blocked on the online work: the game needs its
online service even for Course Practice (offline it shows Loading and goes back to the menu), and that will come
through Nextendo Network. Details: [recomp/docs/sm35-bringup.md](recomp/docs/sm35-bringup.md).

1. ✅ Bring-up to the main menu on macOS (60 fps; Daily Challenges pop-up, main menu, Course Practice course select)
2. ✅ Controls and overlay parity with SMM2 Recomp (inherited: keyboard / controller mapping, overlay menu, on-screen
   controller)
3. 🔄 Repository and CI (proprietary-content guard, package-check tests, runtime compile checks; release workflow)
4. ⬜ Nextendo sign-in, bcat (data delivery) and NEX: the online service the game needs
5. ⬜ Gameplay verification: Course Practice, Special Battle (blocked on 4)
6. ⬜ 35-player online battles (NEX matchmaking and referee, the `eagle` in-match relay, through Nextendo Network)
7. ⬜ Release packages (macOS, Windows, Linux: the launcher plus recompiler)
8. ⬜ Windows / Linux (they build in CI; untested)
9. ⬜ iOS / iPadOS
10. ⬜ Android
