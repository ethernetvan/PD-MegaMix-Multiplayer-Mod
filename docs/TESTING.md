# Testing MegaMix Switch Mods — emulator & real console

How to exercise the mod (multiplayer, overlays, AutoPlay, New Classics)
from Ryujinx and from a real Switch: every CLI command, admin/debug
command, and in-game shortcut.

Target build: Hatsune Miku Project DIVA Mega Mix, EN, TID
`01001CC00FA1A000`, update 1.0.6. Paths below:

- `REPO` = `C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods`
- `MM`   = `C:\Users\cl672\Documents\Coding\mm-re` (helpers live outside the repo)

---

## 1. Build & deploy the client mod

```bash
cd switch-client-mod
./exlaunch.sh build        # -> deploy/subsdk9 + deploy/main.npdm
./exlaunch.sh deploy-ryu   # -> %APPDATA%\Ryujinx\mods\contents\01001CC00FA1A000\mods\exefs\
./exlaunch.sh deploy-ftp   # -> real Switch sdmc:/atmosphere/contents/<TID>/exefs/ (needs FTP up)
```

Before shipping a build, verify no MP symbols got linked as dynamic
imports (a `U` line means an edit never hit the disk and the game crashes
at boot; `aarch64-none-elf-nm` is in `/opt/devkitpro/devkitA64/bin`):

```bash
aarch64-none-elf-nm -C switch-client-mod.elf | grep ' U ' | \
  grep -E 'Config::|mp::|FontLoader::|AutoPlay::|Hints::'   # must print nothing
```

The exefs folder must contain only `main.npdm` + `subsdk9`. A stale
`subsdk8` crashes at boot.

---

## 2. The server (`server/`)

The mod's multiplayer talks NDJSON-over-TCP to a Python server on the PC.

```bash
python server/mp_server.py
# TCP 7777 (clients) / UDP 7778 (discovery) / TCP 127.0.0.1:7790 (admin) / HTTP :8080 (web UI)
```

Useful flags:

```
--port N --discovery-port N --admin-port N --web-port N   # relocate everything
--countdown SECS       # pre-round 3-2-1 countdown (default 3, 0 disables)
--lobby-max N          # raise the 8-member lobby cap (stress demos only)
--no-avatar-relay      # web UI sees uploaded portraits, consoles don't
--mod-version-expected X
--cloud                # single-port mode: everything on --web-port, consoles use WS /switch
--web-token T          # web admin token (default $MP_WEB_TOKEN, else random per start)
--server-name N --public-url U   # env: MP_SERVER_NAME / MP_PUBLIC_URL
```

When a normal server might already be running, use an isolated range,
e.g. `--port 17777 --discovery-port 17778 --admin-port 17790 --web-port 18080`.

**Long-lived servers must be started detached** — one started from an
agent shell dies with the shell, and clients then carry on silently
offline (leader's song starts with no round). From PowerShell at the repo
root:

```powershell
Start-Process -WindowStyle Hidden python -ArgumentList `
  '-B','-u','server/mp_server.py','--port','37777','--discovery-port','37778',`
  '--admin-port','37790','--web-port','38080' `
  -RedirectStandardOutput ..\mm-re\tmp\sN_server.log -RedirectStandardError ..\mm-re\tmp\sN_server.err
```

Windows Firewall needs an inbound Allow for `python.exe` for real
consoles to connect; the server warns at startup if it sees only Block
rules (`server/netcheck.py`).

### Server unit tests

```bash
cd server && python -B -m unittest test_lobby.py test_web_bridge.py test_avatars.py test_netcheck.py -v
```

(`-B` keeps `__pycache__/*.pyc` from churning — they're tracked.)

### Fake clients & bots

```bash
python server/fake_client.py --port 7777 --name Test [--dbg] [--pings N]   # protocol-v1 smoke client
python server/bot.py --name Lead --create --behavior leader --sels 48,254,270 \
    --start-after 15 --load-delay 3 --song-secs 200 [--module N] [--fail-at N] \
    [--vote delay|never|instant] [--vote-delay S]
python server/bot.py --join CODE --behavior follower     # joins an existing room
python server/sim.py --bots 6 [--missing] [--straggler]  # canned multi-bot scenario
```

`bot.py` is also stdin-interactive (`help` lists commands: `status`,
`create`, `join`, `ready on|off`, `load`, `fail`, `final`, `pause`,
`resume`, `mode owner|assigned|rotate`, `next`/`prev`/`pick`/`start`).
`--behavior leader` creates + picks from `--sels` and starts after
`--start-after`; `follower` auto-acks and plays along; anything else is
manual. `sim.py --missing` adds a bot that answers `sel` with
`has:false` (availability testing) and `--straggler` a slow loader.
`--module N` sets the bot's avatar module.

### Web UI / spectator

`http://<pc>:8080` — live dashboard, lobbies, leaderboards, host controls
(incl. "Hide portrait" = server-side `avatar_hide`). Endpoints:
`/api/state` (JSON snapshot), `/api/health`, `/ws` (browser), `/switch`
(console WebSocket). E2E driver and demo recorders:

```bash
python MM/tools/web_e2e.py --mp-port 17777 --web-port 18080 --token testtoken
python MM/tools/web_keepalive.py 17777          # hold a fake lobby open; prints the code
python MM/tools/web_demo_round.py               # records a scripted ~2 min round (isolated 27777 range)
python MM/tools/web_demo_chaos.py               # same, 23 bots, --lobby-max 24
```

---

## 3. Admin / debug channel (`server/admin.py`)

The server forwards `dbg_cmd` JSON to clients. `admin.py` is the CLI:

```bash
python server/admin.py [--port 7790] [--cid CID] <command> ...
```

**Broadcast caveat:** without `--cid` every command goes to EVERY
connected client — including the real Switch. With `--cid` it goes only
to that client (`admin.py lobbies` lists cids). Always pass `--cid` for
Ryujinx when the console is online.

### Input injection (button names: `A B X Y L R ZL ZR Plus Minus Up Down Left Right LStick RStick`, comma-separate)

```
tap BTN --hold 50 --gap 50 --count 1    # press buttons (masked by lobby input blocks like real input)
spam BTN --hz 8 --secs 60               # toggle-spam a button; `spam` alone or `spamstop` stops it
block BTN[,BTN...]                      # mask buttons out of the game's input (Minus can't be blocked)
unblock                                 # clear the mask
cursor X Y [--click]                    # move the ImGui mouse cursor, 1280x720 coords; --click clicks
```

### Navigation & menus

```
overlay open|close          # open/close the MP overlay (same as ZR in song select)
jump PV                     # quick-jump the song selector to a PV id; client replies dbg_jump ok/got
autopause on|off            # pause automatically as soon as the next PV loads (replies dbg_autopause)
list_pvs                    # dump the console's song list to the server log
sel_lists PV...             # which PVs are in the selector's per-difficulty lists (and shown counts)
menu_cursor N               # force the blind results/pause menu model's cursor (retry-recovery tests)
menu_holds --init MS --score_a MS --vp MS --menu MS --nav MS
                            # tune results-menu auto-return hold times (ms @60fps)
module_dump                 # write merged module table to sdmc:/module_tbl_dump.txt (song select)
```

### Lobby control

```
lobbies                     # rooms, members, cids, live score/pct/life during a song
lobby_create                # target client creates a room (reply has the code)
lobby_join CODE             # target client joins a room
lobby_leave
kick CID                    # server kicks a member
set_leader CODE CID         # change a room's leader
```

### AutoPlay & gameplay FX

```
autoplay on|off [--pct N] [--mode M]
                            # AutoPlay; --pct = custom hit rate, deterministic per note
                            # (equal pct on two consoles produces identical scores);
                            # --mode = auto|pro|97|streak|human|clutch|tie (see the
                            # mode table under "Song select" below); the mode name
                            # also shows next to the player in the web UI badge
glow up|down                # fire the rank-change edge glow on the target client
```

### Notifications & avatars

```
notify_style avatars|text|off   # player-event popups style (saved to config.toml)
popup_test                      # fire one notification of each kind in the current style
avatar_peek                     # report the client's avatar cache state
avatar_poke N                   # overwrite Shared Set A slot 1 in memory (module-change tests)
avatar_dump N                   # decode module N's card to sdmc:/av_dump_NNN_WxH.rgba (song select);
                                # convert with MM/tmp/rgba2png.py
avatar_hide CID                 # server-side: that client's portrait is never served again
remote_avatars on|off           # toggle "Custom portraits" on the client
```

### Server mode (local LAN vs online)

```
server_mode off|local|cloud     # off = net thread idles, no sockets at all
cloud_url host[:port][/path]    # '' = default mikumod.evanthomas.dev
```

A client in `off` mode can't receive admin commands — restore by editing
its `config.toml` (`server_mode = "local"`) or `MM/tmp/set_mode.py N
local` for ryu_multi, then restart.

### Raw commands

Anything `admin.py` doesn't wrap can be sent as JSON to the admin port —
see `MM/tools/drive.py cmd '{"cmd":...}'` or `MM/tmp/adm.py` (port
37790). The client-side parser is `HandleDbgCmd` in `mp/MpNet.cpp`;
notable extra: `{"cmd":"set_name","name":"X"}`.

Server log lines to watch while testing:

- `HELLO`/`REJOINED <cid>` — client connects (REJOINED = cid already known)
- `MSG '<name>' {"t":"dbg_menu",...}` — the client's blind results/pause
  menu model (pages, cursor, swallowed presses, auto-return steps)
- `DBG` lines carry `st` (song clock); `dbg_jump`, `dbg_autopause` are
  the async replies above

---

## 4. In-game shortcuts (work on emulator and real console)

### Everywhere

| Combo | Effect |
|---|---|
| Hold Plus + Minus (0.8 s) | Main debug menu (Mot&Wind, GamestateSwitcher, SavedataBackup) |
| Hold ZL + ZR | Toggle mouse focus Game ↔ ImGui |
| Hold L3 + R3, release ≥0.8 s, no 3rd button | Input display: Off → Gamepad → (Keyboard if `enable_keyboard`) → Off |
| Hold L3 + R3 + ZL | Memory scanner (BSS); L3 inside it rescans |
| Hold L3 + R3 + ZR | Resolution scaler + post-process tuning |
| Hold L3 + R3 + Plus | FPS counter overlay |
| Hold L3 + R3 + L | Screen dimmer |
| Hold L3 + R3 + R | Volume booster (BGM/buttons/SE up to 500 %) |
| F10 (USB keyboard) | Toggle keyboard input overlay |
| Plus (in nsw_menu) | Plugin help window (the in-mod manual) |
| 1-/2-finger touch | Mouse / right-click (with `enable_touch`) |

### Song select (pv_sel)

| Input | Effect |
|---|---|
| ZR ×1 | Open the multiplayer overlay (pad mode; ZR again or B closes) |
| ZR ×2 | Toggle NO MOVIE badge |
| ZR ×3 | Cycle AutoPlay bot modes (below), turns it ON (gold pill shows the mode) |
| ZR ×4 | (nothing — reserved) |
| ZR ×5 | Toggle AutoPlay off/on outright |
| L3 + ZR | Toggle 2D MODE badge |
| Minus (on the song-confirm panel) | Chart randomizer menu: X cycles mode (Off/Partial/Full), ZL/ZR adjust per-note chance ∓5 % |
| Touch the search bar / the MP badge | Jump-to-song (ID or fuzzy name, VirtualKeyboard) / open the MP overlay |

ZR taps resolve after a 350 ms gap — tap N times then wait.

AutoPlay modes (ZR ×3 cycle; all deterministic per note — same mode +
same chart on two consoles produces identical scores):

| Pill | Behaviour |
|---|---|
| `AUTO` | Every note Cool — a perfect run |
| `AUTO PRO` | ~1/32 notes get a random degraded judgment (~50 % Fine / ~31 % Safe / ~13 % Sad / ~6 % miss) — tight 1st/2nd gaps with realistic misses |
| `AUTO 97` | One outright miss per 32 notes (~97 %) — deterministic, combo-breaks |
| `AUTO STREAK` | Drops 2 notes in a row every 32 (~94 %) — combo resets, gauge dips |
| `AUTO HUMAN` | Hits ~99 % of notes with delayed presses → Cool/Fine/Safe mix (~95 %), never fails |
| `AUTO CLUTCH` | ~65 % Cool early ramping to 100 % — scripted mid-song comeback |
| `AUTO TIE` | Fixed judgment pattern — forces a results tie |
| `AUTO n%` | Custom hit rate (only via `autoplay --pct`); <87 % drains life and will likely fail the song |

### Freecam (during a PV)

`L + R + Minus` toggles it, then: D-Pad moves, right stick looks,
B/X = down/up, L/R = zoom, hold Y + L/R = roll, hold R3 = speed boost,
A = freeze the scene without the pause menu.

### Debug mode

`L + R + Plus` toggles. Then: `L3` = engine mouse (LStick moves,
ZL/ZR click, hold Y = boost), `L + R + D-Up/Down/Left/Right` = jump to
TEST_MODE / DATA_TEST / CS_MENU / MENU_SWITCH states, `hold R3 + D-Up/Down`
= pick a debug scene, A loads it.

### Customize / New Classics

`ZL` inside the customize menu toggles the New Classics options window.

---

## 5. Ryujinx testing

Prereqs: `enable_internet_access = true` in `%APPDATA%\Ryujinx\Config.json`
(else no sockets). SD root = `%APPDATA%\Ryujinx\sdcard\`; mod log =
`sdcard\hook_log.txt` (written unflushed — lines can arrive minutes late;
a missing line proves nothing), config =
`sdcard\MikuMikuSwitchPlugin\config.toml` (`[multiplayer]` section),
Ryujinx logs in `%APPDATA%\Ryujinx\Logs\`.

Launch:

```
C:\Users\cl672\Documents\Juegos\Ryujinx\Ryujinx.exe "C:\Users\cl672\Documents\Juegos\switchgames\Hatsune_Miku_Merged.nsp"
```

`config.toml` keys that matter for tests (under `[multiplayer]`):
`enabled`, `server_ip` (usually unneeded — discovery sweeps the subnet
and remembers servers in `mp_servers.txt`), `server_port`,
`discovery_port`, `player_name`, `client_id`, `debug_status_chip`,
`notify_style`, `remote_avatars`, `server_mode` (`off`/`local`/`cloud`),
`cloud_url`. Elsewhere: `enable_keyboard`, `enable_touch`, `noMV`,
`no3D`, `new_classics`.

### Single instance, quick round

`MM/tools/v1run.sh TAG [build]` — kills old Ryujinx/server/bot procs,
optionally rebuilds + deploys, clears `hook_log.txt`, starts the server +
Ryujinx, taps through the menus, starts a leader bot (`--sels 48,254,270
--song-secs 200`), reads back the room code and `lobby_join`s the
emulator. `SONGSECS=N` env overrides the song length.

### Driver helpers

```bash
python MM/tools/drive.py tap A [hold_ms]     # admin tap via :7790
python MM/tools/drive.py shot NAME           # window capture -> MM/screens/NAME.png
python MM/tools/drive.py log [N]             # tail the server log ($SRVLOG overrides which)
python MM/tools/drive.py cmd '{"cmd":"...","cid":"..."}'   # raw dbg_cmd
python MM/tools/drive.py sleep 2
python MM/tmp/shot2.py NAME [x0 y0 x1 y1 [scale]]          # crop the 720p game frame off monitor 2
python MM/tmp/ryushot.py NAME                # capture every visible Ryujinx window
python MM/tmp/cellshot.py N NAME             # one ryu_multi tile
```

Real key presses into a Ryujinx window only work ~50 % via
`PostMessage(WM_KEYDOWN/UP)` (`MM/tmp/keypress.py`); `SendInput` never
reaches it. Prefer admin `tap` — it flows through the same masking as
real input.

### Multi-instance (`ryu_multi.py`)

```bash
python MM/tools/ryu_multi.py setup 2 --port 37777 --discovery-port 37778
python MM/tools/ryu_multi.py up 2 --admin-port 37790 [--join CODE] [--stagger 8]
python MM/tools/ryu_multi.py join CODE | status | down | shot NAME
```

Each instance gets its own `-r` root under `MM/ryu_multi/ryuN` (own
`config.toml` with `player_name` RyuN + fixed `client_id
a00000000000000N`, own save), windows tile 2×2 on the non-primary
monitor, instances 2+ are muted. `up` does NOT refresh `mods/` — after
`deploy-ryu`, copy `subsdk9` + `main.npdm` into each
`ryuN/mods/contents/01001CC00FA1A000/mods/exefs/` yourself (or re-run
`setup`, which also rewrites configs). Launcher refuses below ~3 GB free
RAM; 4 instances fit on this PC.

### Scripted test room (`tools/mp_room.py`, in the repo)

```bash
python tools/mp_room.py            # 3 clients, server + setup + up, one room, no song started
python tools/mp_room.py -n 4 --mode compete
python tools/mp_room.py cycle --mode compete   # controller only on already-up instances
python tools/mp_room.py down
```

Puts Ryu1..N in one room (Ryu1 creates) at song select — you pick the
song on the leader. `rotate` mode rotates `autoplay --pct` levels
(96/98/100) every `--period` s; `compete` runs a feedback controller over
the live standings with per-song scenarios: `seesaw`, `pack`, `comeback`,
`duel`, `runaway` (`--scenarios`, `--margin`, `--spread`, `--comeback-at`,
`--rescue-life`, `--seed`). Life below ~60 gets rescued at 100 % so
nobody fails.

### Known Ryujinx issues while testing

- Boot/attract-movie freezes are an open bug — before killing, save a
  screenshot, the Ryujinx log, and `hook_log.txt`, and note time since boot.
- Detached launches need `CREATE_NO_WINDOW|CREATE_NEW_PROCESS_GROUP`
  (`DETACHED_PROCESS` exits silently at boot).
- Run Ryujinx on the second monitor when one is connected
  (`MM/tmp/avenv.py up|server|deploy X|down` handles Config.json
  backup/restore + the isolated 37777-port environment).

---

## 6. Real Switch testing

Console runs Atmosphere with **sys-ftpd-light** (`:5000`, file transfer)
and **sys-botbase** (`:6000`, buttons/touch/screenshots). Helper:
`MM/tools/nx.py` (set `NX_IP` if the console moved networks — ask the
user; a sleeping console answers nothing, find it with a :5000/:6000 TCP
sweep of the PC's subnet).

```bash
python MM/tools/nx.py deploy            # push deploy/* -> atmosphere/contents/<TID>/exefs/
python MM/tools/nx.py log [out]         # fetch sdmc:/hook_log.txt
python MM/tools/nx.py crash [n]         # fetch newest n crash reports -> MM/crash/
MSYS_NO_PATHCONV=1 python MM/tools/nx.py get /hook_log.txt out.txt   # Git Bash: required, else 450
python MM/tools/nx.py put local /remote/path
python MM/tools/nx.py click A B X       # sys-botbase buttons, 0.3 s apart
python MM/tools/nx.py shot NAME         # screenshot -> MM/screens/NAME.jpg
python MM/tools/nx.py raw "touch 640 360"      # touchscreen tap (1280x720)
python MM/tools/nx.py raw "keyHeld HOME 1200"  # hold a button
```

Symbolize crashes:
`aarch64-none-elf-addr2line -f -C -i -e switch-client-mod/switch-client-mod.elf <switch-client-mod + offset>`.

Console-specific caveats:

- Files the game has open (`config.toml`, `hook_log.txt`) can't be read
  over FTP — close the game first (HOME → X → A).
- sys-botbase `click PLUS`/`MINUS`/`HOME` never reach the game — the
  pause menu can't be driven remotely; use `raw "touch X Y"` taps or ask
  the user.
- Launch the game remotely: `raw "keyHeld HOME 1200"` then `click A`
  (a 2 s HOME hold opens the power menu instead).
- Closing the game remotely is unreliable — ask the user.
- The console discovers the server automatically (broadcast + subnet
  sweep + remembered servers in `sdmc:/MikuMikuSwitchPlugin/mp_servers.txt`);
  set `server_ip` only if it's never reached the PC on that network.
- `MM/tmp/nxwatch.py PREFIX SECONDS` samples screenshots and flags STATIC
  frames — detects a frozen movie/attract screen.
- All admin commands in §3 work on the console too (it's just another
  client) — but they require the console connected (`server_mode`
  local/cloud, not off).

`MM/tmp/res_round.py TAG --bots Name:module,...` sets up a results-screen
round between bots and the real Switch; `MM/tmp/live_session.py up`
stages Switch + Ryujinx + web UI + follower bots for a play session.

---

## 7. Cloud / online path

The public server runs `mp_server.py --cloud` behind a Cloudflare Worker
(`mikumod.evanthomas.dev`, consoles on `/switch` over port 80 WS).

- Switch a client: `admin.py --cid C server_mode cloud` /
  `cloud_url ADDR`, or edit `config.toml`, or `MM/tmp/set_mode.py N
  off|local|cloud [URL]` for ryu_multi instances (restart after).
- Test against the local server instead of Render:
  `cloud_url 127.0.0.1:38080` (or `10-169-1-32.nip.io:38080` to exercise
  real DNS; plain WS, no TLS — a `301` in `hook_log.txt` = the host is
  redirecting to https and the Worker route is off).
- `MM/tmp/coldstart_proxy.py LISTEN TARGET HOLD` imitates a sleeping
  free-tier host (1st request 503, 2nd held `HOLD` s) — tests the 65 s
  connect/upgrade timeout path.
- Live state: `curl https://mikumod.evanthomas.dev/api/state`.
- `MM/tmp/cutproxy.py` sits between a ryu_multi instance and the server
  to drop/restore its connection on demand (reconnect-path tests).

---

## 8. Suggested test pass (smoke → full)

1. **Build sanity**: `nm` import check (§1), boot the game, no crash.
2. **Server up**: detached `mp_server.py`, `admin.py lobbies` answers.
3. **Connect**: Ryujinx (or Switch) shows `HELLO` in the server log;
   `debug_status_chip` shows CONNECTED.
4. **Input**: `admin.py --cid X tap A` navigates a menu; `block A`
   freezes navigation, `unblock` restores.
5. **Overlay**: ZR in song select opens the MP overlay; `lobby_create` +
   `lobby_join` on a second client (bot or ryu2) forms a room.
6. **Round**: leader picks a song (`jump PV` to position, then taps);
   all clients load, play (`autoplay on --pct 95`), reach results,
   auto-return to song select. Watch `dbg_menu` lines for menu-model
   sanity.
7. **FX**: `glow up|down`, `popup_test`, `notify_style` cycling,
   `remote_avatars` + `avatar_poke`/`avatar_dump`.
8. **Recovery**: kill one client mid-song (REJOINED on return), `cut`
   its connection via cutproxy, `autopause on` + retry-slip via
   `menu_cursor`.
9. **Offline**: `server_mode off` produces zero traffic; back to `local`
   rediscovers the server.
