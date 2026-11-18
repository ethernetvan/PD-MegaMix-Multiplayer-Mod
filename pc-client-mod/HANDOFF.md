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

## Session 2026-11-18 evening: status and findings
Done and committed on `pc-port` (commits 2348ae3, 484a38c, c1c545b):
- F8 panel (WndProc subclass was never installed; F8/Esc now polled while focused), key leak from other apps (unfocused: only the mod's injected input is re-applied; keyboard device bitset cleared in the kb poll hook), HUD drawn in the game's letterboxed 16:9 HUD rect (`MpDrawAll` in mod.cpp: DisplaySize = HUD rect during the parity draws, then all their vertices/clip rects are translated; HUD windows captured by `BeginWin`), gauge race back to the Switch code, panel member rows, assigned pick mode default, server rename + 15 s idle drop of pinging consoles.
- Verified unfocused: 2-round bot sessions (gate, pause/resume, results, auto-return), rank badge under the score, gauge fill + markers on the game's bar, panel rows stacked.
- User decisions: Future Tone UI is out of scope. Avatars are in scope.
- The user's own mp_config.toml values were backed up to `mm-re/tmp/mp_config.user_backup.toml`; restore it when testing is done (tests need server_mode local on 127.0.0.1:17777, test server log `mm-re/tmp/pc_server2.log`).

Avatars: RE findings (MM+ 1.03), not implemented yet:
- Module table: handler `*` at 0x1416EBEE8 (getter 0x140441B60); `get_module_by_id(handler, id, out)` 0x1404421A0. Two vectors of 0xC0-byte entries: [+0x00,+0x08) vanilla (628 entries), [+0x18,+0x20) extra (negative ids). Entry: +0x00 id, +0x04 sort_index, +0x08 chara, +0x0C cos, +0x68 spr_sel_md set id, +0x6C SPR_SEL_MDnnnCMN set id (id 0 -> 505), +0x70 md img spr id, +0x74 CMN md img spr id (id 0 -> 34812), prj::string name after the bools (see MMPlusMods XSongPack pv_game.cpp `module_data`). Signature mh = FNV-1a(chara, cos, sort_index) like the Switch code.
- Save data: GetSaveData 0x1401D6510 -> 0x14114C664. The Switch Shared Set A slot-1 offset (u32 index 0x546C5 = +0x151B14) reads 0 here: UNVERIFIED, find the real offset (diff the save after changing the module in Customization, or ask the user which module is equipped).
- Sprites: LoadSprSet 0x1405B4770 (id, prj::string_view*), CheckSprSetLoading 0x1405B4810 (true while loading), UnloadSprSet 0x1405B48B0, put_sprite 0x1405B49C0 (SprArgs*, struct in ref/nc diva.h; DefaultSprArgs 0x1405B78D0). Sprite sets: std::map<u32 index, SprSet> with head pointer at 0x14CBFF3A0 (lookup 0x1405B8EC0, set id -> index via 0x1405BC600 entry +0x28); SprSet vtable 0x140CB7360. Game textures are `dx_texture {int uses; dx_texture* free_next; ID3D11Texture2D*; ID3D11ShaderResourceView*; format; internal_format; flags; width; height; mip_levels}` (MMPlusMods wrap.cpp).
- Plan: load the CMN set on the game thread, get the md img sprite's dx_texture (try put_sprite and read back SprArgs::texture, or walk the SprSet), CopyResource to a staging texture on the render thread, decode BC5 "YCbCr" (mip0 Y/A, mip1 Cb/Cr) exactly like switch-client-mod MpAvatar.cpp, head-crop to 96x96, upload as an ImGui texture (SRV), plus the upload/relay path to the server.
- Probe result (dbg_cmd `dump_avatar {module:0}` at song select, `MpAvatarStub.cpp` GamePump): LoadSprSet(505) + CheckSprSetLoading works; `put_sprite` with id 34812 (alpha 0) returns the queued SprArgs whose +0xA0 is the engine texture object. That object's +0x28 is the `dx_texture`: ID3D11Texture2D at +0x10, SRV at +0x18, format 14 (BC5), 512x512, 2 mips -> same YCbCr layout as Switch. The queued SprArgs also carries sprite_size (+0x12C, 408x494 for the card) and texture_pos/size right after (read them to crop). Next: CopyResource into a STAGING BC5 texture on the render thread, map both mips, port the Switch decode (`Bc4Block`/`Bc5Region`/`DecodeYCbCr`/`OpaqueTop`/`HeadBand`/`FinishUpload` in switch MpAvatar.cpp lines 347-630) and the cache/remote/upload code, create an SRV per portrait for ImGui.
- Local module (Shared Set A slot 1): u32 at GetSaveData() + 0x168D68 (save base 0x14114C664 -> 0x1412B53CC). Verified 2026-11-18: the user equipped Racing Rin 2010 Ver. (module id 143) and that value is the only 143 in the first 2 MB of the save. Neighbours: [.., 9, 143, -1 x5, 31, 0, -1 ..]. Module names: prj::string at entry +0x80 (size +0x90, cap +0x98, inline when cap < 16).

## Open tasks (user, 2026-11-18 ~17:25)
1. Race bar: the fill must render UNDER the game's BORDER / G / E markers on the clear gauge, with a texture similar to the game's bar, like the Switch version; alignment is still a few px off. Check how the Switch DrawGaugeRace/rank code gets under game markers (game sprite pass vs ImGui).
2. Capture the bar with several bots progressing at different speeds (bot.py `--song-secs` differs per bot -> different pct rates).
3. Avatars (portrait pipeline, see notes above), LocalModule from save +0x168D68.
4. Follower stuck in the song-start popup when the leader launches -> dropped; handle it.
5. Non-critical: all modules became unlocked after the mod was installed (were not before). Nothing in our code or DML writes unlocks; investigate (save module flags / test automation in Customization).
- Race fill under the game's markers (task 1): the clear gauge and its BORDER/G/E markers are AET layers owned by PVGameUI (`aet_list[95]`, see ref/nc diva.h; UpdateGaugeFrame 0x14027A490). Plan: draw the fill through the game's sprite renderer (put_sprite 0x1405B49C0, SprArgs resolution_mode HD = 720p coords, `priority` field) from a game-thread hook, at a priority/submission point below the marker layers; find the markers' AET priority by reading aet::GetComposition / the layer args of the gauge entries. Fallback: ImGui fill clipped to the bar interior only.
- Sprite rect drawing reference: MMPlusMods `MMPlusModsShared/sprite.cpp` `put_sprite_multi(rect, mode, prio, color, layer)` (kind SPR_KIND_MULTI=4, trans = rect.pos, SetSpriteSize, resolution_mode HD) and the enums in sprite.hpp (SPR_KIND_RECT=3, SPR_PRIO_00..; SprArgs layout with `prio` at +0x1C, `color` at +0x08, `trans` at +0x34, `sprite_size` at +0x12C, `flags` |= SPRITE_SIZE). Try submitting the race fill from the pv-update detour with prio values around the gauge's and compare against the markers.
