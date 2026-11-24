# PC Port Guide — MegaMix Switch Mod: Features & Architecture

This document is the complete feature catalog and architecture reference for the
Nintendo Switch Project DIVA Mega Mix mod. It is written for whoever ports the mod
to the PC/Steam release (**Hatsune Miku: Project DIVA Mega Mix+**).

It describes **what exists**, **how it works**, and **how each piece should be
treated in the port** — including which UI elements must be restyled for PC and
which must be reproduced pixel-for-pixel.

> Status of this document: describes the Switch implementation as it exists today.
> Where something is buggy, partial, or design-only, it is marked. Existing docs
> (`MULTIPLAYER_STATUS.md`, `KNOWN_BUGS.md`, `FUTURE_TODO.md`, `ACCOUNTS_DESIGN.md`)
> carry the day-to-day detail; this doc is the porting contract.

---

## Table of contents

1. [What this mod is](#1-what-this-mod-is)
2. [The porting rule: restyle vs. parity](#2-the-porting-rule-restyle-vs-parity)
3. [Feature catalog](#3-feature-catalog)
   - 3.1 [Multiplayer (flagship)](#31-multiplayer-flagship)
   - 3.2 [Gameplay & input tools](#32-gameplay--input-tools)
   - 3.3 [Overlay UI & developer tools](#33-overlay-ui--developer-tools)
   - 3.4 [Server, web dashboard & cloud](#34-server-web-dashboard--cloud)
4. [Client architecture](#4-client-architecture)
5. [Multiplayer client internals](#5-multiplayer-client-internals)
6. [Wire protocol](#6-wire-protocol)
7. [Server internals](#7-server-internals)
8. [Web dashboard internals](#8-web-dashboard-internals)
9. [Cloud deployment](#9-cloud-deployment)
10. [Configuration reference](#10-configuration-reference)
11. [Hotkey reference](#11-hotkey-reference)
12. [UI parity classification (detailed tables)](#12-ui-parity-classification-detailed-tables)
13. [PC port mapping](#13-pc-port-mapping)
14. [Implementation status matrix](#14-implementation-status-matrix)
15. [Testing & validation](#15-testing--validation)

---

## 1. What this mod is

The repository is four cooperating components plus tooling:

| Component | Path | Language | Role |
|---|---|---|---|
| Client mod | `switch-client-mod/` | C++ (devkitA64, GCC 15) | exlaunch `subsdk9` module injected into the game process; all hooks, gameplay changes, overlays, and the multiplayer client live here |
| Multiplayer server | `server/` | Python 3.13, stdlib only | lobby engine, transports (TCP + UDP discovery + WebSocket), avatars, admin port, web bridge |
| Web dashboard | `web/` | TypeScript / React / Vite | live spectator + host-control dashboard served by the server |
| Cloudflare worker | `cloudflare/` | JS | bridges plain-HTTP `/switch` WebSockets from consoles to the HTTPS-only Render origin |
| Tools/tests | `server/test_*.py`, `server/bot.py`, `server/sim.py`, `server/fake_client.py`, `server/admin.py` | Python | unit tests, protocol bots, admin CLI |

### Target (Switch)

- Game: **Hatsune Miku Project DIVA Mega Mix**, English release.
- Title ID `01001CC00FA1A000`, update **1.0.6** (`v393216`), main build ID
  `179265bd41f7781dac979e77702011241b442ba1`.
- Every `InstallAtOffset` / `base + 0x...` in `switch-client-mod/source/program`
  is a **main-module virtual offset for that exact build**. None of the numeric
  offsets carry to PC; the *semantic hook points* (which game function, and why)
  are the portable part.

### Heritage

The client mod began as a fork of lsmsmx's exlaunch port for MegaMix (a
DivaModLoader-style environment for Switch). It inherits DML-style features
(mod_ folders, raised limits, external save data), graphics restoration and
ported MM+ mods. Those inherited features, New Classics, and the debug/freecam
tooling are out of scope for the PC port and are not cataloged here. On top
of them this repository adds the **entire multiplayer
stack** (client, protocol, server, dashboard, cloud). For a PC port, the
multiplayer stack is the main novel work: most inherited features either already
exist on PC natively or exist as the original MM+ mods this code was ported
*from* — see §13.

---

## 2. The porting rule: restyle vs. parity

Every user-visible element in the mod falls into one of three classes:

| Class | Rule for PC port |
|---|---|
| **RESTYLE** — mimics *Nintendo Switch system menus* or Switch hardware affordances | Redesign for PC. Keyboard/mouse-first navigation, PC focus/hover conventions, real text fields, responsive sizing, PC controller/keyboard glyphs. Do **not** copy the look. |
| **PARITY** — designed to integrate with / look native to *Project DIVA itself* | Reproduce **exactly**. Same shapes, colors, animation curves, timing, placement, and behavior on PC as on Switch. These are shared multiplayer UX and must be indistinguishable between platforms. |
| **NEUTRAL** — developer/debug chrome (plain ImGui windows) | Keep equivalent functionality; styling is free. A plain ImGui/debug window is acceptable on PC. |

§12 contains the element-by-element tables. The one-line summary:

- **Restyle:** the multiplayer lobby overlay (all its screens), the on-screen
  virtual keyboard, every Switch button-glyph guide (A/B/X/Y/+/ZL/ZR), the
  Joy-Con-grip input display.
- **Parity:** hexagonal portraits, in-game leaderboard, bottom race gauge with
  avatar markers, rank placement badge + rank-change effects, countdown/GO!,
  waiting/banner overlays, results hexagon clusters, AUTO pills, the MP badge on
  song select.
- **Neutral:** all debug/developer ImGui windows (memory scanner, res scaler,
  post-process, FPS, dim, volume, state switcher, save backup, motion player,
  wind control, quick jump, extra menu, plugin help, debug window).

---

## 3. Feature catalog

Legend: **[MP]** multiplayer feature · **[UI]** has
user-visible UI (see §12 for classification) · **[CFG]** has config.toml keys.

### 3.1 Multiplayer (flagship)

Client-side implementation lives in `switch-client-mod/source/program/mp/`;
protocol/lobby engine in `server/`; spectator/host UI in `web/`.

**Connectivity**

- Server modes **[UI][CFG]**: `OFF` / `LOCAL` / `ONLINE` selectable on the MP
  overlay's OPTIONS screen (`server_mode`). OFF opens no socket and makes no
  request. LOCAL = LAN. ONLINE = cloud WebSocket.
- LAN discovery: UDP probe on discovery port (default 7778) sent to broadcast,
  the subnet broadcast, `127.0.0.1`, configured `server_ip`, every remembered
  server, and a unicast sweep of the local subnet (up to a /22 via
  `nn::nifm::GetCurrentIpConfigInfo`). Every server the console successfully
  connects to is remembered in `sdmc:/MikuMikuSwitchPlugin/mp_servers.txt`
  (max 4, newest first) and probed on later boots.
- Transports: raw non-blocking TCP (LAN, port 7777) or RFC 6455 WebSocket over
  plain TCP (cloud, `ws://<cloud_url>/switch`). Identical NDJSON message
  semantics on both.
- Session: `hello {proto:1, mod, game, cid, name}` → `welcome`. Server warns on
  mod-version mismatch (`--mod-version-expected`). `cid` is a stable 16-hex
  client ID auto-generated into config. Reconnect is automatic; a cid the server
  already knows logs `REJOINED` and can reclaim its seat (ownership restore
  within 120 s).
- Keepalive: client pings every 2 s; watchdog + reconnect on silence.

**Rooms & roles**

- Create/join/leave rooms with 4-letter room codes (alphabet
  `ABCDEFGHJKLMNPQRSTUVWXYZ`). Max **8 members**.
- Two distinct roles tracked separately:
  - **Owner** — room creator. Can kick members, change pick mode, assign the
    picker, pause the whole group instantly. Hands off in join order on leave;
    rejoining within 120 s restores ownership.
  - **Leader** — the member currently picking the song. Rotates per round or is
    fixed/assigned depending on pick mode.
- Kick: owner-only (`kick {cid}`), two-step pad confirmation in the UI.
- Pick modes (`pick_mode`): `rotate` (leader cycles each round), `owner` (owner
  always picks), `assigned` (owner designates a picker). A pending change is
  applied at `round_end`.

**Song-select synchronization**

- Leader browses normally; every cursor move sends `sel {pv, name}`.
- Followers mirror the leader's selection: the client jumps its own song-select
  cursor to the same PV.
- Availability is checked against each console's **own sorted PV lists**
  (sort mode × difficulty × edition), *not* pv_db — pv_db includes DLC a console
  may not own. Each follower replies `sel_ack {pv, has, ok, fb_pv, fb_name}`;
  the server aggregates into `sel_status {missing, cant}`.
- UI: the picker sees a "not available for …" pill; followers see a
  "<name> is picking a song (N)" line on the MP badge. **[UI→PARITY]**

**Launch choreography**

- `start_req` → server broadcasts `launch {round, pv, players, spectators}`.
- Followers auto-navigate song select and press confirm; a follower still in
  the module/customization picker is allowed to finish first.
- A member not in song select at launch becomes a **spectator** (waits in song
  select, sees spectate banners, rejoins next round).
- Load gate: each console loads the PV, waits paused; `loaded` → server sends
  `countdown {secs}` (default 3, `--countdown`, 0 disables) → `start` → all
  consoles unpause simultaneously.

**In-song**

- Each console streams `state {round, score, combo, pct, life}` telemetry;
  the server rebroadcasts `board` at ≥100 ms intervals.
- Live leaderboard **[UI→PARITY]**: per-player score/%/life/rank, connected
  flag, local-player marker; hidden while a spectator is in song select.
- Bottom **race gauge** **[UI→PARITY]**: per-player fill along the bottom edge
  plus a hexagonal avatar marker above each player's percentage pointer, with
  collision spreading and connector stems.
- **Rank placement badge** **[UI→PARITY]**: top-right ordinal badge with
  rank-specific gradients (see §12), slide-in + elastic pop entry, horizontal
  3D flip, bounce/shake on rank change, shockwave ring, four-point sparkles,
  rising/falling double chevrons; fades out when a note target approaches the
  top-right corner so it never obstructs gameplay.
- **Rank-change glow** animation on the leaderboard row. **[UI→PARITY]**
- Player-event notifications **[UI→PARITY]**: joined / left / disconnected /
  reconnected / combo milestones / low life / drops. `notify_style` =
  `avatars` (portrait popups) | `text` (banner text) | `off`. **[CFG][UI]**
- Failing out early (life 0): the song ends for that player; their `final` is
  submitted and they wait for standings (early-drop notification is a known
  gap — §14).
- **Countdown / "GO!"** overlay **[UI→PARITY]**: dimmed band, turquoise rules,
  stylized glossy outlined digits, animated countdown then GO!.

**Group pause**

- Pressing pause in a lobby round sends `pause_req`. **Owner** pauses everyone
  instantly; other members trigger a **vote** — every connected player must
  agree within 15 s. Server broadcasts pause/resume.
- While group-paused, pause-menu navigation is blocked except the emergency
  exit; a reconciler keeps the game paused if it drifts. **[UI→PARITY]**
- In rooms with ≥2 members, **Retry** (and Lag Config/Options in the pause
  menu) is swallowed by input blocking; a retry that slips through is detected
  (song clock running after results with no pv_sel reinit) and undone via
  pause → "Return to Song List". Solo rooms behave vanilla.

**Results & round lifecycle**

- Each console sends `final {round, score, pct, max_combo, jc:[5], life}`.
- Server broadcasts `standings` (partial then complete) — **competition
  ranking**: tied rows share the *lowest* position of the tied group
  (1-3-3-4). **[UI→PARITY]**
- Results screen shows a **hexagon cluster** of all players' avatars; tied
  players render as a single connected hexagon-sized group with stacked names.
  **[UI→PARITY]**
- Auto-return: a blind menu model drives results → song list automatically
  when the leader exits or a new round launches (recipe: A, A, Down, A — see
  §5 menu model).
- Round end: leader/owner `back_in_sel` in RESULTS ends the round →
  `round_end {next_leader}` handles rotation/handoff.

**Avatars**

- Each console announces `avatar {module, chara, mh}` — the module equipped in
  its Shared Set A slot 1. `mh` is an FNV-1a signature over
  (chara, cos, sort_index) — language-independent, stable across consoles with
  different mod orders.
- Portrait pipeline: resolve the module's selection card sprite
  (`SPR_SEL_MD%03dCMN`), decode BC5/YCbCr, head-crop, normalize to 96×96 RGBA.
- Custom/modded modules can upload portraits (`avatar_img`, base64) to the
  server; server sanitizes (own-module only, square, 32–96 px, not blank/flat,
  alpha + RGB-under-transparent normalization), normalizes to 96×96 PNG, serves
  them at `/api/avatar/<id>.png`, and can relay them to other consoles.
- Consoles opt in to receiving remote portraits with `remote_avatars`
  ("Custom portraits" on OPTIONS) **[CFG][UI]**; fetch is `avatar_get` →
  `avatar_px` (96×96 RGBA, ≤6 cached, 3 retries, never written to SD).
- Fallbacks: same-signature local image → (opt-in) owner's upload → chara's
  official default module → silhouette. Big-head modules (e.g. Mikudayo) blend
  to a wider face-centered crop.
- Moderation: `avatar_hide` removes a portrait from snapshots/relay until
  server restart (admin CLI + web "Hide portrait").

**AutoPlay in multiplayer**

- AutoPlay is allowed in lobbies but every console reports
  `autoplay {on, pct, mode}`; the AUTO tag shows on the board, standings, and
  web UI.
- Partial/assisted modes are server-controllable (`admin.py autoplay on|off
  [--pct N]`); equal `--pct` on two consoles produces deterministic ties.
- ZR ×3 in song select cycles bot modes locally.

**Emergency exit**

- Hold **Minus for 3 s** anywhere in a lobby → leaves the lobby and releases
  all input manipulation immediately; a filling-circle indicator shows the
  hold progress. **[UI→PARITY]** Minus is never blocked by the input system.

**Lobby overlay UI** — see §12. All screens (Home / Edit Name / Join Room /
Room / Room Config / Options / Online Server editor) are **RESTYLE** (Switch
system-menu styling), while every element that appears *inside gameplay or the
game's own menus* is **PARITY**.

**Client admin/debug surface**: `server/admin.py` drives `dbg_cmd` messages —
input injection (`tap`/`spam`/`block`), song jumps (`jump`), autopause,
overlay open/close, cursor move/click (720p coords), menu cursor forcing,
`sel_lists`, `module_dump`, `remote_avatars`, `avatar_hide`, `autoplay`,
`lobby_create/join/leave`, `menu_holds`, `glow`, `notify_style`, `popup_test`,
`server_mode`, `cloud_url`, avatar peek/poke/dump.

### 3.2 Gameplay & input tools

- **AutoPlay** (`AutoPlay.cpp`) **[UI→PARITY for the in-game indicator]**:
  hands-free note hitting with bot "skill" modes — `custom` (pct), `auto`,
  `pro`, `97`, `streak`, `human`, `clutch`, `tie` — each with its own
  hit-rate/judgment-degradation behavior (deliberately late presses to land
  Fine/Safe/Sad). Deterministic per-note outcome for a given pct. ZR ×3 in
  song select cycles modes; indicator pill + AUTO tags in HUD/MP UI.
  Partial-autoplay (`--pct`) is the MP server control surface.
- **ChartRandomizer** **[CFG-less; UI→PARITY hint]**: Off / Partial / Full
  chart shuffling with per-note swap chance; cycled from song select;
  on-screen badge when active.
- **Challenge Time** (`challenge_time = default|enabled|disabled`) — vixen's
  Challenge Time port: per-difficulty challenge mode.
- **TouchSlider**: touchscreen → virtual slide zones (left/right halves act
  as slide sticks with deadzone tracking). `enable_touch` gates touch input.
- **USB keyboard support** (`keyboard.cpp`, `keyboard_sliders.cpp`):
  `enable_keyboard` maps physical keyboard keys to game buttons and analog
  sliders; also feeds ImGui text input.
- **ExPatch**: unlocks Extreme difficulty; **skipAdv**: skips the advertise/
  intro flow; **remove_watermarks**, **disable_hand_scaling**,
  **disable_lyrics**, **force_japanese** (language force),
  **force_ft_ui**/`ft_ui` (FT-styled UI surfaces where implemented).
- **Hints** (`overlays/Hints.cpp`) **[UI→PARITY w/ RESTYLE glyphs]**: in-game
  hint line rendered in the game's own hint-bar style — e.g. "Plus: plugin help", 2D/NO-MOVIE/RANDOMIZER status badges, MP state pills.
  Uses Switch button glyph pills that must become PC-appropriate glyphs.
### 3.3 Overlay UI & developer tools

All of these are ImGui-rendered on top of the frame via the NVN present hook.
Unless noted they are **NEUTRAL** (debug chrome — keep functionality, free
restyle on PC).

- **MP overlay** (`mp/MpUi.cpp` via `MpOverlay`/`DrawPanel`) — **RESTYLE**;
  the full lobby UI. Detailed in §12.
- **VirtualKeyboard** — **RESTYLE**; on-screen keyboard for name/room
  code/server URL entry, with pad-navigation mode. On PC use native text
  fields.
- **InputOverlay** — **RESTYLE visuals, PARITY function**: bottom-center
  translucent input display for recordings; modes Gamepad (Switch grip
  look — replace with PC/arcade controller display) / Keyboard / hidden;
  also projects touch-screen swipes into a mini zone. Cycle: pure L3+R3
  hold ≥0.8 s released.
- **MemoryScannerUi** — BSS-gap scanner: lists free BSS regions (svc memory
  query), sizes, totals. L3+R3+ZL.
- **ResScalerUI** — live resolution scaler slider. L3+R3+ZR (opens with
  PostProcessUI).
- **PostProcessUI** — live tone-map/exposure/gamma/saturation/PSE sliders.
- **FpsOverlay** — corner FPS readout, Miku-teal/yellow/red thresholds.
  L3+R3+Plus.
- **DimOverlay** — vertical PV-dimming slider (writes `dim`). L3+R3+L.
- **VolumeOverlay** — three channel cards (BGM/BTN/SE) tier boost control
  (writes `bgmBoost`/`btnBoost`/`seBoost`). L3+R3+R.
- **MotionPlayerUi** — bone control + motion recorder/player ("Ultimate
  Player"): per-bone rotate/translate sliders, record arm/frames, save/load
  motion dumps to `DMLSwitchPort/Dump/*.txt`, bone index reference.
- **WindControl** — wind spectrum override + custom vectors + file presets.
- **StateSwitcher** — scene/substate jump table (all game states listed).
- **SaveBackup** — save backup/restore/delete between `ExlSD:/Save`,
  `ExlSD:/MikuMikuSwitchPlugin/Save`, and `.../Backups` with timestamped
  folders and confirm dialogs.
- **SongQuickJump** — song-select quick-jump/search using the sorted PV
  lists; also the canonical API for "which PVs are selectable"
  (`PvSelectableAt`) used by MP availability checks.
- **PluginHelpUi** — 780×510 two-page help dialog on the main menu
  (DivaPink/Cyan styling) — custom mod chrome; on PC, fold into whatever
  mod-settings UI replaces the MP overlay.
- **ExtraMenu** — toggle grid inside the debug window for the above windows.
- **ImGuiOverlay** (`ImGuiOverlay.cpp`) — the overlay manager itself: NVN
  present hook → ImGui frame; hotkey handling; gamepad→mouse emulation for
  ImGui (stick = cursor, ZL/ZR = click, ZL+ZR 0.5 s = focus toggle);
  focus/busy arbitration so overlays can capture input; draw order
  (debug windows → sub-overlays → MP panel → MP gameplay HUD → keyboard →
  render).

### 3.4 Server, web dashboard & cloud

- **`mp_server.py`** — asyncio server; transports + sessions + lobby manager
  + avatar store + admin port + web bridge; UDP discovery responder; client
  table; `dbg_*` telemetry logging; `dbg_cmd` dispatch. Ports: TCP 7777,
  UDP 7778, admin 127.0.0.1:7790, web 8080 (single-port in `--cloud`).
- **`lobby.py`** — pure, asyncio-free lobby/round engine (testable; outbox
  pattern). Phases `SELECT/LOADING/PLAYING/RESULTS`; timeouts: member removal
  after 60 s DC in SELECT, 10 s leader handoff, 30 s leader-loaded start, 90 s
  no-leader-load, 90 s all-back round end, 15 s pause vote, 60 s
  all-disconnected abort; board ≥100 ms; 120 s owner-rejoin restore.
- **`avatars.py`** — upload gate (`UploadGate`: own module only, rate limits,
  dimension/blank/alpha checks, normalization to 96×96, stdlib-zlib PNG
  encoding, content-derived IDs) + `RelayGate` for console-to-console relay.
- **`admin.py`** — admin/debug CLI (see §3.1 client admin surface).
- **`netcheck.py`** — startup LAN address listing + Windows Firewall rule
  warning.
- **Web dashboard** (`web/`) — live lobby overview, per-lobby live board,
  results cards with judgment counts and superlatives, activity feed, member
  list with hex avatars and connection state, host controls (pick mode,
  assign/make picker, kick, hide portrait), name-claim login + admin token
  (`MP_WEB_TOKEN` / `?token=`). Browsers see opaque HMAC `mid` tokens, never
  raw cids/IPs.
- **Cloud** (`render.yaml` + `cloudflare/worker.js`) — single-port cloud mode:
  `/` dashboard, `/health`, `/ws` browsers, `/switch` consoles; Cloudflare
  Worker bridges console plain-HTTP WS → Render HTTPS origin (the Switch
  client has no TLS). 65 s connect wait for free-tier cold start.
- **Bots/tests** — `bot.py` (interactive protocol bot), `sim.py` (multi-bot
  scripted rounds: leader + followers + missing/straggler), `fake_client.py`
  (minimal protocol client), `test_lobby.py` / `test_web_bridge.py` /
  `test_avatars.py` / `test_netcheck.py`.

---

## 4. Client architecture

### Runtime model

- **Framework**: exlaunch — the mod builds to `subsdk9` (an ELF) loaded by
  Atmosphere (or Ryujinx's mod loader) into the game process alongside the
  main NSO. `deploy/main.npdm` accompanies it. Build:
  `cd switch-client-mod && ./exlaunch.sh build`; deploy
  `deploy-ryu` (Ryujinx) / `deploy-ftp` (console SD `exefs/`).
- **Hooking**: exlaunch `HOOK_DEFINE_TRAMPOLINE` +
  `InstallAtOffset(<main-module vaddr>)` and `exl::patch::CodePatcher` for
  inline ARM64 instruction patches. `FUNCTION_PTR` declares typed game
  functions. All offsets target main 1.0.6.
- **Init order** (MP-relevant, `main.cpp`): `Config::init` → MP socket
  guards → `mp::Init` (if enabled) → `nvn_hooks::InstallHooks` →
  `ImGui::Init` → selector/results/PV-game hooks (on Switch these live in the
  New Classics hook files, or `mp::scenehooks::Install` when those are off) →
  keyboard; the `nnMain` hook then runs `mp::moduledb::Init` and
  `AutoPlay::init`.
- **Rendering**: `nvn_hooks` wraps `nvnQueuePresentTexture` (resolved via
  `nvnBootstrapLoader`/proc-address hook) → each frame runs
  `nvnImguiCalc()` → all overlays draw into ImGui → NVN-backed ImGui backend
  renders the draw data over the game frame.
- **Threads**: dedicated MP net thread (low `nn::os` priority; 0–31, 31 lowest);
  game thread(s) call hooked selectors/PV task/StageResult; ImGui frame runs on
  the render thread. `thread_local` is unavailable (`__aarch64_read_tp`
  missing) — all thread affinity is explicit.
- **Memory discipline**: fixed-size ring buffers and hand-rolled sync for
  game↔net handoff — no malloc on game threads in the MP path (newlib
  allocator races caused crashes). Large payloads (avatars) use a separate
  64 KiB blob channel, not the ~512 B line queue.

### Source map (`switch-client-mod/source/program/`)

| File(s) | Purpose |
|---|---|
| `main.cpp`, `lib.hpp`, `macros.hpp`, `offsets.hpp`, `Types.hpp`, `patches.hpp`, `setting.hpp`, `version.hpp` | entry, exlaunch glue, shared constants/structs |
| `Config.cpp/.hpp`, `toml.hpp` | TOML config read/write (`MikuMikuSwitchPlugin/config.toml`), all config keys |
| `logger.cpp/.hpp`, `loggers.hpp` | `hook_log.txt` logging (unflushed on Ryujinx) |
| `AutoPlay`, `ChartRandomizer`, `TouchSlider`, `keyboard`, `keyboard_sliders`, `hid.hpp` | §3.2 input/gameplay |
| `nvn_hooks`, `ImGui.hpp`, `ImGuiOverlay.cpp`, `Allocator.hpp` | render/overlay infrastructure |
| `overlays/` | §3.3 ImGui windows |
| `mp/` | §5 multiplayer client |

Flat object dir + case-insensitive filesystem ⇒ **all filenames must be
globally unique** (hence the `Mp`/`Nc` prefixes).

---

## 5. Multiplayer client internals

`source/program/mp/` — the client is deliberately split so the port can lift
the platform-independent parts wholesale.

| File | Role |
|---|---|
| `MpApi` | Dynamic `nn::socket`/`nn::nifm` binding + **socket guards** that block the game itself from reinitializing/tearing down the network service the mod owns |
| `MpNet` | Net thread: network bring-up, discovery/connection state machine, nonblocking TCP, WebSocket client (cloud), NDJSON frame I/O, ping/watchdog/reconnect, fixed outbound ring (128×~512 B) + 64 KiB avatar blob path, `dbg_cmd` dispatch |
| `MpJson` | Minimal JSON parse/emit helpers (no heap churn) |
| `MpLobby` | Game-thread consumer of inbound lines; **phase machine** `NONE/SELECT/LAUNCHING/LOADING/PLAYING/SPECTATING/DROPPED/RESULTS`; snapshot (`Snapshot`) consumed by all UI; telemetry emission (`state`/`final`/`scene`/`sel`/`back_in_sel`/ready reasons); hooks on selector init/destroy/frame, PV-game frame, StageResult init/destroy |
| `MpSceneHooks` | MP-only selector/results hooks (used on Switch when the New Classics hooks are not installed) |
| `MpInput` | HID-level injection/blocking via `GetNpadState` hooks across all Npad styles; merged poll dedupe (edge detection must combine sources); Minus never blocked; ImGui pad polling bypass handling |
| `MpMenu` | **Blind menu model** — the game exposes no cursor signal for pause/results menus, so this counts the presses the game actually receives (post-block/inject, debounced) and predicts page/cursor; anchors: StageResult init/dest, `win_vp_in` AET layer, `pv->paused` edges, pv_sel init; frame-counted hold timings (`menu_holds` tunable): init ≈96, score-page A ≈66, VP ≈120, menu ≈51, nav ≈21 frames |
| `MpModuleDb` | Captures the game's merged module table (`gm_module_tbl` parser hook); per-entry FNV-1a signature (chara/cos/sort_index) for cross-console module identity; drives avatar announce + availability |
| `MpAvatar` | Portrait decode (spr set → BC5/YCbCr → head crop → RGBA), local cache (LRU 10), remote fetch path, silhouette fallback, big-head crop rule |
| `MpOverlay`/`MpUi` | All MP drawing: the RESTYLE overlay screens + every PARITY in-game element (§12) |

Key data-flow: game thread only *enqueues* into fixed rings; net thread owns
sockets; inbound lines queue to game thread; `Snapshot` is the single read
model for UI. This separation is what a PC port should keep — the transport and
HUD can be replaced independently.

---

## 6. Wire protocol

Protocol **v1**, NDJSON (one object per line, `"t"` = type) — identical over
LAN TCP and cloud WebSocket. `server/lobby.py`'s docstring is canonical.

**Client → Server**

| Msg | Fields | When |
|---|---|---|
| `hello` | `proto, mod, game, cid, name` | connect |
| `ping` | — | 2 s keepalive |
| `set_name` | `name` | rename |
| `lobby_create` / `lobby_join` | `{}` / `code` | rooms |
| `lobby_leave` | — | leave |
| `kick` | `cid` | owner only |
| `sel` | `pv, name` | leader browsing, SELECT phase |
| `sel_ack` | `pv, has, ok, fb_pv, fb_name` | follower availability reply |
| `start_req` | `pv, name` | leader, SELECT |
| `launch_fail` | `round` | follower couldn't launch |
| `loaded` | `round` | load gate |
| `state` | `round, score, combo, pct, life` | live telemetry |
| `final` | `round, score, pct, max_combo, jc[5], life` | song end |
| `back_in_sel` | `round` | member back in song select |
| `scene` | `where: sel\|results\|game\|other` | scene changes |
| `avatar` | `module, chara, mh` | module identity, any time |
| `avatar_get` | `img` | fetch remote portrait (opt-in) |
| `avatar_img` | `module, w, h, rgba` | portrait upload (base64) |
| `autoplay` | `on, pct, mode` | AutoPlay state |
| `ready` | `ready, reason, pv` | readiness signal (e.g. still in module picker) |
| `set_pick_mode` | `mode, cid` | owner: `owner\|assigned\|rotate` + assignee |
| `pause_req` / `resume_req` | `kind` | group pause request / resume vote |
| `kick` | `cid` | owner only (also listed above) |
| `dbg_*` / `dbg_cmd` replies | — | telemetry + admin |

**Server → Client**

| Msg | Fields |
|---|---|
| `welcome` / `error` / `pong` | session |
| `lobby` | `code, phase, leader, you, round, sel_pv, members[]` (per-member: `cid,name,connected,role,loaded,module,chara,mh,img`) |
| `lobby_left` | `reason: left\|kicked\|closed` |
| `lobby_error` | `code, msg` |
| `sel` | `pv, name` (leader's current selection) |
| `sel_status` | `pv, missing[], cant[]` |
| `launch` | `round, pv, name, players[], spectators[]` |
| `countdown` | `round, secs` |
| `start` | `round, dropped[]` |
| `board` | `round, e[]:{cid,name,score,pct,life,rank,connected}` |
| `event` | `kind, cid, name, value` |
| `standings` | `round, complete, e[]:{cid,name,score,pct,max_combo,life,rank,final}` — ranks: ties share lowest position |
| `round_end` | `round, next_leader` |
| `pause` / `resume` | `by: cid` (`sync` flag on reconciler sync) |
| `pause_vote` | `kind, agreed[], need, ends_at` |
| `avatar_px` | `img, rgba` or `img, err` |

**Discovery**: UDP JSON `{"t":"discover","proto":1}` → `{"t":"server","port",…}`.
**Admin port** (7790): line commands mirroring `dbg_cmd` (tap/jump/autoplay/…).

The protocol is **platform-independent** — keep it verbatim on PC; only the
client transport adapter changes.

---

## 7. Server internals

`server/mp_server.py` (asyncio, stdlib): `Client` sessions (TCP or WS),
`Server` orchestrating `LobbyManager`, `AvatarStore`/`UploadGate`/`RelayGate`,
`WebBridge`, admin line server, `DiscoveryProtocol` UDP responder, `netcheck`
firewall warning. `Server.table()` feeds the web snapshot. `--cloud` folds
everything onto one port.

`server/lobby.py`: pure engine — `Lobby`/`Round`/`Member`, phase transitions,
rank computation, pick modes, votes, reconnect/restore timers, all decisions
expressed as outbox messages (unit-testable; `test_lobby.py` is the spec).

`server/web_bridge.py`: stdlib HTTP+WS — serves `web/dist`, `/api/state`,
`/api/avatar/<id>.png`, browser `/ws` (backlog + periodic snapshots), console
`/switch` (NDJSON passthrough, large-frame support for avatar uploads),
activity feed, auth (name claim + `MP_WEB_TOKEN`), host actions, HMAC `mid`
privacy tokens.

`server/avatars.py`: sanitize/normalize/encode pipeline + gates.

Everything here is **directly reusable on PC** — the server knows nothing
about the platform.

---

## 8. Web dashboard internals

React SPA (`web/src`): `App.tsx` (hash routing `#/lobby/<CODE>`), `state.ts`
(reducer + snapshot store mirroring `web_bridge` schema — see `types.ts`:
`Phase`, `PickMode`, `MemberSnap`, `RoundSnap`, `StandingRow`, `VoteInfo`,
`ConsoleEntry`, `ServerInfo`), `socket.ts` (WS client: hello/feed/snapshot/
auth/action), components: `Overview`, `LobbyView` (live board w/ FLIP row
transitions, results cards + count-up animations, judgment counts, superlatives,
activity feed, phase panel, member list, host controls), `HexAvatar`
(same hexagon visual language as in-game), `LoginDialog`, toasts.

Platform-independent — no port needed beyond pointing it at the same server.

---

## 9. Cloud deployment

`render.yaml` Blueprint → `python server/mp_server.py --cloud` on Render
(free tier). One public port: `/` dashboard, `/health`, `/ws` browsers,
`/switch` consoles. `cloudflare/worker.js` on route
`mikumod.evanthomas.dev/switch*` re-issues the console's plain-HTTP upgrade to
`https://mikumod.onrender.com/switch` and pipes the WS (must target the
onrender host — subrequest to own hostname 404s). Consoles wait ≤65 s for the
cold-start upgrade; 2 s client pings keep the free tier awake. Env:
`MP_WEB_TOKEN`, `MP_SERVER_NAME`, `MP_PUBLIC_URL`.

On PC the same path works, but a TLS-capable client could skip the Worker.

---

## 10. Configuration reference

`sdmc:/MikuMikuSwitchPlugin/config.toml` (regenerated with all keys by
`Config.cpp`).

**Top level**: `mods`, `debug`, `imgui` — master toggles.

**[features-ish section]**: `new_classics`, `ExPatch`, `challenge_time`
(`default|enabled|disabled`), `skipAdv`, `remove_watermarks`,
`disable_hand_scaling`, `disable_lyrics`, `force_japanese`, `force_ft_ui`,
`enable_touch`, `enable_keyboard`, `bgmBoost`, `btnBoost`, `seBoost`,
`acapellaMode`, `ft_ui`.

(Graphics, New Classics, and mod-loader keys also exist in this file but are out of scope for the port.)

**[multiplayer]**: `enabled` (default true), `server_mode`
(`off|local|cloud`, default `local`), `cloud_url`
(default `mikumod.evanthomas.dev`), `server_ip`, `server_port` (7777),
`discovery_port` (7778), `player_name`, `debug_status_chip`, `notify_style`
(`avatars|text|off`, default avatars), `remote_avatars` (default false),
`client_id` (autogenerated 16-hex).

**Other state files**: `mp_servers.txt` (remembered LAN servers, max 4),
`MikuMikuSwitchPlugin/Save` + `Backups/`, `emu_states.txt` (state jump file).

---

## 11. Hotkey reference

| Combo | Action |
|---|---|
| Plus + Minus (hold 0.8 s) | main debug ImGui menu |
| L3+R3+ZL | memory scanner (BSS gaps) |
| L3+R3+ZR | resolution scaler + post-process |
| L3+R3+Plus | FPS overlay |
| L3+R3+L | dim overlay |
| L3+R3+R | volume overlay |
| L3+R3 pure hold ≥0.8 s, release | cycle input display: off → gamepad → (keyboard if `enable_keyboard`) → off |
| ZL+ZR hold 0.5 s | toggle ImGui/game focus ("mouth focus") |
| ZR ×3 (song select) | cycle AutoPlay bot mode |
| Minus hold 3 s (in lobby) | MP emergency exit |

PC port: replace with PC-idiomatic bindings (function keys, Ctrl/Shift
chords, or an ImGui menu); keep MP emergency-exit semantics on a sensible key.

---

## 12. UI parity classification (detailed tables)

### 12.1 RESTYLE — Nintendo-styled (redesign for PC)

| Element | Current (Switch) behavior/style | PC port direction |
|---|---|---|
| **MP overlay shell** (`MpUi.cpp`) | "Switch-system-menu styled, touch/cursor driven": 84%-screen dimmed overlay, dark gray rounded cards, cyan focus ring `(0,195,227)`, header bar, fixed 720p scale | A desktop window/panel or full-screen PC menu; mouse hover + click, keyboard tab/focus, scroll; keep DIVA accent colors optional |
| **Home screen** | Three cards: Edit Name / Create Room / Join Room | PC lobby window with the same three actions as buttons/list |
| **Edit Name / Join Room / Online Server editor** | Card + virtual keyboard + pad focus | Native text input fields |
| **Room screen** | Member podium + hexagon list, owner controls, two-step pad confirm for kick/exit | Keep the *layout concept* + hexagons; PC list/table with buttons, dialogs for confirm |
| **Room Config / Options** | Rows of cards w/ gear buttons; Server OFF/LOCAL/ONLINE cards; toggles (notify style, custom portraits, status chip) | PC settings panel (list/dropdowns/checkboxes) |
| **Button guide footer** | A/B/X/Y/+ glyph pills ("A Select  B Back  X …  + Options") | PC convention: show keyboard/mouse hints (Enter/Esc/etc.) or controller-agnostic glyphs |
| **VirtualKeyboard** | On-screen QWERTY grid w/ caps/cursor row; pad-navigation mode | Unnecessary — real keyboard; keep only as accessibility option |
| **Status chip** (`debug_status_chip`) | Small Switch-styled status pill | Small debug/status label — free |
| **InputOverlay gamepad mode** | Joy-Con-grip-shaped button display | Generic controller / arcade-stick layout display (function parity) |

### 12.2 PARITY — DIVA-integrated (reproduce exactly)

| Element | Specification |
|---|---|
| **Hexagonal portraits** | Pointy-top hexagons; DIVA pink/orange frame accents; used in lobby room UI, board markers, popups, results clusters, web `HexAvatar` |
| **Live leaderboard** | In-song board: rows of name/score/%/life, connection + local-player markers, AUTO pill; hidden while spectator in song select; rank-change edge glow |
| **Race gauge** | Bottom-edge per-player fill bars + percentage pointer with mini hexagon avatar above each, collision spreading, connector stems |
| **Rank badge** | Top-right ordinal w/ rank gradients — 1st radiant gold→amber, 2nd ice-blue→cobalt, 3rd copper→crimson-bronze, 4th Miku-turquoise→ocean-teal, 5th–8th neon-violet→royal-purple; slide-in + elastic pop, horizontal 3D flip, bounce/shake on change, shockwave ring, 4-point sparkles, up/down double chevrons; **fades when a note target nears top-right** (never blocks gameplay) |
| **Countdown / GO!** | Dimmed horizontal band, turquoise rules, outlined/glossy animated digits then "GO!" |
| **Waiting overlay** | "Waiting for players" treatment while load-gated (before countdown) |
| **Banners/popups** | In-song event notifications — portrait popups (avatars style) or text banners; placement/behavior preserved |
| **Standings / results hexes** | Results screen cluster: one hexagon per player, podium ordering; **ties render as a connected cluster** with stacked names |
| **Song-select MP badge** | "Multiplayer" state pill + AUTO pill + "X is picking…" line integrated into the game's selector UI; replaces AutoPlay badge slot |
| **Hints bar** | Game-style hint line (Plus help / MP / AUTO / RANDOM badges) — keep layout/positioning, swap Switch glyphs for PC glyphs |
| **Emergency-exit ring** | Filling-circle hold indicator, Minus-3s semantics |

### 12.3 NEUTRAL — debug chrome (any reasonable PC equivalent)

Debug window, ExtraMenu, StateSwitcher, SaveBackup, MemoryScannerUi,
ResScalerUI, PostProcessUI, FpsOverlay, DimOverlay, VolumeOverlay,
MotionPlayerUi, WindControl, SongQuickJump window, PluginHelpUi (fold into
mod settings), MP debug/telemetry displays.

---

## 13. PC port mapping

| Switch subsystem | PC/Steam equivalent |
|---|---|
| exlaunch `subsdk9` + `main.npdm`, Atmosphere `exefs` | MM+ injection: **DivaModLoader plugin** (DLL) or equivalent; the same "module loaded into the game" model |
| All `InstallAtOffset` main-module vaddrs (build 1.0.6) | Re-derive for the MM+ executable; the *semantic* hook points carry over: `TaskPvGame*`, song-selector init/ctrl/disp/dest, `StageResult` init/dest, `gm_module_tbl` parser, `sorted_pv_lists`, `win_vp_in` layer, pause flag, AET/spr loaders, score/hit-state funcs |
| `nn::socket`, `nn::nifm` | Winsock2/`asio`; adapter enumeration via Win32 (`GetAdaptersAddresses`) for subnet sweep + a remembered-server file |
| `nn::hid` `GetNpadState` hook chain | Hook MM+'s input poll (same semantic layer the Switch code hooks) — injection/blocking/dedup logic ports directly |
| Touch screen (`GetTouchScreenState`) | Mouse or drop (PC has no touch); TouchSlider → mouse-drag zones or omit |
| `nn::pl` shared fonts | Bundled/system font for ImGui; keep the two-tier (base + big rank) atlas pattern |
| NVN `PresentTexture` hook + `nvnImgui` | MM+ already hosts ImGui-style mods (DivaImGUI); hook the game's present/end-of-frame equivalently |
| `nn::fs` / `ExlSD:` paths | Plain file I/O under a mod data dir |
| `nn::os` thread + fixed rings + manual sync | `std::thread` + the same ring design (still sound); malloc-safety rules can relax but keep the discipline |
| `nn::audio` WaveBuffer hooks | Hook the PC audio path for volume boosts/acapella |
| Save backup SD paths | Back up the MM+ save location |
| Button glyph pills (A/B/+/ZL/ZR) | PC key/controller-agnostic icons |
| Virtual keyboard | Native text fields |
| Cloud WS via Cloudflare → Render | Reuse as-is; PC could also go TLS direct to Render |

**Server, protocol, web, Cloudflare worker, lobby engine: no changes needed.**
The entire MP UX contract (board cadence, phases, roles, ranking, votes,
choreography) is platform-agnostic — the port is essentially "same
`MpLobby`-level state machine + same HUD over a new runtime adapter".

### Suggested port order

1. Reuse `server/` + `web/` unchanged; verify with `fake_client.py`/`sim.py`.
2. Build the PC injection + hook layer; re-derive semantic hook points.
3. Port `MpNet` (sockets/WS/discovery) and `MpLobby`/`MpInput`/`MpMenu` logic —
   these files are written to be lifted; swap `nn::*` calls only.
4. Port the PARITY HUD exactly (§12.2).
5. Build the RESTYLE lobby UI natively for PC (§12.1) — keyboard/mouse first.
6. Port AutoPlay/MP integration, avatars (needs a PC-side way to decode the
   module card texture — the spr/texture formats are shared conceptually but
   the decode path is platform work).
7. Bring over whichever §3.2 gameplay tools are wanted.

### Landmines

- `thread_local` was unusable on Switch — fine on PC, but the code avoids it;
  keep explicit affinity anyway.
- Edge-triggered input must merge all poll sources (the Switch code debounces
  across Npad styles — on PC, merge keyboard+gamepad+injected sources).
- Menu navigation is **blind** (no cursor signal) — the frame-counted hold
  model must be recalibrated to MM+'s menus; keep the swallow/retry-recovery
  semantics.
- Module identity via `mh` (FNV-1a chara/cos/sort_index) assumes the same
  merged module table exists on PC — verify the PC `gm_module_tbl` shape.
- Avatar decode is format-specific (spr set → BC5→RGB); PC textures differ —
  budget real work here.
- Availability checks use the console's own sorted lists, not pv_db — keep
  that semantics or song sync breaks for DLC mismatches.

---

## 14. Implementation status matrix

| Area | Status |
|---|---|
| LAN discovery/connect/reconnect, remembered servers | Implemented; verified on Ryujinx (multi-instance) and real Switch incl. routed /23 subnet discovery |
| Rooms, roles, pick modes, kick, ownership handoff/restore | Implemented + unit-tested (`test_lobby.py`) |
| Song mirroring + availability | Implemented; verified multi-console |
| Launch gate, countdown, synced start | Implemented; verified |
| Live board, rank badge, race gauge, popups, glow | Implemented; visual verified |
| Group pause + vote, retry swallow + recovery | Implemented |
| Results standings + hex clusters + ties + auto-return | Implemented |
| Spectating | Implemented (member off song select at launch; missing-song path) |
| Emergency exit | Implemented; feedback polish pending |
| Remote avatars/upload/relay/moderation | Implemented + `test_avatars.py` |
| Partial AutoPlay / server control | Implemented |
| Cloud mode (Render + CF worker) | Implemented; consoles verified connecting |
| Web dashboard + host controls | Implemented |
| Accounts (`ACCOUNTS_DESIGN.md`) | **Design only** — interim name-claim auth is insecure by design; do not treat as final |
| Known gaps/bugs | B-on-song-select can trigger a stray MP round (state-10 detect vs confirm); per-song clear threshold/final-clear reporting missing; early-drop notification missing; session rankings specced not implemented — see `KNOWN_BUGS.md` |
| Unverified-at-scale | Full hardware multi-round session (Ryujinx verified; 6-console and 4-console rounds exercised but not universal hardware QA), straggler drop, auto-leave — see `FUTURE_TODO.md` |

---

## 15. Testing & validation

- **Server unit tests** (the protocol spec executable):
  `cd server && python -B -m unittest test_lobby.py test_web_bridge.py test_avatars.py test_netcheck.py -v`
- **Web tests**: `npm --prefix web test`.
- **Protocol bots**: `server/sim.py` (scripted multi-bot rounds: leader +
  followers + missing/straggler), `bot.py` (interactive), `fake_client.py`
  (discovery + minimal client).
- **Client-side debugging**: `server/admin.py` drives every client surface
  (taps, jumps, autoplay %, overlay/cursor, menu model, avatars, notify style)
  — port an equivalent `dbg_cmd` surface early; it is how the Switch build was
  tested end-to-end.
- **Multi-instance emulator runs**: `mm-re/tools/ryu_multi.py` spins N isolated
  Ryujinx instances each with own config/save/cid — on PC, use N game
  instances with per-instance config/client_id instead.
- **Build sanity (Switch only)**: after `./exlaunch.sh build`, verify no
  unresolved MP/game symbols:
  `aarch64-none-elf-nm -C switch-client-mod.elf | grep ' U ' | grep -E 'Config::|mp::|FontLoader::|AutoPlay::|Hints::'` → must be empty.
- **Acceptance for the port**: two PC instances + one bot complete a full
  round — mirror, launch, synced start, live board, early-fail, standings,
  auto-return, leader rotation — with the web dashboard live, matching the
  Switch behavior in §3.1.
