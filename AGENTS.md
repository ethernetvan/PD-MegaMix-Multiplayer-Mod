# MegaMix Switch Multiplayer Mod — agent notes

Current status, design and the prioritized open-work list: `docs/MULTIPLAYER_STATUS.md`.
Testing helpers live outside the repo in `C:\Users\cl672\Documents\Coding\mm-re\tools\`:
`drive.py` (admin taps/screens for Ryujinx), `v1run.sh` (rebuild + Ryujinx + bot round), `nx.py` (real Switch).

## Target
- Hatsune Miku Project DIVA Mega Mix, EN, TID `01001CC00FA1A000`, update **1.0.6** (v393216), main build ID `179265bd41f7781dac979e77702011241b442ba1`.
- All `InstallAtOffset` / `base + 0x...` offsets in `switch-client-mod/source/program` are main-module offsets for that build.
- Some code under `program/nc/` belongs to New Classics (a custom PV mode); its gameplay hooks (`TaskPvGame*`, `GetHitState*`) are installed unconditionally and are real game functions. pv_sel/results switch hooks are only installed when `new_classics = true` (default).

## Build / deploy (client mod)
- Toolchain: devkitPro at `/opt/devkitpro` (devkitA64, GCC 15 — `config.mk` sets `-Wno-error`).
- Build: `cd switch-client-mod && ./exlaunch.sh build` → `deploy/subsdk9` + `deploy/main.npdm`.
- Ryujinx deploy: `./exlaunch.sh deploy-ryu` → `%APPDATA%\Ryujinx\mods\contents\01001CC00FA1A000\mods\exefs\`.
- Switch deploy: copy both files to `sdmc:/atmosphere/contents/01001CC00FA1A000/exefs/` (or `./exlaunch.sh deploy-ftp`).
- Object dir is flat and Windows is case-insensitive: every source filename must be globally unique (new MP files are prefixed `Mp`).
- `thread_local` is unusable (no `__aarch64_read_tp` export). `nn::os` thread priorities are 0–31 (31 = lowest).

## Ryujinx
- Exe: `C:\Users\cl672\Documents\Juegos\Ryujinx\Ryujinx.exe "C:\Users\cl672\Documents\Juegos\switchgames\Hatsune_Miku_Merged.nsp"`; logs in `...\Ryujinx\Logs\`.
- `enable_internet_access` must be `true` in `%APPDATA%\Ryujinx\Config.json` for sockets.
- SD root = `%APPDATA%\Ryujinx\sdcard\`; mod log `hook_log.txt`, config `MikuMikuSwitchPlugin\config.toml` (`[multiplayer]` section).
- Socket `Poll` is unreliable under Ryujinx — the client uses non-blocking sockets. nn::socket uses Linux errno values (EAGAIN 11, EINPROGRESS 115).
- Multiple clients at once: `python mm-re/tools/ryu_multi.py setup N --port P --discovery-port D | up N --admin-port A [--join CODE] [--stagger 8] | join CODE | status | down`. The game ports are written into each instance's `config.toml` by `setup` (the `--port` of `up` is ignored), e.g. `setup 2 --port 37777 --discovery-port 37778` then `up 2 --admin-port 37790`. Each instance gets its own data root (`mm-re/ryu_multi/ryuN`, launched with `Ryujinx.exe -r <root>`), so it has its own `config.toml` (`player_name` RyuN, fixed `client_id` a00000000000000N), save and caches. `up` does NOT refresh an instance's `mods/`: after `deploy-ryu`, copy `deploy/subsdk9` + `main.npdm` into `mm-re/ryu_multi/ryuN/mods/contents/01001CC00FA1A000/mods/exefs/` yourself before restarting (`setup` copies them but also rewrites every instance's config). Windows tile into a 2x2 grid on the non-primary monitor (not fullscreen); instances 2+ are muted. RAM limit on this PC: **3 instances** (~2.3 GB private each; ~2.8 GB free with three up). The server logs `REJOINED <cid>` (not `HELLO`) for a cid it already knows. `ryu_multi.py shot` captures black here (Pillow grabs only the primary display): use `ImageGrab.grab(all_screens=True)` and crop.
- Real (non-injected) key presses into one Ryujinx window: only `PostMessage(WM_KEYDOWN/WM_KEYUP)` to its hwnd works, and only ~50% of the time; `keybd_event`/`SendInput` don't reach it. Prefer admin taps (`admin.py --cid ... tap X`), which are masked by the lobby's input blocks like real input.
- **User preference: run Ryujinx on the SECOND monitor** (`\\.\DISPLAY6`, x=1920, 1280x1024), never the main one. Ryujinx goes fullscreen on the monitor its saved window position (`window_startup` in `Config.json`) is on; `mm-re/tmp/avenv.py up|deploy|server|down` does this (backs up and restores `Config.json` and `config.toml` byte-identical) on the isolated ports 37777/37778/37790, and `mm-re/tmp/shot2.py NAME [x0 y0 x1 y1 [scale]]` captures the 1280x720 game frame.
- `hook_log.txt` is written without flushing on Ryujinx: lines can show up minutes late, so a missing line is not proof that code didn't run.
- Boot/attract-movie freezes on Ryujinx are an OPEN BUG (see `docs/MULTIPLAYER_STATUS.md`, Round 7), not an acceptable emulator quirk. Before killing a frozen session, save a screenshot, the Ryujinx log and `hook_log.txt`, and note the time since boot.

## Real Switch (user's console)
- IP changes with the network: currently `10.167.1.198` (/23) with the PC at `10.169.0.37` (different subnets, routed; earlier: Switch `10.167.0.120` / PC `10.169.0.213`, and `10.147.142.150` / `10.147.142.225` on a phone hotspot). Ask the user if it doesn't answer; `nx.py` honours `NX_IP`. UDP discovery is unreliable, so `server_ip` in the Switch's `sdmc:/MikuMikuSwitchPlugin/config.toml` must be the PC's current address on the same network (read at boot only). Check the PC's address with `Get-NetIPAddress -AddressFamily IPv4` (plain `ipconfig` output was stale once).
- sys-ftpd-light on :5000 and sys-botbase on :6000. Helper: `mm-re/tools/nx.py` (`deploy`, `log`, `crash`, `get/put`, `click A`, `shot NAME`). From Git Bash, set `MSYS_NO_PATHCONV=1` for `get`/`put`, otherwise `/remote/path` arguments are rewritten to Windows paths and FTP answers `450 failed to open file`.
- Files the game has open (`config.toml`, `hook_log.txt`) cannot be read over FTP while the game (or its crash dialog) is running — close it first (HOME, X, A).
- Crash reports: `sdmc:/atmosphere/crash_reports/`; symbolize with `aarch64-none-elf-addr2line -f -C -i -e switch-client-mod/switch-client-mod.elf <switch-client-mod + offset>`.
- The exefs folder must contain only `main.npdm` + `subsdk9`. A stale `subsdk8` from older mod versions crashes at boot (moved to `atmosphere/contents/01001CC00FA1A000/subsdk8.disabled.bak`).

## Multiplayer client: menus, input, song availability
- The game gives no signal for the cursor in the results end-menu or the pause menu (probed: no AET layer call, no PVGameData byte). `mp/MpMenu.cpp` tracks both blind: it counts the presses the game actually receives (after blocks/injects, debounced across Npad styles) and strips input it says can't land. Real anchors: StageResult init/dest, the `win_vp_in` layer (results page 2), `pv->paused` edges, pv_sel init. Details and timings: `docs/MULTIPLAYER_STATUS.md` Round 8.
- Rooms with 2+ members: Retry is swallowed in both menus (Lag Config/Options too in the pause menu); a retry that slips through is detected (song clock running after results with no pv_sel init) and undone via pause -> Return to Song List. Solo rooms are vanilla.
- `input::Apply` runs once per GetNpadState hook (Handheld/FullKey/JoyDual, called by the game and by `nc/input.cpp`) plus our merged poll: anything edge-based must combine the sources (see `MpInput.cpp`), otherwise a held button on a Pro Controller or sys-botbase looks like a new press on every poll.
- Which songs a console can select = the selector's `sorted_pv_lists` (sort mode x difficulty x edition, `SongQuickJump::PvSelectableAt`), not pv_db: pv_db lists every official song including DLC the console doesn't own.

## Server (Python 3.13, stdlib only) — `server/`
- Run: `python server/mp_server.py` (TCP 7777, UDP discovery 7778, admin 127.0.0.1:7790, web UI 0.0.0.0:8080). Windows Firewall must allow Python on private networks for real consoles.
- Tests: `cd server && python -B -m unittest test_lobby.py test_web_bridge.py test_avatars.py -v` (`-B`: `server/__pycache__/*.pyc` are tracked, don't churn them).
- When another server may be running, test on alternate ports: `--port 17777 --discovery-port 17778 --admin-port 17790 --web-port 18080`.
- A test server started from an agent shell dies when that shell is torn down (it looked like random crashes twice; consoles then carry on offline, e.g. a leader's song starts without a round). Start long-lived servers detached, from the repo root: `Start-Process -WindowStyle Hidden python -ArgumentList '-B','-u','server/mp_server.py','--port','37777','--discovery-port','37778','--admin-port','37790','--web-port','38080' -RedirectStandardOutput ..\mm-re\tmp\sN_server.log -RedirectStandardError ..\mm-re\tmp\sN_server.err` (PowerShell), then hardlink `mm-re/tmp/live_server.log` to the new log for `ryu_multi.py`.
- `--lobby-max N` raises the per-lobby member cap (default 8, which is what consoles support) for stress demos.
- Bots/sim: `server/bot.py --help` (`--module N` sets the bot's avatar), `server/sim.py`. Admin/debug CLI: `server/admin.py` (tap/spam/block/jump/autopause/list_pvs/lobbies/kick/set_leader/overlay open|close/cursor X Y [--click], 720p coords; avatars: `avatar_peek` (cache state), `avatar_poke N` (overwrite Shared Set A slot 1 in memory), `avatar_dump N` (write module N's whole decoded card to the SD root as `av_dump_NNN_WxH.rgba`, song select only; `mm-re/tmp/rgba2png.py` converts); `menu_cursor N` (force the blind results/pause menu model's cursor, e.g. to test the retry recovery); `sel_lists PV...` (which PVs are in the selector's per-difficulty lists and how many are shown); `autoplay on|off [--pct N]` (partial AutoPlay: hits N % of the notes, deterministic per note, so equal N on two consoles ties; ZR x3 in song select cycles it on the console); `lobby_create`, `lobby_join CODE`, `lobby_leave`; `menu_holds --init/--score_a/--vp/--menu/--nav MS` (results-menu hold times in ms at 60 fps, converted to frames)).
- Server log: `MSG '<name>' {"t":"dbg_menu",...}` lines trace the client's results/pause menu model (pages, cursor, swallowed presses, auto-return driver steps, retry recovery); `DBG` lines include `st` (song clock).
- Admin debug commands without `--cid` are broadcast to EVERY connected client, including the real Switch. When the console is connected, target Ryujinx explicitly: `python admin.py --cid <cid> tap A` (cids are listed by `admin.py lobbies`).
- Real-Switch UI testing: `nx.py raw "touch X Y"` taps the touchscreen (1280x720), `nx.py shot NAME` captures it. sys-botbase clicks drive A/B/X/Y, the D-pad, L/R and ZL/ZR, but `click PLUS`/`MINUS`/`HOME` never reach the game on this console (pause can't be tested remotely). `nx.py raw "keyHeld HOME 1200"` then `click A` launches the game from the HOME menu; a 2 s hold opens the power/sleep menu instead. Closing the game remotely failed (HOME, X, A didn't work): ask the user.
- Lobby engine is pure logic in `server/lobby.py` (protocol v1 documented in its docstring).

## Web spectator/admin UI — `server/web_bridge.py` + `web/`
- Bridge (stdlib, same process as `mp_server.py`): serves `web/dist`, `GET /api/state`, `GET /api/health`, `GET /api/avatar/<module>.png` (a console's uploaded portrait, first-per-module), WebSocket `/ws` (snapshot push every 200 ms on change, activity feed, `auth`/`action` messages; schema mirrored in `web/src/types.ts`). Flags: `--web-port` (0 disables; a busy port only disables the web UI), `--web-host`, `--web-token` (default random, printed at startup with a `?token=` link).
- Access: anyone on the LAN spectates. Interim login: typing the lobby owner's in-game name unlocks that lobby's host controls (kick, pick mode, assign/make picker); the admin token unlocks every lobby. Real accounts are designed in `docs/ACCOUNTS_DESIGN.md`.
- Browsers never see raw console cids (a cid is enough to hijack a console's session): members are `mid = HMAC(per-process secret, cid)[:12]`.
- Frontend: React 19 + Vite + TypeScript (strict), no other runtime deps. `cd web && npm ci && npm run build` (the server shows a "not built" page until `web/dist` exists). Checks: `npm run typecheck && npm test`.
- Styling follows the game's own visual language (see `mm-re/screens/m1b_menu1.png`, `v1c_sel2.png`, `ui_room.png`): design tokens in `styles.css` `:root`, chevron-tag bars, parallelogram chips, dark translucent data panels on a bright teal confetti backdrop. Player icons are hexagons via `components/HexAvatar.tsx` — currently gradient placeholders; pass `src` to show a real icon once the server sends one.
- Dev: `cd web && npm run dev` proxies `/api` + `/ws` to `MP_WEB_BACKEND` (default `http://127.0.0.1:8080`). Install new deps with exact pins and versions at least 7 days old (`npm install --save-exact --before=<date>`).
- Web UI demo recordings (headless Edge, nothing on any monitor): `mm-re/tools/web_demo_round.py` (3 players) and `web_demo_chaos.py` (23 players, 1080p60 screencast capture); both run their own server on 27777/27778/27790/28080.

## Demo recording
- `python tools/record_demo.py [--skip-tour]` records a captioned, chaptered MP4 to `C:\Users\cl672\Documents\daniel-gdrive\pdmm\megamix-mod-demo_<date>.mp4`: boots the NSP in Ryujinx WINDOWED on the second monitor (never fullscreen/the primary; it backs up `Config.json`, sets `window_startup` to that monitor, restores byte-identical, and no longer kills other Ryujinx instances - it refuses to run while a main-root Ryujinx is open; `--monitor` overrides the auto-picked ddagrab output, `--crop X,Y,W,H` the measured game rect), runs its own server on 37777/37778/37790 (`--port-base` must avoid a running test server - use e.g. 47777 if 37777 is taken) plus bots, tours song select + every multiplayer screen, then plays one AutoPlay round. Work files/logs: `%TEMP%\megamix_demo\<timestamp>\`.
- It patches `[multiplayer]` in `sdcard\MikuMikuSwitchPlugin\config.toml` for the run and restores it byte-identical afterwards; boot watchdog relaunches Ryujinx once on a stalled log / no connection. Capture = ffmpeg `ddagrab` + NVENC; audio from Stereo Mix is dropped automatically if it is silent.
- The full tour path has not completed an end-to-end run yet (only `--skip-tour`); the default song (pv 700) and `--song-secs 120` are unverified.

## Reverse-engineering artifacts
- Kept OUTSIDE the repo in `C:\Users\cl672\Documents\Coding\mm-re\` (extracted exefs, `nso_tool.py`, symbol dumps, screenshots, logs). Never commit game binaries.

## Git
- Commits must NOT be authored by or attributed to Devin: no Devin author/committer, no `Generated with [Devin](...)` line, no `Co-Authored-By: Devin ...` trailer, no Devin mention anywhere in the message. Commit as the repo's configured identity (Evan Thomas).
