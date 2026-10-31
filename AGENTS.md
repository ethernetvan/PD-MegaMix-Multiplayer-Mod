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
- Run: `python server/mp_server.py` (TCP 7777, UDP discovery 7778, admin 127.0.0.1:7790, web UI 0.0.0.0:8080). Windows Firewall must allow Python on private networks for real consoles.
- Tests: `cd server && python -B -m unittest test_lobby.py test_web_bridge.py -v` (`-B`: `server/__pycache__/*.pyc` are tracked, don't churn them).
- When another server may be running, test on alternate ports: `--port 17777 --discovery-port 17778 --admin-port 17790 --web-port 18080`.

## Web spectator/admin UI — `server/web_bridge.py` + `web/`
- Bridge (stdlib, same process as `mp_server.py`): serves `web/dist`, `GET /api/state`, `GET /api/health`, WebSocket `/ws` (snapshot push every 200 ms on change, activity feed, `auth`/`action` messages; schema mirrored in `web/src/types.ts`). Flags: `--web-port` (0 disables; a busy port only disables the web UI), `--web-host`, `--web-token` (default random, printed at startup with a `?token=` link).
- Access: anyone on the LAN spectates. Interim login: typing the lobby owner's in-game name unlocks that lobby's host controls (kick, pick mode, assign/make picker); the admin token unlocks every lobby. Real accounts are designed in `docs/ACCOUNTS_DESIGN.md`.
- Browsers never see raw console cids (a cid is enough to hijack a console's session): members are `mid = HMAC(per-process secret, cid)[:12]`.
- Frontend: React 19 + Vite + TypeScript (strict), no other runtime deps. `cd web && npm ci && npm run build` (the server shows a "not built" page until `web/dist` exists). Checks: `npm run typecheck && npm test`.
- Dev: `cd web && npm run dev` proxies `/api` + `/ws` to `MP_WEB_BACKEND` (default `http://127.0.0.1:8080`). Install new deps with exact pins and versions at least 7 days old (`npm install --save-exact --before=<date>`).
- Bots/sim: `server/bot.py --help`, `server/sim.py`. Admin/debug CLI: `server/admin.py` (tap/spam/block/jump/autopause/list_pvs/lobbies/kick/set_leader).
- Lobby engine is pure logic in `server/lobby.py` (protocol v1 documented in its docstring).

## Reverse-engineering artifacts
- Kept OUTSIDE the repo in `C:\Users\cl672\Documents\Coding\mm-re\` (extracted exefs, `nso_tool.py`, symbol dumps, screenshots, logs). Never commit game binaries.

## Git
- Commits must NOT be authored by or attributed to Devin: no Devin author/committer, no `Generated with [Devin](...)` line, no `Co-Authored-By: Devin ...` trailer, no Devin mention anywhere in the message. Commit as the repo's configured identity (Evan Thomas).
