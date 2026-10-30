# MegaMix Switch Multiplayer Mod — agent notes

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

## Server (Python 3.13, stdlib only) — `server/`
- Run: `python server/mp_server.py` (TCP 7777, UDP discovery 7778, admin 127.0.0.1:7790). Windows Firewall must allow Python on private networks for real consoles.
- Tests: `cd server && python -m unittest test_lobby.py -v`.
- Bots/sim: `server/bot.py --help`, `server/sim.py`. Admin/debug CLI: `server/admin.py` (tap/spam/block/jump/autopause/list_pvs/lobbies/kick/set_leader).
- Lobby engine is pure logic in `server/lobby.py` (protocol v1 documented in its docstring).

## Reverse-engineering artifacts
- Kept OUTSIDE the repo in `C:\Users\cl672\Documents\Coding\mm-re\` (extracted exefs, `nso_tool.py`, symbol dumps, screenshots, logs). Never commit game binaries.
