# PC port handoff (MegaMixMP, DivaModLoader mod for MM+ Steam)

Status as of 2026-11-18 ~13:45. Read `AGENTS.md` (section "PC (Steam MM+) port") first; it has build/deploy/launch rules and the user's hard constraints.

## User constraints (non-negotiable)
- All PC mod files stay in `pc-client-mod/`. The game's `mods\MegaMixMP` is a junction to `pc-client-mod/dist/MegaMixMP`.
- Commits go on a separate `pc-port` branch, staging only PC files. Per AGENTS.md, commits must not mention Devin. Nothing has been committed yet; `pc-client-mod/` is untracked.
- The game runs on the second monitor only (DISPLAY2, x=1920..3840, y=0..1200), in its own framed window with a 1920x1080 client. Never on the primary monitor, and never steal focus.
- Every MP feature must work while the game is unfocused. Never use SendInput/keybd_event (keys land in the user's apps). Never call bare `cmd` from PowerShell/Python (the extensionless devkitPro `cmd` triggers a "pick an app" popup).
- Prefer driving the game programmatically (direct calls or hooks) over simulated keys, so user key remaps don't matter.
- The PC lobby panel is restyled for desktop (F8, real text fields, no virtual keyboard). The in-game DIVA HUD (gauge race, rank badge, countdown, leaderboard, popups, results hexagons) must match the Switch version exactly.
- Don't touch the user's MP server on ports 7777/7790 (PID 37468). The test server is PID 46092 on 17777/17778/17790/18080 (log `mm-re/tmp/pc_server.log`). `admin.py --port 17790 --cid pc00000000000001 ...` (put `--port` before the subcommand).

## What works (verified unfocused)
- DML v0.0.16 installed; mod loads; ImGui DX11 overlay; WndProc input; fonts.
- LAN/cloud networking, hello, lobby create/join/leave/kick/rename, PC panel (Lobby + Settings tabs).
- Selector hooks (MegaMix UI `PVSelectorSwitch` + FT UI `PVselPS4`) feed a Switch-layout shadow struct. Leader `sel`, follower mirroring via `PVListSetSelectedIndex`, launch via abstract-index A tap.
- Telemetry (`state`/`final`), results/standings, leader rotation, results auto-return, all driven through InputState injection with the corrected per-index contributor logic (`MpInput.cpp` WriteSets).
- Pause/resume mid-song through the game's own pause manager (see the next section). Real pause-menu cursor at the menu task `+0x278` (Ctrl `0x140659ED0`, state `+0x6C`). Item order: 0 Resume, 1 Retry, 2 Lag Config, 3 Options, 4 Return to Song List.
- RE/dev tooling is gated behind `MP_DEV_TOOLS` (`build.ps1 -Dev`).

## Round sync, pause and start gate: FIXED and verified (2026-11-18 13:40)
The white screens / freezes / stuck pauses had four causes, none of them in the lobby logic:
1. Console deadlock. `WriteLog` wrote to the DML console under a mutex. One click on that console (QuickEdit "Select" mode) blocks every writer, so the game's main thread hung (white, "not responding"). Proven with cdb stacks and by cancelling the selection. Fix: logging is asynchronous (`src/platform/MpLog.cpp`, ring buffer + logger thread), QuickEdit is switched off on the console, and the launcher minimizes the console instead of parking it over the game.
2. `PvUpdateDetour` (exe+0x241DF0) was declared `void`. The function returns an int and its caller (exe+0x2684C9) ends the song when it is nonzero, so songs ended or bounced back to song select at random, and the hold returned garbage.
3. The pause-menu Ctrl detour (exe+0x659ED0) was declared `void`. Ctrl returns true when the menu has finished closing; with the return lost the menu task never died, so resume and Return to Song List never completed (the "state 5 stall" / white screen after a round).
4. The game pauses the song every frame its window is not foreground (PV task check at exe+0x2684A8 -> `RequestSystemPause`). Unfocused test runs were fighting that. Byte exe+0x14AB9E8 = 1 disables it; the mod sets it while in a room and restores the game's value outside.

Design now (see the long comment "native pause control" in `src/mp/MpInput.cpp`):
- Start gate: `PVGameLoaderFinishUp` reports the load (-> `loaded`), then the pv update is skipped (returning 0) until the server's `start`. No pause menu at the gate. While waiting the screen is the blank pre-song frame with the countdown overlay on top.
- Group pause: `PauseWant(1)` calls the game's own `RequestSystemPause` (exe+0x65C780); `PauseWant(0)` runs the pause menu's own "pause key" branch from inside its Ctrl (resume flag, closing state, SE, out animations). Both are idempotent targets, never toggles. The reconciler just sets the target and reports once if it isn't reached in 5 s.
- Removed: `PauseReqDetour` (it was hooking `SetEnabled`, not a request), `DeadPauseGuard`, pending-list unlinking, forced menu state writes, DevTap for pause (the keyboard-poll hook is dev-only now), the A/B flag.
- `keys.h`: logical Plus = abstract pause action, InputState key 0xA0 (what the pause manager and menu test), so a real pause press is detected whatever it is bound to.

Verified (logs `mm-re/tmp/t_mp_m3.log`, `t_edge.log`, sheets `mm-re/tmp/ab/m3_sheet.png`), game UNFOCUSED, no OS input:
- 2 consecutive rounds vs a bot (bot leads round 1, PC leads round 2): launch, held at the gate (picture still), released within the same second the bot got `start`, song running, results, auto-return to song select, leader rotation. An earlier 3-round run (focused for rounds 1-2) also passed.
- In both rounds: owner pause -> PC paused with menu; owner resume -> running; PC `pause_req` -> paused; PC `resume_req` -> running.
- Leave while held at the gate -> hold released, song runs solo. Leave while group-paused -> resumed by the mod.
- Solo: pause, resume, pause, Return to Song List -> selector back, no white screen.
- Note: outside a room an unfocused game pauses itself (vanilla behaviour, restored on purpose).

Test scripts are in `pc-client-mod/tools/mptest/` (`lead.py` helpers, `t_solo.py`, `t_mp.py TAG ROUNDS`, `t_edge.py`). They use the admin port and hard-coded paths of this machine. `unfocus_game()` takes focus off the game without input (SC_MINIMIZE via PostMessage, then restore without activating).

Lessons for anyone adding hooks: every detour must have the real return type and return the original's value; check the caller's use of eax/al with `tools/xdis.py` first. Never do blocking I/O on a game thread.

Still open from this area:
- The game window keeps its own remembered size (client 1910x1175 here); the launcher's placement does not stick. Set the resolution in the game's settings, then re-check HUD layout.
- Emergency exit with a real F9 hold is untested (needs the user).
- The PC "played" these rounds with no input (life 0 drop-outs); a round where the PC survives to the song end was not run.

## Next
1. Re-verify unfocused: Retry swallow-veto on the real cursor (+ Lag Config/Options); retry-slip recovery; emergency exit (F9 3 s or XInput Back; the user must test the real key).
2. (done) Reconciler no longer loops.
3. Results end-menu real cursor: same technique as the pause menu (StageResultSwitch `0x14064C0E0`/`0x14064C300`, StageResultPS4 `0x140231A70`/`0x140232DD0`; hook their task Update and find the cursor field).
4. HUD layout parity:
   - Race gauge: the user reports the other players' bar is invisible and the pointers sit on the bar. The previous fix hardcoded 16:10 constants, which is wrong; make it aspect-aware. Measure at 16:9 1920x1080 first; it should match Switch (bar ~692-710, x 130-1240 of 1280). Ideally read the gauge position from the game.
   - The results hexagon cluster is clipped at the right edge.
   - Check the FT UI gauge position too.
5. Song-select MP pill parity (Switch `overlays/Hints.cpp` MP/AUTO pill + "<name> is picking a song") with "F8" key text.
6. Two-round sessions in BOTH UI modes (MegaMix + Future Tone), PC as leader and follower, unfocused.
7. Later: `MpModuleDb` + `MpAvatar` (portraits), AutoPlay reporting.
8. Before committing: create branch `pc-port`, stage only `pc-client-mod/` (+ `docs/PC_PORT_GUIDE.md`, `AGENTS.md` PC section if the user wants), ensure a 0-warning release build.

## Key files
- `src/mod.cpp`: DML exports, ImGui/D3D11, WndProc, hook install order, A/B switch.
- `src/game/MpSelPc.cpp`: game hooks (selectors, TaskPvGame, results, customize, InputState poll) + selector shadow.
- `src/game/addresses.h`, `src/game/keys.h`: addresses and the input findings (VK→device-index map: Esc→28, Enter→55, arrows 123/125/126/128, F1-F12 86-97).
- `src/mp/MpInput.cpp`: InputState injection/blocking, native pause control (pause-menu hook + real cursor, `PauseWant`, background-pause flag), start-gate hooks (loader finish + pv-update hold), dev tooling behind `MP_DEV_TOOLS`.
- `src/mp/MpLobby.cpp`: phase machine (Switch port), start gate (`LoaderHeld`/`LoaderFinished`/`s_expectLoad`), sync-pause/reconciler.
- `src/mp/MpMenu.cpp`: blind menu model (now reads the real pause cursor).
- `src/mp/MpUi.cpp`: PARITY HUD (gauge `DrawGaugeRace` was edited, see item 4). `src/mp/MpPanelPc.cpp`: PC lobby panel.
- References: `mm-re/pc/ref/nc` (New Classics PC, addresses/structs), `mm-re/pc/ref/DivaModLoader`, `mm-re/pc/ref/MultiDiva`; `pc-client-mod/tools/xdis.py` and `peread.py` for disassembly.
