# MegaMix LAN Multiplayer — status, design and open work

Read `AGENTS.md` first (build/deploy/test commands, Ryujinx + real-Switch access, gotchas).

## Product goal
Modded Switch consoles (and Ryujinx) join a lobby on a PC server. The leader picks a song in song select; every
console mirrors the pick, launches together, waits paused until all have loaded, plays with a live leaderboard,
sees final standings, then returns to song select. A web page will later spectate all lobbies (milestone 5).

## Architecture
- Client (`switch-client-mod/source/program/mp/`, exlaunch C++ in the game process):
  - `MpApi` — runtime-resolved `nn::socket` / `nn::nifm`; guard hooks on `nn::socket::Initialize/Finalize`
    (the game initializes sockets itself at boot; first caller wins, game Finalize is swallowed).
  - `MpNet` — dedicated thread (nn::os prio 31 = lowest, core 2): nifm request, UDP discovery or `server_ip`,
    non-blocking TCP (Poll is unreliable on Ryujinx), NDJSON, 2s ping, 6s watchdog, reconnect. Out-queue ring
    from game threads; inbound lines go to `MpLobby` via a ring. Debug commands (`dbg_cmd`) from the admin port.
  - `MpLobby` — client phase machine: NONE/SELECT/LAUNCHING/LOADING/PLAYING/SPECTATING/DROPPED/RESULTS.
    Hooks: pv_sel ctrl (state 6 browse, 8 confirm panel, 10 commit; `sel+0x2617C` pv), TaskPvGameCtrl
    (`GetPVGameData()` score/combo/life/percentage/paused/loaded), StageResultSwitch init/dest.
  - `MpInput` — HID-level injection/blocking inside the GetNpadState hooks (`keyboard.cpp`). Minus is never
    blocked; holding Minus 3s = emergency exit. ImGui's own pad poll bypasses blocks (per-thread via TPIDRRO_EL0).
  - `MpUi` / `MpOverlay` — ImGui panels (windows must push a full-screen clip rect or text is invisible).
- Server (`server/`): `mp_server.py` transport + discovery + admin; `lobby.py` pure engine (protocol v1 in its
  docstring); `test_lobby.py`; `bot.py`/`sim.py` test clients.

## Verified
- Ryujinx: two full rounds vs a bot (follower and leader roles), mirroring, launch piggyback, auto-pause gate,
  START + resume, live board, events, early fail, standings, auto-return, leader rotation.
- Real Switch: boots, connects (`MP: online …`). A full hardware round has NOT been played yet.

## Game facts (verified or user-provided)
- Song select: D-pad L/R = difficulty; L/R/Y = filters/sort; X or R-stick = module picker (sel busy != 0).
  A on a song → confirm panel (state 8); A again without moving = Start → state 10 → Dest. The panel ignores
  input for ~0.5s after appearing.
- Pause menu: Resume (default) / Retry / Lag Config / Options / Return to Song List. Plus pauses and resumes.
- Results: Clear/Not Clear → ~2s → results; A → rank/points; A → menu (RETRY default, Return to Song List below).
- Failing (life 0) ends the song early.

## NEXT SESSION START HERE
Items 1–7 below were implemented in commit `78ecb4e` (28/28 server tests pass, client builds, boots to song
select) but have NOT been exercised on a device. First job: verify each on Ryujinx and the real Switch —
MP button + overlay open/close + minimized room info, create/join from the overlay, owner pick modes across
rounds, owner pause/resume for everyone, non-owner pause vote (pass + 15s expiry), results stay until the
player leaves, emergency-exit ring, ZR×5 AutoPlay (single/double ZR side effects: confirm the old double-ZR
2D-mode / L3+ZR no-movie toggles still behave as the README describes), and the interactive bot CLI.
Kill stale `mp_server.py` processes before starting a server (port 7778 conflicts).

## Round 5 user feedback (supersedes items 1–7 where they conflict)
- Whenever the user is asked to test: auto-deploy to the physical Switch first (`mm-re/tools/nx.py deploy`;
  close the game first if config edits are needed).
- AutoPlay IS allowed in a lobby, but the client must report it (`autoplay {on}`) and the server tracks it per
  member and shows it to everyone (lobby members, board, standings, e.g. an "AUTO" tag).
- Song picking defaults to passing/rotating, and every client shows "<name> is picking a song..." in song select.
- Results: when the lobby owner leaves results, everyone still on results is moved on automatically (round_end
  → client runs the results→song-list recipe: A, A, Down, A with delays); others may leave earlier on their own;
  keep the 90s auto-progress fallback.
- The Multiplayer button must REPLACE the AutoPlay badge in `overlays/Hints.cpp`, reusing its exact styling and
  position (not a separate window).
- Overlay: three large Switch-style rounded buttons filling the space — left pencil icon "Edit Name", middle
  people icon "CREATE LOBBY", right geometric-shape icon "JOIN LOBBY". In a lobby, equally large touch targets
  for leave/settings. Only the X closes the overlay (no outside-tap close); make the X a large touch target.
- Bot: `python bot.py` gets `error proto_mismatch` then a SystemExit traceback — hello must send `proto: 1`; handle
  server errors/disconnects without tracebacks (stay running, allow reconnect).

Round-5 status (commit after c14aa8c): implemented and deployed to the real Switch, NOT verified on device.
Known gap: the "AUTO" tag is tracked by the server but not yet rendered in the client leaderboard/standings/member rows.
Verify next: overlay 3-button layout + X-only close, results→song-list recipe after owner leaves, "is picking a song"
placement, interactive bot session end-to-end.

## OPEN WORK (user requests, in priority order)
1. **Lobby roles + song-pick mode.** Track `owner` (lobby creator; passes to the next member in join order if
   they leave) separately from `leader` (current song picker). Owner chooses `pick_mode`:
   `owner` (owner always picks), `assigned` (owner assigns a member), `rotate` (current behavior: rotate after
   each round). Server: `set_pick_mode {mode, cid?}` (owner only); `lobby` msg gains `owner`, `pick_mode`.
2. **Group pause.** During a round, a real Plus press is intercepted (Plus stays blocked) and sent as
   `pause_req`. Owner → immediate `pause` broadcast; everyone else → a vote: banner on all clients
   "Pause vote: N/M agreed — press + to agree (Ns left)", passes when all connected players agree, expires after
   15s. On pass every client pauses itself (inject Plus only if not already paused). Resume works the same way
   (owner immediately, others vote) → `resume` broadcast. While group-paused, block pause-menu navigation so
   nobody can Retry/Quit (emergency exit still works). Add unit tests.
3. **Lobby UX rework.** Replace the always-on panel with a "Multiplayer" button placed where the AutoPlay UI
   currently is in song select. Tapping it opens a large overlay (most of the screen): edit name, create/join,
   member list, owner settings (pick mode, assign picker), leave. Close via an X button or tapping outside.
   While open, block game input behind it (except Minus). When minimized and in a lobby, the button area shows
   compact lobby info (room code + members) in the same spot.
4. **Hide AutoPlay.** Toggle only with ZR pressed 5 times within 3s (must not fire the existing double-ZR 2D-mode
   toggle as a side effect — decide at window end). Show nothing when off; persistent "AUTOPLAY ON" text while
   on. Force AutoPlay off and ignore the toggle while in a lobby.
5. **Results: no auto-advance.** Remove the results auto-return; players leave results when they choose.
   Unblock input on results (a RETRY then just plays solo; client marks itself not ready). Server waits for
   back_in_sel up to 90s (was 25s) before round_end; a player still in results when the next round launches
   auto-launches when they reach song select (existing LAUNCHING behavior) or is dropped by the 30s rule.
6. **Emergency exit feedback.** While Minus is held in a lobby, show a filling circle in a corner with
   "Hold to Emergency Exit Lobby".
7. **Bot CLI rework (`server/bot.py`).** The user couldn't test because of a "version mismatch" error: exempt
   bots from the mod-version check (mod string starting with `bot`) and never treat the warning as fatal; compute
   the expected version from consoles only. Make the bot an interactive, user-friendly CLI: print every server
   message in readable form; `auto on|off` (auto-ack, auto-load, auto-play with rising score, auto-final,
   auto back_in_sel); as leader: browse songs (`next`/`prev`/`pick <pv|name>` using `mm-re/re/pv_list.txt` or a
   built-in list), `start`; manual controls: `create`, `join CODE`, `leave`, `name NEW`, `ready on|off`,
   `load`, `fail`, `final`, `pause`, `resume`, `status`, `help`. Runs while commands are typed.
8. Still unverified paths: module-picker readiness, missing-song fallback, straggler drop, auto-leave on exiting
   song select, full round on real hardware.
9. UI polish: lobby panel overlapped the song list; status chip overlaps the % gauge in-song (hide or move it
   during songs).
10. Percent markers for other players on the bottom % gauge (calibrate pointer x vs pct from screenshots).
11. Make MP independent of `new_classics = true` (pv_sel/results hooks are only installed when NC is on).
12. Milestone 5: web spectator client (HTTP/WebSocket bridge in the server; view any lobby; kick / reassign
    leader from the web).

## Known quirks
- Different subnets (PC 10.169.0.x, Switch 10.167.0.x) → set `server_ip` on the console.
- Commit `8e4dc28`'s message also lists lobby fixes that actually landed in `35c196e`.
