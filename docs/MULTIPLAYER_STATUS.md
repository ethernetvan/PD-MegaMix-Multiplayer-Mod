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
  docstring); `test_lobby.py`; `bot.py`/`sim.py` test clients; `web_bridge.py` HTTP/WebSocket bridge for the
  `web/` spectator/admin SPA (`test_web_bridge.py`).

## Verified
- Ryujinx: two full rounds vs a bot (follower and leader roles), mirroring, launch piggyback, auto-pause gate,
  START + resume, live board, events, early fail, standings, auto-return, leader rotation.
- Real Switch: boots, connects (`MP: online …`). A full hardware round has NOT been played yet.

## Game facts (verified or user-provided)
- Song select: D-pad L/R = difficulty; L/R/Y = filters/sort; X or R-stick = module picker (sel busy != 0).
  A on a song → confirm panel (state 8); A again without moving = Start → state 10 → Dest. The panel ignores
  input for ~0.5s after appearing. B on the song list (back to the main menu) ALSO goes through state 10:
  6 → 10 → Dest, so only an 8 → 10 edge is a song start (verified 2026-11-16 in the server's `SEL` lines).
- Pause menu: Resume (default) / Retry / Lag Config / Options / Return to Song List; the cursor wraps both ways
  and opens on Resume every time. Plus, B, and A on Resume resume immediately (no countdown); Plus and B resume
  from any item. D-pad presses within ~100 ms of the menu opening are dropped. Plus does nothing during the
  fail sequence at life 0.
- Results (CLEAR and NOT CLEAR alike): score page --A--> `win_vp` page --A--> 2-item menu [Retry (default),
  Return to Song List]; Down/Up toggle, Left/Right do nothing, B doesn't close the menu. Return + A → StageResult
  dest and pv_sel init within ~1 s. Retry (from results or pause) restarts the song with `pv->loaded` staying 1.
- No cursor signal exists for either menu (no AET layer-name call, no PVGameData byte); the only results-page
  signal is the AET in/loop/out call with `in = "win_vp_in"` (page 2).
- Failing (life 0) ends the song early; the results then read NOT CLEAR… + DROP×OUT.
- Clear border (`PVGameData`, verified 2026-11-16 by disassembly + Ryujinx): `+0x2D0D0 clear_border` = the
  song + difficulty clear % (70.0 on the Extreme of pv 246; set per song, not a fixed 80), `+0x2D0D4
  clear_border_eff` = the running border (clear_border × reference score so far / total; it climbs during
  the song and equals clear_border at the last note; the game's `percentage >= border` tests at 0x170974 /
  0x1bb388 and the HUD border marker at 0x6893e0 read it). Great/Excellent borders are per-difficulty tables
  at 0xb12774 (65/75/80/85/90) and 0xb12760 (80/85/90/95/97).
- Song availability: `sorted_pv_lists[8*sort + 2*difficulty + edition]` in the selector (all lists of the current
  sort mode are built at pv_sel init; edition 1 only at Extreme). pv_db contains DLC the console doesn't own.

## NEXT SESSION START HERE
Latest state: Round 8 below; the remaining prioritized list is in `docs/NEXT_SESSION_TODO.md` (local handoff
file). The paragraph below is historical (round 4/5).
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

## Round 6: multi-screen Switch-style lobby UI (6.2" handheld)
- Bottom-left status chip is off by default; config key renamed `show_status` -> `debug_status_chip` (old
  saved `true` values are ignored).
- Song-select Multiplayer button (`Hints.cpp`, AutoPlay badge spot) shows OFF / DISCONNECTED / CONNECTED /
  `ROOM: CODE`, plus a right-aligned "<name> picking a song (N)" line in a room. It opens the overlay in every
  state (OFF shows how to enable it).
- Overlay (`MpUi.cpp`) is split into screens: HOME (Edit Name / Create Room / Join Room cards + state pill),
  EDIT NAME and JOIN ROOM (compact dialog above the virtual keyboard), ROOM (top-3 podium, ordered member list,
  owner-only kick, Room Config, Exit Room), ROOM CONFIG (owner: rotate / owner / assigned + picker). Opens on
  ROOM while in a lobby. Only the X closes it. Text sizes were raised for the handheld screen (base font 11 px).
- Server: owner-only `kick {cid}` (unit tested); transport now forwards `set_pick_mode`, `autoplay`,
  `pause_req`, `resume_req`, `kick` (they were silently dropped before).
- Fixes found while testing: `lobby` messages with 2+ members overflowed the client JSON token budget; the song
  search bar drew over the overlay; banners overlapped the overlay header; ROOM footer showed a stale "You're the
  leader" after the picker changed; the virtual keyboard was hijacked by SongQuickJump (`IsOpenFor`).
- Verified on Ryujinx and on the real Switch (touch): button states, HOME, EDIT NAME, JOIN ROOM (typed code),
  CREATE, ROOM with 3–6 members, kick, ROOM CONFIG assigned picker, minimized room info, Exit Room. Not yet
  re-verified with the new UI: a full round, group pause votes.
- Controller support (docked, no touchscreen): single **ZR** opens the overlay (gap-based: ZR×2 = No Movie,
  ZR×5 = AutoPlay, L3+ZR = 2D all preserved); **ZR** closes it. D-pad/left stick moves a cyan focus ring
  (spatial, no wrap; hold repeats after 400 ms then every 120 ms), **A** activates, **B** backs out / closes,
  kick and Exit Room need a second **A** within 3 s (touch stays single-tap). On the keyboard: A types,
  B deletes (empty buffer = cancel), Y = space, X = caps, L/R = caret, Plus = submit. Pad state comes from
  `mp::input` nav edges (merged state pre-block, so injected admin taps work); a release guard keeps held
  buttons/sticks blocked until released after close. Admin `tap`s are now masked by blocks like real input;
  the lobby's own injects use `Tap(..., force=true)`.
- Fixed: every inbound `lobby` message ran `ApplyBlocks()` and handed input back to the game behind an open
  overlay (pad A on a menu item also opened/started the song). `PushBlocks()` now forces a full block while the
  overlay is open. Verified on Ryujinx (docked, pad only); real-Switch pad test still pending (console offline).

## Round 7: lobby avatars (Module Shared Set A)
- Every member's podium/list avatar is the game's own module-select portrait (`SPR_SEL_MD%03dCMN` /
  `..._MD_IMG`) of the first module in their **Module Shared Set A**, framed in a pointy-top pink→orange
  gradient hexagon (white keyline, pale plate, sparkles on the podium, cyan rim = you). No extra screens: the
  module is read straight from the save manager, so changing it anywhere in the game updates the avatar.
- Local module: `((int32_t*)saveMgr)[0x546c5]` = Shared Set A slot 1 (3 sets × 9 slots × 7 u32, set stride
  0x3f). The save manager pointer comes from the `FindScore`/`FindOrCreateScore` hooks; polled every 20 pv_sel
  frames (`MpAvatar::GamePump`). Sent as `avatar {module}` after every `welcome` and on change; the server keeps
  it per client and adds `module` to every `lobby` member (and to `admin lobbies`). Bots: `--module N`.
- Portrait pipeline (`MpAvatar.cpp`): game thread loads the sprite set and draws the sprite once at alpha 0
  to resolve its `nvn::Texture` (embedded at `obj+0x28`) and rect (2,2 408×494 in a 512×512 RGTC2 texture);
  the present thread copies mip 0+1 to a CPU buffer with an async fence (`BeginTextureReadback` /
  `PollTextureReadback`, never waits), decodes DIVA "YCbCr BC5" (mip0 = Y/A, mip1 = Cb/Cr, BT.709; rows are
  stored bottom-up), crops head-and-shoulders (side 0.37 × card width, top 15 % of the crop above the
  figure's first opaque row) and uploads an RGBA texture (`CreateRgbaTexture`, backend slots 2–15). Cache of 10
  modules, LRU, texture frees deferred 4 frames. Unknown/custom modules (no sprite) fall back to a silhouette.
- Threading rule: game sprite calls (`LoadSprSet`/`UnloadSprSet`/`CheckSprSetLoading`/`DrawSprite`) run
  without the avatar mutex, because the render thread takes it in `Get()`; the readback never blocks the
  present thread.
- Fixed on the way: `SayHello` parsed the whole receive buffer, so a reconnect whose welcome arrived together
  with the lobby resync failed with `unexpected reply` and retried; it now parses only the first line.
- Debug: `admin.py avatar_peek | avatar_poke N | avatar_dump N` (see `AGENTS.md`).
- Long-term plan for selectable poses: `docs/AVATAR_POSES_PLAN.md`.
- Verified on Ryujinx: 7-member room (Miku, Rin, Len, Luka, KAITO, Kurt, custom → silhouette); `avatar_poke`
  40 → 0 updates the local avatar and the server; changing Vocal 1 of Shared Set A to KAITO in the game's own
  Customize screen and back to Miku; Ryujinx restart rejoins the 7-member room without `unexpected reply`.
  Real Switch: 2 boots idled 5.5 min each through the SEGA logo / intro / attract loop (one connected to the
  server) with no freeze. The avatar screens themselves have not been captured on the console yet.
- **Open bug: boot/attract-movie freezes on Ryujinx.** The picture stops during the SEGA logo, intro or an
  attract PV; the process keeps running and the mod's net thread still answers. Seen with pre-avatar builds and
  with avatar builds (2 of 4 boots on 2026-11-01, back to back, at ~14:16 and ~14:21). Not reproduced on
  the real Switch so far. Root cause unknown; not an accepted emulator quirk. Next step: Ryujinx GDB stub
  stack dump of the game threads during a freeze, and an A/B with the multiplayer overlay disabled.

## Round 8: results/pause menus driven blind, ties, song-select following (2026-11-02)
Commits `815f20b`, `79e244c`, `ce0a059` (on top of `f9f16c6`).
- **Menu model (`mp/MpMenu.cpp`).** Fed from `input::Apply` for the game's own GetNpadState polls (after blocks
  and injects), it counts the presses the game actually receives, keeps a page/cursor model, and strips input
  the model says can't land (presses inside a page's hold time are swallowed too, so game and model agree).
  Results: SCORE (hold 2 s after StageResult init; A passes, 1.2 s hold) → VP on `win_vp_in` (2 s hold) → A →
  MENU cursor 0 (1 s hold; Up/Down toggle, 350 ms hold) → A on Return → EXIT. Pause: opens at cursor 0 (350 ms
  hold), 5 items wrapping (150 ms hold). On a tracked page the sticks are zeroed and every button the menu
  can't use is stripped. A 4 s EXIT without a StageResult dest resyncs the model to MENU on Return.
- **Rooms with 2+ members.** A on Retry is swallowed in both menus with a banner; in the pause menu Lag Config
  and Options too (sub-menus would desync the count). Return to Song List from pause leaves the round and sends
  the `final`. Safety net: after a results dest with no pv_sel init, a song clock that keeps running = a retry
  slipped through → the client pauses and leaves via Return to Song List. Solo rooms are vanilla.
- **Results auto-return.** When the round is over (`round_end` after the leader/owner left results, or a launch
  while on results) the client walks SCORE → VP → MENU → Return by itself once its results screen has loaded
  (~4–6 s), with all real input blocked meanwhile.
- **Group pause.** The local game follows the server state through a per-frame reconciler (forced Plus, 600 ms
  apart, one per 2 s after 5 misses). Real Plus → `pause_req`/`resume_req`; A on Resume and B → `resume_req`.
  Real-input edges are now combined across Npad styles, which fixes one Plus press on a Pro Controller /
  sys-botbase producing ~6 requests. Server: pause votes ignore players who already finished.
- **Ties.** One competition-rank helper (1-2-2-4 on pct, then score) for standings and the live board. Results
  screen: tied players share a row, hexagons side by side under one label (max 3, then +N). Web: tie card.
- **MP Live.** Top-right below the note-rating icons; shared rank, name, % and a life bar per player.
- **Song-select following.** Availability comes from the selector's lists, not pv_db: a song missing everywhere
  → no jumps, `sel_ack has:false`, "You don't have <song>", a launch on it spectates; missing only at the
  follower's difficulty → switch to the closest difficulty with a chart. Max 3 failed jumps per target. The
  picker sees "Not available for: <names>". The per-step "Leader picked" banner is gone. This explains the
  admin `jump 700 → got 611`: Ievan Polkka isn't in the emulator's selectable lists, so the selector stays put.
- Verified on 2 Ryujinx instances: auto-return (incl. round_end arriving while still in the song), Retry
  swallowed on results, owner pause/resume, B → resume_req, retry recovery (forced with `admin.py menu_cursor 1`),
  solo-room vanilla pause and Retry, tie row + MP Live screenshots, browsing matrix (steps, held scroll, L/R, Y,
  difficulty, A+B, quick jump), missing songs (bot picker, pv 59/700), closest-difficulty switch, picker pill.
  Real Switch: results Retry swallowed and manual exit, follower launch + synced start.

### Round 8 addendum (commits `9a9afea`, `cae89dc`, `7836bb3`)
- Ranking: tied players share the LOWEST position of their group (two tied for first are both 2nd; 1st + two
  tied for 2nd -> 1, 3, 3) on standings, live boards and the web; winner styling stays on the top group.
- Results screen: a tie is one connected cluster about one hexagon in size (2 = diagonal pair r=30, 3+ =
  triangle r=26, 4+ shows a "+" in the third slot); names stacked one per line, local player first, 4+ ends with
  "and X others"; rows below a tie move down. Web: tie card.
- "Waiting for players" popup shows for every member; in-song banners are small at the top-left; followers keep
  full input in the module picker / customize.
- Partial AutoPlay cheat for testing: ZR x3 in song select cycles 100/75/50/25 %, `admin.py autoplay on --pct N`.
- MP works with `new_classics = false` (MpSceneHooks); FT UI is still unsupported.

## Round 9: real portraits on the web, AutoPlay percentage (2026-11-02, session 4)
- Web avatars are the real module portraits. Each console uploads its OWN portrait once per module per
  connection: `avatar_img {module,w,h,rgba}` = 96x96 RGBA (alpha-weighted box downscale of the decoded
  in-game crop), base64, sent as one ~49 KB line through a separate 64 KiB blob slot in `MpNet` (the 510-byte
  ring can't carry it; a failed blob send ends the session so the stream never half-writes). The server keeps
  the first upload per module (`server/avatars.py`, PNG encoded with stdlib zlib; console readline limit raised
  to 128 KiB) and serves `GET /api/avatar/<module>.png`; snapshots carry `avatar` (URL or null) on consoles,
  members and standings. Portraits are game assets: memory only, never committed. Custom modules keep the
  placeholder hexagon.
- AutoPlay percentage: the server keeps `autoplay_pct` from `autoplay {on, pct}`; the web badge reads
  `AUTO 50%`. In game, a gold `AUTO` / `AUTO N%` pill sits under the life gauge for the whole song (stamped
  from TaskPvGameCtrl, hidden on results; in-song MP banners stack below it), and the song-select badge shows
  the same pill under the Multiplayer badge (it used to overlap the title and was unreadable).
- ZR shortcut: in song select 1 tap = MP overlay, 2 = No Movie, 3 = partial AutoPlay (cycles 75/50/25/100),
  4 = nothing (by design), 5 = toggle AutoPlay. Verified with real key presses on Ryujinx.
- Ryujinx boot freezes all happened in the SEGA logo / opening movie (`TaskAdvLogo`); the test roots now run
  with `skipAdv = true`, which skips it (see the boot tally in `docs/NEXT_SESSION_TODO.md`).
- Avatar uploads are admission-gated and sanitized server-side (`avatars.py` UploadGate + parse_upload: own
  announced module only, rate-limited, square 32..96, size checked before decoding, blank/flat rejected,
  normalized to 96x96, transparent RGB zeroed, re-encoded; nosniff/CSP sandbox/CORP on responses).
- Results/pause menu holds count FRAMES (`menu::Frame()` from `mp::OnFrame`), not ms: the game's results input
  locks are frame-based animations and the emulators run at 21-50 FPS with 3 instances. Results defaults
  96/66/120/51/21 frames (init/score_a/vp/menu/nav). VP stays at 120 because cleared results lock longer than
  failed ones; a real "VP done" signal is still to be found.
- In song: other players' percentages are "race fills" inside the game's bottom gauge (per-player colours;
  only their lead beyond the local fill, pointers for everyone; MP Live rows carry matching colour chips),
  and moving up / down a place flashes a soft green / purple glow from the screen edges.

## OPEN WORK (user requests, in priority order)
1. **Lobby roles + song-pick mode.** Track `owner` (lobby creator; passes to the next member in join order if
   they leave) separately from `leader` (current song picker). Owner chooses `pick_mode`:
   `owner` (owner always picks), `assigned` (owner assigns a member), `rotate` (current behavior: rotate after
   each round). Server: `set_pick_mode {mode, cid?}` (owner only); `lobby` msg gains `owner`, `pick_mode`.
2. *(Done; reworked in Round 8: pause-menu navigation is allowed, Retry/Lag Config/Options are swallowed.)*
   **Group pause.** During a round, a real Plus press is intercepted (Plus stays blocked) and sent as
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
5. *(Superseded: the leader/owner leaving results ends the round and moves everyone on, see Round 8.)*
   **Results: no auto-advance.** Remove the results auto-return; players leave results when they choose.
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
9. UI polish: in-song leaderboard / waiting / standings windows still use small default ImGui text (the lobby
   screens were enlarged in round 6; the status chip is now debug-only).
10. Percent markers for other players on the bottom % gauge (calibrate pointer x vs pct from screenshots).
11. Make MP independent of `new_classics = true` (pv_sel/results hooks are only installed when NC is on).
12. Milestone 5: web spectator client — **implemented on branch `web-frontend`** (`server/web_bridge.py`, `web/`;
    see AGENTS.md "Web spectator/admin UI"). Verified with bots/fake consoles only, not with real consoles yet.
    Follow-ups: real accounts (`docs/ACCOUNTS_DESIGN.md`), web pause/resume-all, ownership transfer.
13. **Beat-by-beat judgment timeline.** Clients already send aggregate `jc:[5]` counts (COOL/GOOD/SAFE/BAD/
    MISS) in `final`, which the web results screen renders as per-player breakdowns. A real per-note or
    per-second judgment timeline (for richer analysis — e.g. where in the chart each player missed) is not
    reported today and would need a new field in the client `final`/`state` message plus server forwarding.

## Round 10: discovery that always finds the server, custom-module portraits (2026-11-04, session 5)
- **Discovery.** A stale `server_ip` used to pin the client to a dead address forever (the Switch still had `10.169.0.37` on the phone hotspot). Now each pass tries the last session's address, `server_ip`, then a discovery round: broadcast, subnet broadcast, 127.0.0.1, `server_ip` and unicast to every host of the subnet (hotspots drop broadcasts). Real Switch: connected 16 s after launch with the stale `server_ip`; Ryujinx with an empty `server_ip`: 27 s after launch. Server: LAN addresses at start, Windows Firewall check (`server/netcheck.py`), discovery log rate-limited per source.
- **Module table.** `mp/MpModuleDb.cpp` captures the merged gm_module_tbl (layout in AGENTS.md). The user's Mikudayo pack reuses official ids 437/438 (White T-Shirt MEIKO / Breadful T-Shirt on a console without the pack): that is why Ryujinx showed the wrong module for the Switch's Mikudayo.
- **Portraits.** `avatar {module, chara, mh}`; members carry `chara/mh/img`. A console only uses its own image when its table entry has the same signature; otherwise the uploaded portrait if the player opted in (`remote_avatars`, default off, "Custom portraits: Off | On" on the MP home screen), else that character's default module. Safety: opt-in; server relays only on request, only for a member of the asker's lobby, rate-limited, sanitized 96x96 pixels (no image format parsed on the console, strict length/charset check, unsolicited replies ignored, nothing written to SD); per-uploader storage by content id (a custom module can no longer replace an official module's portrait on the web); moderation (`admin.py avatar_hide`, web "Hide portrait"); `--no-avatar-relay`.
- **Crop.** Big-headed modules (head band >= 0.31 x card width) blend to a crop of 1.2 x head width centred on the face; Mikudayo now shows eyes and mouth with the head still overflowing the frame. Other modules are unchanged.
- Verified (Switch + Ryujinx, room MMSN): Switch shows its own Mikudayo with the new crop; Ryujinx shows Yowane Haku (Mikudayo is chara HAKU in the pack's table) with the toggle off, the Switch's uploaded Mikudayo with it on, Haku again after `avatar_hide`; XHD DE:MONSTAR (895, MIKU, not on Ryujinx) -> Hatsune Miku. Module 972 (X_Song_Pack) uses texture format 37, which the portrait decoder doesn't support (silhouette on the owner's own console; others see the chara default).

## Round 11: routed-subnet discovery, MP OPTIONS screen, input-display hotkey (2026-11-10, session 6)
- **Bug (user):** on the "Connector Resident" Wi-Fi the Switch (10.167.1.61/23) never found the server (PC 10.169.1.126/23): different routed subnets, so broadcast and the own-/23 sweep (513 probes/round) never reach the PC, and `server_ip` was the PC's lease from an earlier visit (10.169.0.37). Unicast crosses fine.
- **Fix (`MpNet.cpp`):** every server the console connects to is saved to `sdmc:/MikuMikuSwitchPlugin/mp_servers.txt` (`a.b.c.d port` lines, max 4, newest first). Each discovery round also probes those addresses, and on rounds 0-5 and every 4th after sweeps the /22 around one of `server_ip` / the remembered servers in turn (blocks overlapping the own subnet are skipped). Log: `discovery #0: me 10.167.1.61/23 + sweep 10.169.0.0/22, N probes -> server ...`, `remembered server ...`.
- **Verified:** Switch with the user's original config (stale `server_ip = 10.169.0.37`) and no `mp_servers.txt`: `DISCOVER from 10.167.1.61 -> replied` and REJOINED 16 s after launch; `mp_servers.txt` then held `10.169.1.126 7777`.
- **MP OPTIONS screen:** notification style and custom portraits moved off the HOME screen (they sat over the three cards) to an OPTIONS screen, opened by the gear "Options" button right of the status pill. Verified on Ryujinx with pad input (Down from CREATE ROOM reaches Options, A opens, Right+A saved `notify_style = "text"`, B returns to HOME with Options focused) and on the Switch by touch (Options, Done).
- **Small keyboard in song select (user):** the plugin's input display in Keyboard mode. L3+R3 now toggles only on release of a pure >= 0.8 s hold, Keyboard mode only with `enable_keyboard` (false on the Switch and Ryujinx), banner on every toggle. Verified on Ryujinx: pure hold -> "Input display: Gamepad", again -> "Input display off" (Keyboard skipped), L3+R3+A -> no toggle. L3/R3 still reach the game (R3 cycles the module set on song select).
- **MP keyboard cleanup:** `DrawPanel()` runs every frame, so the Edit Name / Join Room keyboard closes with the overlay; verified on the Switch (`admin.py overlay close` while editing the name). The song-search (KB) keyboard is left alone (verified).
- sys-botbase button clicks and remote taps named `DOWN`/`RIGHT` don't drive the MP overlay; the client's tap names are case-sensitive `Up/Down/Left/Right`, combos comma-separated (`tap LStick,RStick --hold 1200`).

## Round 12: Mario-Kart style in-game placement badge + gauge marker avatars (2026-11-10)
- **Top-right placement badge (`DrawLeaderboard` in `MpUi.cpp`):** replaces the old multi-row `MP Live` window during songs with a compact Mario-Kart style ordinal placement badge (`1st`, `2nd`, `3rd`, `4th`, `5th`, ...) styled to match Project DIVA's HUD (`(W - 52, 70)` in 720p, right below the score counter).
  - Per-rank saturated top-to-bottom vertical gradients (`ImFont::RenderChar` emits one quad per glyph with top/bottom vertices at `t=0`/`t=1`, so both stops are kept saturated like `GlowText` with no white wash): Radiant Gold -> Deep Amber-Orange + twinkling 4-point DIVA sparkles (`1st`), Ice-Blue Silver -> Cobalt Steel (`2nd`), Copper-Gold -> Crimson-Bronze (`3rd`), Miku Turquoise -> Ocean Teal (`4th`), Neon Violet -> Royal Purple (`5th`–`8th`), with unified dark outer rim, crisp white inner keyline, drop shadow, soft outer glow, forward italic shear, and an angled DIVA backing plate with rank-coloured bevel trim and diagonal speed stripes.
  - Animations: slide-in + elastic pop on song entry; 3D horizontal flip (`s_placePrevRank` rotating edge-on in 0..110 ms, swapping to `s_placeDispRank` at 110 ms with elastic bounce on rank-up or drop shake on rank-down), expanding angled parallelogram shockwave ring, burst of 4-point sparkles on rank-up, and animated rising `^^` / falling `vv` double chevrons on the left of the badge (`admin.py glow up|down` also fires the placement animation).
  - Non-interference with gameplay: drawn on the background draw list with no bulky window box; `OnPvGameFrame` checks active unhit targets (`g_current_game->target`) and smoothly fades the badge to 26% opacity whenever a note target or flying button enters the top-right (`x > 1050, y < 160` in 720p).
- **Bottom gauge marker avatars + palette (`DrawGaugeRace`, `kRacePalette` in `MpUi.cpp`):** each other player's resolved portrait (`mp::avatar::Resolve`) is drawn in a mini pointy-top DIVA hexagon (`r = 13.5` in 720p, framed in that player's `RaceColor` gradient with white keyline) directly above their coloured triangle pointer on the bottom `%` gauge (`y = 667.5`). When markers sit within 25 px of each other, avatar centres fan out horizontally with a colour-matched stem connecting each hexagon to its exact `%` triangle tip. Swapped white out of `kRacePalette` in favour of Hot Pink (`(255, 75, 145)`) and Mint-Emerald (`(45, 225, 165)`).
- **Results-screen crash fix (`init.cpp`, `MpLobby.cpp`, `ModLoader.hpp`, `result.cpp`):** crash report `01794330230_01001cc00fa1a000.log` symbolized to `StageResultSwitchDetailDispHook` -> `nc::DrawResultsWindowText` -> `libcxx_map::clear()` racing on `newlib`'s heap against `MpNet` (`MpLobby::PushLine` allocating `std::deque<std::string>` while receiving `board` messages from still-playing emulators), because `newlib`'s `__malloc_lock`/`__malloc_unlock` called weak no-op stubs `__libc_lock_acquire_recursive`/`__libc_lock_release_recursive`. Implemented a real atomic recursive `__malloc_lock`/`__malloc_unlock` keyed by `TPIDRRO_EL0` in `init.cpp`, replaced `MpLobby`'s `std::deque<std::string> s_inQ` with a fixed static ring buffer, replaced `libcxx_map::clear()`'s `std::vector` with a fixed stack array, and guarded `nc::DrawResultsWindowText` with `nc::ShouldUseConsoleStyleWin()`.
- **Verified:** 6-player and 4-console rounds across 3 Ryujinx emulators (`Ryu1..3`) + physical Switch (`Evan`) + follower bots, including the Switch failing early into results and waiting 60+ s while the emulators finished the song and auto-returning to song select (`mm-re/screens/v6_nx_1st_gold.jpg`, `v6_nx_results_alive.jpg`, `v6_ryu1_2nd_pinkbar.png`).

## Round 13: KNOWN_BUGS.md items (2026-11-16)
- **B on song select no longer starts a round:** the leader commit in `OnPvSelFrame` fired on any entry into
  pv_sel state 10, and B (back to the main menu) is 6 → 10 → Dest. It now needs the confirm-panel edge 8 → 10
  (`s_lastSelState` is also reset on pv_sel init). Verified on Ryujinx: after a round, the new picker on the
  unscrolled cursor pressed B → `state=10` / `DEST`, no `start_req`, no launch.
- **Clear status from the console:** `state` carries `border` (the song's clear %), `final` carries `border`,
  `clear` (the console's verdict on the FINAL state: life left and pct >= border; never after a pause-menu
  quit), `drop` (all life lost) and `quit`. The server passes them through `board`/`standings` and the web
  snapshot; the web results use the console's verdict (old clients: the reported border; no border: no label)
  and the live gauge draws each player's own border tick. Verified: Ryu1 99.40 % vs border 70 → clear, Ryu2
  life 0 → drop (the game showed NOT CLEAR… DROP×OUT too).
- **Final resent after a reconnect:** a session that dropped right after the results screen opened lost
  the finals of Ryu1/Ryu2 on the online server (all four shim sessions dropped at once 3 s later; the
  round then completed through the 30 s stale rule with no clear verdict). The client keeps its last
  `final` line and resends it on `welcome` while still in RESULTS for that round (`[MP-L] reconnect:
  resending final`); verified by restarting `cloudshim.py` on the results screen. Online (Render) test
  2026-11-16, 4 Ryujinx: borders 70/70/50/60, Ryu3 cleared at 67.68 % against 50 %, Ryu4 drop.
- **Drop-out notification:** the server sends `event {kind: "drop_out"}` once per member and round when a
  player who had life reaches 0 (`state`), or when their `final` has `drop`. Consoles route it like combo /
  low_life (portrait popup tag "DROP OUT", text banner "X dropped out!", or off; hidden on the results
  screen); the web feed shows it. Verified on Ryujinx (server log `dropped out`).
- **Room session ranking:** per member `wins`, `placement_score`, `places` (times ranked 1st/2nd/3rd as shown
  on the results, 1-3-3-4 ties) in `lobby.py`; applied once per round when its standings complete
  (`_apply_session_ranking`): N = the round's players including members who left mid-round (placed last,
  0.0), P = 1-indexed placement with ties averaged, wins += (P == 1), placement_score += (N-P)/(N-1); rounds
  with N < 2 don't count; a member who leaves keeps their stats in `Lobby.session_kept` and gets them back on
  rejoining, until the lobby empties and closes (new joiners start at 0). Order: wins, then placement
  score. Consoles get `sr: [pos, wins, 1st, 2nd, 3rd]` per member in `lobby`; the ROOM screen's podium is the
  session top 3 and the list is in session order with identical flat grey trophies (tall cup with handles and
  the place number 1/2/3, short stem and base; `DrawTrophy`/`DrawTrophyRow`) and counts; the podium has no
  sparkles and takes half the panel width so the list has more room. The
  web Members panel is sorted the same way with trophies, and song select shows a "Session ranking" top 3.
  Note: backing out of song select still leaves the room after 8 s (existing behaviour); the stats come back
  when that player rejoins.

## Round 14: platform icons, per-player difficulty, Same difficulty option, missing-song tag, Random pick (2026-11-23)
Server, web, Switch and PC (`pc-client-mod/`), all on `main` (pc-port was merged into it first).
- **Protocol additions** (docstring of `server/lobby.py`): hello `plat: switch|pc` (older clients are told apart
  by `mod` "pc-..." / `game` "mm+..."; bots are `bot`); `diff {d, e}` = the console's song-select difficulty,
  sent on change; `loaded` / `final` carry `diff`/`ed` = the chart actually played (frozen for the round, so
  flipping the difficulty on song select with the results up doesn't change the standings); `set_opt {key:
  "same_diff", value}` (owner only). `lobby` carries `same_diff` and per member `plat`, `diff`, `ed`, `nosong`
  (that console acked `has:false` for the picker's song; sent to EVERY member, cleared on the next `sel`);
  `board` / `standings` rows carry `diff`/`ed`; event `same_diff`.
- **Party screen:** a Joy-Con pair (Switch, Mega Mix) or a monitor (PC, Mega Mix+) after every name, podium
  and list (`IconPlat`). Web: the same icon as a badge on the member tile and in "Online consoles".
- **Same difficulty (ROOM CONFIG -> Difficulty: FREE | SAME; PC panel: Difficulty combo; web host controls;
  `admin.py room_opt CODE same_diff on|off` server-side, `admin.py --cid C same_diff on|off` through a
  console).** Followers are moved to the picker's (d, e) by the normal follow jump (`ForcedDiff`,
  `s_folWantD`); when the console lacks that chart it takes the closest one (one banner per song/difficulty).
  Left/Right join the follower lock only while the picker reports a difficulty. The launch autopilot gets
  onto the difficulty before pressing A (4 tries, then starts on its own rather than miss the round). With
  no song to follow, only the difficulty is synced (`s_diffSync*`).
- **Results:** a tag in the game's difficulty colour on the bottom edge of each hexagon (tie clusters too,
  `EXEX` in the small ones), ONLY when the players weren't all on the same chart; consoles that report none
  don't count. Web: chips on the live board, the pending table and the result cards under the same rule,
  always on the member tiles.
- **Missing song:** `DrawNoSongTag` (MpUi.cpp, called from Hints.cpp / the PC badge): a slanted warm-gradient
  plate under the "picking a song" pill for everyone: "<name> doesn't have this song" + portraits, or "You
  don't have this song / <song> - you'll sit this round out". Party screen: NO SONG tag, red dot, footer in
  plain words. The old pill now only lists who has the song but can't select it. Web: striped strip on the
  song-select panel + NO SONG badge.
- **Random:** the picker's cursor stays on pv -2 on the Random entry, so nothing was mirrored until the round
  started. The rolled song (`random_pv.data2`, sel+0x260D8; PC shadow copies it) is now sent as `sel` as soon
  as A opens the confirm panel (state 8). Verified both ways: Ryu1 picker -> PC followed to pv 14 in the same
  second; PC picker -> Ryu1 followed to pv 261.
- **Server fix:** `last_activity` survived from the previous round, so in round 2+ a player who hadn't scored
  yet was "stale" the moment another final arrived and the round completed without them.
- **Verified** (Ryujinx + MM+ + scripted consoles, room GLUH/YTHN): icons, FREE/SAME from the Switch config
  screen and through the admin port, forced difficulty incl. EXTRA EXTREME -> EXTREME fallback banner,
  Left/Right lock, tags on a 1-3-3-4 tie and none when everyone was on EXTREME, both wordings of the missing
  tag on both clients, Random both directions, the stale fix. NOT verified: a real Switch, the PC Difficulty
  combo by mouse (state driven through `same_diff`), web host toggle by click (server action unit-tested).
- **Test tools** (outside the repo, `mm-re/tmp/qol/`): `puppet.py` (a console scripted through a command
  file: any message, `{round}` substituted), `webdemo.py`, `shot.py` / `pcshot.py` (window captures),
  `place.py` (both games on the second display), `t_pc_return.py` (scripted PC round).
- **Display note:** on the virtual displays (Virtual Display Driver, DISPLAY22/23) Ryujinx presented at
  2-12 fps with the GPU idle and its inputs timed out (round dropped at the 30 s load limit); on a real
  second display it runs at 60. Both games render on the RTX 3060 (Ryujinx `preferred_gpu`, MM+ got
  `GpuPreference=2` in HKCU\Software\Microsoft\DirectX\UserGpuPreferences).

## Round 15: gauge ticks over the race bars, expanded results panel (2026-11-24)
- **Gauge ticks (Switch):** the opponents' lead bars are ImGui (drawn after the game), so they covered the
  BORDER / G / E ticks. `DrawGaugeRace` now leaves a slot open at each tick (-1.4 .. +3.3 units around
  130 + 11 * pct, measured) and the game's own tick shows through. Tick % = `clear_border_eff` and
  `clear_border_eff * great|excellent % / clear_border` (game fns 0x17a410 / 0x17a440 read the difficulty
  tables at 0xb12774 / 0xb12760; same maths as the gauge's tick placement at 0x689530). The PC client
  already draws its fill through the sprite queue under the markers.
- **Expanded results panel (`DrawResultsPanel`, identical in both `MpUi.cpp`):** `standings` rows carry
  `jc:[5]` and `autoplay`. On the results screen in a room the first A never reaches the game: it opens a
  panel the hexagons fly out into, one card per player (rank, portrait, platform, chart, verdict, score,
  COOL/GOOD/SAFE/BAD/MISS bar + counts, max combo, cool rate, clear %, final %). Switch: 4 per page, L/R
  or Left/Right flip, A/B close, Y reopens. PC: 8 on one page (two columns), Enter / F7; no reopen on the
  Retry / Return page (that menu reads the keyboard device, which the block mask doesn't cover). State in
  `MpLobby.cpp` (`s_resPanel` 0 none / 1 armed / 2 open / 3 closed, `kResPerPage`); the auto-return driver
  is unaffected (its forced taps pass the block).
- **Verified:** Ryujinx, 4 and 6 players (two pages, a player still playing, DROPxOUT, AUTO tag), close ->
  Y reopen -> close -> A reaches the game (`dbg_menu` res_score -> res_vp); MM+ with 8 players on one page,
  Enter / F7 / Esc. NOT verified: a real Switch, a real pad or keyboard on PC (admin taps only).

## Round 16: room join errors (2026-11-24)
- **Before:** JOIN ROOM went back to HOME whatever the server answered; a wrong code only gave the banner
  `Lobby error: not_found`. Offline, `lobby_join` / `lobby_create` went into the send queue and reached the
  server whenever the session came back (on the Switch the greyed HOME cards still take a touch; the PC rows
  were never greyed).
- **Lobby layer (both `MpLobby.cpp`, same edits):** `CmdLobbyJoin(code, quiet)` keeps the join's state for the
  UI (`Snapshot.joinState` JOIN_IDLE / PENDING / FAILED, `joinCode`, `joinErr`). `lobby` = in; `lobby_error`
  `not_found` / `full` / `already_in_lobby` while pending = "Room ABCD not found - check the code" / "Room ABCD
  is full" / "You're already in a room"; no answer within 6 s, or the session dropping, fails it too. A code
  with anything but letters and digits is refused locally (it also broke the JSON line). Offline, join and
  create send nothing and queue nothing. A failure is a banner unless the caller asked `quiet` and the overlay
  is open to show it (admin `lobby_join` -> banner).
- **Switch (`MpUi.cpp`):** JOIN ROOM stays up after JOIN (the keyboard closes itself on submit and is reopened
  the next frame); the room screen replaces it once the server lets the console in. Right of the title:
  "Joining ABCD...", the refusal in red with a red field until the code is edited, "Not connected to the
  server" for as long as the console is offline. A touch on a greyed HOME card = cancel SE + a banner saying
  why. On text screens the keyboard window is raised over the panel every frame: a tap on the panel used to
  leave it under the panel's screen dim (every opening after the first).
- **PC (`MpPanelPc.cpp`):** Create Room and the Join button are off while offline (their help line says why).
  The join's outcome shows in the help band (yellow) with the code field outlined in pink until the code is
  edited; Enter in the field while offline -> "Not connected to a server yet - can't join a room".
- **Verified:** Ryujinx - wrong code through the keyboard's JOIN key and the dialog's JOIN button, edit clears
  the note, right code -> room screen, 8-member room -> full, admin `lobby_join` -> banner (overlay closed and
  open), server killed on the join screen -> offline note, three JOIN presses offline -> nothing reaches the
  restarted server, touch on the greyed CREATE ROOM card -> banner. MM+ (private unfocused instance, DLL =
  HEAD + this change) - wrong code by Enter and by the Join button, edit clears it, right code -> room, server
  killed -> rows off + help line, Enter offline -> note and nothing reaches the restarted server, admin
  `lobby_join` -> banner. NOT verified: the 6 s timeout and a drop while a join is pending, a real Switch, a
  real keyboard / mouse on PC (text went in as injected ImGui characters and posted `WM_CHAR`).
- **Offline input for tests** (admin taps need the server): Ryujinx takes `PostMessage` `WM_KEYDOWN` / `WM_KEYUP`
  on its top-level hwnd as pad keys and `WM_LBUTTONDOWN` / `WM_LBUTTONUP` there (client coords) as a touch,
  each about two times out of three. MM+: post `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, `WM_LBUTTONUP` back to back (the
  ImGui backend answers a posted move with `WM_MOUSELEAVE` at once); `WM_CHAR` / `WM_KEYDOWN` reach the panel's
  text fields while the window is unfocused. Nothing global, no focus change.

## Round 17: security pass (2026-11-24)
Found by reading every network-facing path (server, web bridge, both clients); each server item has a test in
`server/test_security.py`.
- **Server, anyone who can connect could stop it:** `state` / `final` fields were stored as sent, so one
  message with `pct: "x"` made the board sort raise inside the ticker and every lobby's timers stopped for good
  (also `NaN`, which came back out as JSON no browser parses). All client fields are now typed and clamped
  (`_int` / `_num` / `_text` in `lobby.py`), `NaN` / `Infinity` are refused at parse (`parse_msg`), a lobby
  that raises in `tick` no longer stops the others, and the ticker survives any exception.
- **One stalled client froze everyone:** `drain_outbox` awaited each recipient's socket, so a client that
  stopped reading blocked the ticker and all other deliveries. `Server.send` never waits; a client with more
  than 1 MiB queued is dropped. The outbox is also taken as one batch (two callers used to resend or drop each
  other's messages).
- **Other server fixes:** a `hello` with a non-text `mod` made every later hello fail; names, song titles and
  reasons were unbounded and untyped (a 200 kB or non-string name went to every console and browser); every
  cid ever seen stayed in memory; rooms are capped (2000) so `_new_code` can't spin; log lines no longer carry
  raw client text (forged lines); discovery ignores junk and oversized datagrams.
- **Web bridge:** on Windows a request path with backslashes (`/%5C%5Chost%5Cshare%5Cx`) reached
  `os.path.realpath` as a UNC path, which makes the server process open an SMB connection to `host` (any web
  page could trigger it against a local server). Paths are now checked as text before the filesystem is
  asked. Added `nosniff`, `X-Frame-Options`, `Referrer-Policy` and a CSP; `/api/state` is `no-store`; an admin
  token given with `--web-token` / `MP_WEB_TOKEN` is no longer printed; the generated one is 128 bits.
- **Web owner login by name** (anyone can read the owner's name from `/api/state`): now a switch,
  `--owner-name-login auto|on|off` / `MP_OWNER_NAME_LOGIN`; `auto` = on locally, **off with `--cloud`** (the
  admin token still works; the login dialog says so). Owners manage their room from the game.
- **Seat hijack by cid:** lobby messages give every member's cid to every member, and a hello with that cid
  replaced the connection. Clients now send a secret `key` (`[multiplayer] client_key`, 128 bits from the
  OS / kernel generator, made on first run) and the server ties each cid to the first key seen for it until
  180 s after the cid is gone (`claim_cid`); any other hello gets `cid_in_use`. A console told `cid_in_use`
  takes a new `client_id` (copied config). Old clients (no key) work as before and stay hijackable.
- **Clients (same edits on Switch and PC):** `sel_status` built two lists with `off += snprintf(...)`; with 8
  long names the offset passed the buffer and the next call wrote out of bounds (`s_notReady[128]`, a 96-byte
  stack buffer) - reachable by any server, and through the real server by room members' names. All such
  appends go through `mpjson::Appendf`. Server numbers that feed draw code are finite and clamped
  (`mpjson::ToNum`). Lines are NUL-terminated before `strstr` runs on them.
- **`dbg_cmd` is off by default:** it let whichever server a console found (LAN discovery takes the first
  answer) press buttons, move the cursor, change the saved server address and write dumps to the SD card.
  `[multiplayer] debug_commands = true` turns it back on for development; `tools/record_demo.py` and
  `mm-re/tools/ryu_multi.py setup` set it, `tools/package_emu_pack.py` clears it (and the key).
- **Verified:** 206 server tests; web typecheck, tests, build; Switch build on Ryujinx - key generated and
  sent, a hello for its cid without / with another key refused while it stayed connected, admin taps ignored
  with `debug_commands = false` and working with `true`, and a hostile server (`%TEMP%\mmsec\rogue.py`:
  oversized names in `sel_status`, out-of-range numbers, NaN, deep nesting, 50 kB lines, an unsolicited
  portrait) left it running and pinging. PC: builds with 0 warnings; NOT run by this pass. NOT verified: the
  old client builds actually crashing on the `sel_status` payload (read from the code), a real Switch.
- **Still open (by design, need bigger changes):** no TLS to consoles (the key and everything else can be
  read or changed on the path); a console trusts whatever server it finds; scores are whatever the client
  says; rooms are public (`/api/state` lists the codes) with no ban; no rate limits (proposed, not built).

## Round 18: No Fail room option, Same difficulty checked PC <-> Switch (2026-11-24)
- **Room option `no_fail`** (owner, default off): every console plays the round with the game's No Fail Mode on
  or off, whatever its own setting in the song's confirm panel. Protocol (docstring of `server/lobby.py`):
  `set_opt {key: "no_fail", value}`, `lobby.no_fail`, `launch.no_fail` = the value frozen for that round (a
  change during a round applies from the next launch), event `no_fail`. In a No Fail round the server sends no
  `drop_out` events (nobody leaves the song); `drop` in `final` / the standings keeps its meaning: the song
  ended with no life left, which the game's own results title also calls DROP x OUT.
- **How the game does it (both builds):** TaskPvGame keeps its launch data at task+0x68 and `no_fail` is one
  byte of it (Switch +0x182, MM+ +0x1BA = MMPlusMods `TaskPvGame::Data`). The pv_game init (Switch 0x1a3bf0,
  called by TaskPvGameInit 0x64c910) copies it to PVGameData (Switch +0x2D0E5, MM+ +0x2D31D, `no_fail` in both
  headers now), which is what the game tests at zero life; the HUD's difficulty label turns into `NF-...`.
  Both clients write that byte in their TaskPvGameInit hook before the original runs
  (`mp::lobby::RoundNoFail()`: the launch's value, else the lobby's; -1 = not in a room or a server without
  the option, and then nothing is touched) and log `No Fail Mode: room on|off (this game had on|off)`. The
  player's own setting is never written: the confirm panel keeps showing it (B closes that panel WITHOUT
  keeping changes; they only stick when the song is started from it).
- **UI:** Switch ROOM CONFIG - Difficulty (FREE / SAME) and No Fail Mode (OFF / ON) share one row of four
  cards, and the room footer gets "| No Fail". PC panel - a "No Fail Mode" row under Difficulty (the owner
  steps it, everyone else reads it). A banner on every console when it changes. Both hide it when the server
  sends no `no_fail`. `admin.py room_opt CODE no_fail on|off` (server side), `admin.py --cid C no_fail on|off`
  (through the owner's console), bot command `nofail on|off`. Web: the snapshot carries `no_fail` and the
  `set_opt` action takes it; there is no button for it in the web host controls yet.
- **Same difficulty** needed no change.
- **Verified** (MM+ and Ryujinx in one room on a local server, both left idle so the life gauge empties about
  33 s in): room OFF with the Switch game's own No Fail ON (`room off (this game had on)`) -> both drop out at
  zero life; room ON with both games' own setting OFF -> both play the whole song at zero life (3:41, 542
  misses, `NF-` label), no `drop_out` events; the option set from the PC panel (click), the Switch ROOM CONFIG
  (touch and pad), and both admin paths; the PC as a guest shows the row read-only. Same difficulty: PC picker
  on EXTREME -> the Switch (on HARD seconds before) launched EXTREME; Switch picker on NORMAL -> the PC launched
  NORMAL (TaskPvGame data difficulty 1, clear border 50); with no song up the difficulty alone follows within a
  second, both ways. NOT verified: a real Switch, the web.
- **Notes:** an idle No Fail player sends no `state` once the life is 0, so the 30 s stale rule stops waiting
  for them as soon as the first final arrives (bots finish after 60 s: the round ended under the PC that was
  still in its song). `--behavior follower` bots start rounds whenever the rotation makes them the picker.
  Test helpers outside the repo, `mm-re/tmp/uiview/`: `up.py` (MM+ review room with N bots), `ryu.py` (one
  private Ryujinx on root `ryu_multi/ryu4`, not in ryu_multi's state file), `wcap.py` (PrintWindow capture, works
  for both games while other windows cover them), `mon.py` (reads the PC's TaskPvGame data and PVGameData).

## Round 19: connection limits, request budgets, room web key (2026-11-24)
- **Console connections (`server/ratelimit.py` `ConnGate`, wired in `Server.handle`):** per address at most
  8 open and 20 new per minute; 10 failed hellos (nothing sent, not the protocol, `bad_hello`, `cid_in_use`)
  in a minute and the address waits out the minute; 500 open on the whole server. A refused connection is
  told why (`error` `rate_limited` / `server_full`). Only accepted connections count, so a console that keeps
  retrying isn't locked out longer. Flags: `--conn-per-ip`, `--new-conn-per-min`, `--failed-hello-per-min`,
  `--max-consoles` (0 = no limit). This machine (loopback) is never limited per address: bots, `ryu_multi`,
  the test tools.
- **Online server:** Render sees every console that comes through the Worker as one address, so per-address
  limits there only apply to an address the Worker vouches for: it sends `X-Console-IP` + `X-Console-Auth`
  when it has the secret `PROXY_SECRET` (`npx wrangler secret put PROXY_SECRET`), and the server believes
  them when `MP_PROXY_SECRET` (Render env, `--proxy-secret`) is the same value. Until both are set, online
  consoles only fall under the 500 total. Connections that reach `onrender.com/switch` directly are never
  counted per address either.
- **Request budgets (`lobby.py`):** `lobby_create` 3 / min and `lobby_join` attempts 10 / min per client
  (`lobby_error rate_limited`; the consoles show "Too many tries - wait a minute" on the join screen and a
  banner otherwise). `set_name`, `avatar`, `diff`, `ready`, `autoplay` are always applied, but each client
  can make the server resend the lobby (or `sel_status`) only 2 times a second per kind; anything beyond
  that rides on one deferred resend when the budget frees up (`Lobby.deferred`, flushed in `tick`), so the
  final state always arrives. Budgets survive a reconnect.
- **Room web key:** the owner's console sends `web_key {}` and gets `web_key {code, key, url}` - a 10-character
  key (`ABCDE-FGHJK`, 50 bits, made by the server on first request) that only that console is ever sent.
  On the dashboard, Login -> Room key gives host controls for that room (kick, song picker, room options,
  hide portrait) - the same rights the owner-name login gave, which stays off online. A new owner gets a new
  key and the old one stops working at once (also for browsers already signed in). Five wrong keys close the
  browser's connection. Switch: ROOM CONFIG -> Web Key. PC: Room page -> Show Web Key (owner only). The
  server admin token is unchanged.
- **Verified:** 234 server tests (`test_limits.py` is new), web typecheck / 48 tests / build, both clients
  build. Ryujinx: Web Key screen shows the key and the dashboard address; a browser-side WebSocket login
  with a wrong key is refused, with the key (typed lower-case with a space) it owns the room and its
  `set_opt` switched the console's ROOM CONFIG to SAME; 4 quick room creations -> the 4th refused with the
  banner; from the LAN address the 9th open connection and the 21st in a minute were refused while
  127.0.0.1 kept working. NOT verified: the dashboard's Login dialog in a browser, the PC panel row, the
  Worker + `MP_PROXY_SECRET` path on the real deployment (Worker logic tested with a stubbed fetch).

## Known quirks
- Different routed subnets (PC 10.169.x, Switch 10.167.x): found automatically once the console has connected to the PC on that network before (remembered servers) or `server_ip` is within the PC's /22; otherwise set `server_ip` once.
- Commit `8e4dc28`'s message also lists lobby fixes that actually landed in `35c196e`.
