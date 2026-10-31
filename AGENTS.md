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

## Real Switch (user's console)
- IP `10.167.0.120` (PC Wi-Fi is `10.169.0.213`, a different subnet → UDP discovery cannot reach the server; `server_ip` is set in the Switch config).
- sys-ftpd-light on :5000 and sys-botbase on :6000. Helper: `mm-re/tools/nx.py` (`deploy`, `log`, `crash`, `get/put`, `click A`, `shot NAME`).
- Files the game has open (`config.toml`, `hook_log.txt`) cannot be read over FTP while the game (or its crash dialog) is running — close it first (HOME, X, A).
- Crash reports: `sdmc:/atmosphere/crash_reports/`; symbolize with `aarch64-none-elf-addr2line -f -C -i -e switch-client-mod/switch-client-mod.elf <switch-client-mod + offset>`.
- The exefs folder must contain only `main.npdm` + `subsdk9`. A stale `subsdk8` from older mod versions crashes at boot (moved to `atmosphere/contents/01001CC00FA1A000/subsdk8.disabled.bak`).

## Server (Python 3.13, stdlib only) — `server/`
- Run: `python server/mp_server.py` (TCP 7777, UDP discovery 7778, admin 127.0.0.1:7790). Windows Firewall must allow Python on private networks for real consoles.
- Tests: `cd server && python -m unittest test_lobby.py -v`.
- Bots/sim: `server/bot.py --help`, `server/sim.py`. Admin/debug CLI: `server/admin.py` (tap/spam/block/jump/autopause/list_pvs/lobbies/kick/set_leader/overlay open|close/cursor X Y [--click], 720p coords).
- Admin debug commands without `--cid` are broadcast to EVERY connected client, including the real Switch. When the console is connected, target Ryujinx explicitly: `python admin.py --cid <cid> tap A` (cids are listed by `admin.py lobbies`).
- Real-Switch UI testing: `nx.py raw "touch X Y"` taps the touchscreen (1280x720), `nx.py shot NAME` captures it.
- Lobby engine is pure logic in `server/lobby.py` (protocol v1 documented in its docstring).

## Demo recording
- `python tools/record_demo.py [--skip-tour]` records a captioned, chaptered MP4 to `C:\Users\cl672\Documents\daniel-gdrive\pdmm\megamix-mod-demo_<date>.mp4`: boots the NSP in Ryujinx fullscreen on monitor 0 (don't use that display during the run), runs its own server on 37777/37778/37790 (`--port-base`) plus bots, tours song select + every multiplayer screen, then plays one AutoPlay round. Work files/logs: `%TEMP%\megamix_demo\<timestamp>\`.
- It patches `[multiplayer]` in `sdcard\MikuMikuSwitchPlugin\config.toml` for the run and restores it byte-identical afterwards; boot watchdog relaunches Ryujinx once on a stalled log / no connection. Capture = ffmpeg `ddagrab` + NVENC; audio from Stereo Mix is dropped automatically if it is silent.
- The full tour path has not completed an end-to-end run yet (only `--skip-tour`); the default song (pv 700) and `--song-secs 120` are unverified.

## Reverse-engineering artifacts
- Kept OUTSIDE the repo in `C:\Users\cl672\Documents\Coding\mm-re\` (extracted exefs, `nso_tool.py`, symbol dumps, screenshots, logs). Never commit game binaries.

## Git
- Commits must NOT be authored by or attributed to Devin: no Devin author/committer, no `Generated with [Devin](...)` line, no `Co-Authored-By: Devin ...` trailer, no Devin mention anywhere in the message. Commit as the repo's configured identity (Evan Thomas).
