# Threat model: PD-MegaMix-Multiplayer-Mod

It comes from reading the code, `README.md`, `AGENTS.md` and `docs/ACCOUNTS_DESIGN.md`, plus the owner's answers (2026-11-16), recorded under "User-provided facts". Facts marked *(inferred)* are a reading of the code and do not change any finding's priority on their own.

## System overview
This is a multiplayer mod for Hatsune Miku Project DIVA Mega Mix on Nintendo Switch/Ryujinx. Consoles join lobbies on a Python server, play the same song together and stream their scores, which a web dashboard shows live. The same server code runs in two ways:
- **LAN / PC mode** (`python server/mp_server.py`):
  - Consoles connect over TCP :7777 (NDJSON), after UDP discovery on :7778.
  - The web UI and the console WebSocket `/switch` are on :8080 (0.0.0.0).
  - The admin port 127.0.0.1:7790 relays `dbg_cmd` debug commands to consoles.
- **Online mode** (`--cloud`, Render free tier, `render.yaml`):
  - Everything is on one public HTTPS port at `mikumod.evanthomas.dev`: the dashboard `/`, `/ws`, `/api/state`, `/api/avatar/<id>.png`, `/health` and the consoles' `/switch`.
  - Consoles have no TLS. They use plaintext `ws://...:80/switch`, which the Cloudflare Worker `cloudflare/worker.js` re-issues to `https://mikumod.onrender.com`.
  - There is no admin port and no LAN TCP/UDP.

## Components
| Name | Code | Notes |
|---|---|---|
| Client Mod | `switch-client-mod/source/program/` (esp. `mp/` — `MpNet.cpp`, `MpApi.cpp`, `MpLobby.cpp`, `MpAvatar.cpp`, `MpJsmn.h`; `Config.cpp`, `ImGuiOverlay.cpp`) | Native C/C++ injected into the game. Parses untrusted server JSON (jsmn) and base64 avatar pixels. Executes `dbg_cmd` sent by the server (taps, cursor, jumps, SD dumps, `server_mode`/`cloud_url` changes). Writes `config.toml`, `mp_servers.txt` and dumps to the SD card. |
| Game Server | `server/mp_server.py`, `server/lobby.py`, `server/avatars.py`, `server/netcheck.py` | Console protocol (NDJSON over TCP or WebSocket), lobby state machine, avatar upload gate. Identity is the `hello.cid` bearer value. |
| Web Bridge | `server/web_bridge.py` | Stdlib HTTP + WebSocket server: static files, `/api/*`, browser `/ws` (auth by admin token or by a claimed owner name), `/switch` adapter, `X-Forwarded-For`/`CF-Connecting-IP` peer logging. |
| Dashboard | `web/src/` (React 19 + Vite) | Spectator/host UI. Keeps the admin token from `?token=` in localStorage. |
| Dev Tooling | `server/admin.py`, `server/bot.py`, `server/sim.py`, `server/fake_client.py`, `tools/` | Developer-only CLIs, bots and simulators. |
| Cloud Edge | `render.yaml`, `cloudflare/worker.js`, `cloudflare/wrangler.toml` | Render service config (generated `MP_WEB_TOKEN`), plaintext-to-HTTPS Worker bridge. |

## Deployment tag values
- `online`: the issue is reachable on the Render/Cloudflare deployment (cloud mode).
- `LAN`: only in local/PC mode (TCP 7777, UDP 7778, admin port, LAN web port).
- `both`: shared code reachable in either mode (e.g. `/switch`, `/ws`, the lobby engine, the console client).

## Trust boundaries
1. Console ↔ server: plaintext NDJSON over TCP or WS. Neither side is authenticated: the server trusts `cid`, and the console trusts whatever server it found by discovery, `server_ip`, `mp_servers.txt` or `cloud_url`.
2. Browser ↔ Web Bridge: anonymous users can spectate (`/api/state`, `/ws` snapshots). Owner rights come from typing the owner's display name; admin rights come from `MP_WEB_TOKEN`.
3. Internet ↔ Render, through Cloudflare (online mode only). The Worker forwards any path and query under `/switch*` to the origin.
4. Localhost ↔ admin port 127.0.0.1:7790 (LAN mode only): no auth, so any local process can drive every console with `dbg_cmd`.
5. Server → console: server messages are parsed by native code inside the game process, which can write to the SD card.

## Entry points
- TCP :7777 `hello` + lobby messages; UDP :7778 discovery (LAN).
- `GET /switch` WS upgrade (both modes; public in online mode).
- `GET /ws` browser WS (`auth`, `action`: kick, set_pick_mode, set_leader, hide_avatar), with an Origin check.
- `GET /api/state`, `/api/avatar/<12hex>.png`, `/health`, static files from `web/dist` (realpath containment).
- `avatar_img` uploads (≤96x96 RGBA, rate-limited) and `avatar_get` relay (same lobby only).
- Admin port 127.0.0.1:7790 (LAN).
- Console side: every server→client message type, including `dbg_cmd`, plus UDP discovery replies.

## Assets
- **Console integrity**: memory safety of the native mod running in the game process, and the user's SD card (saves, `config.toml`). *(inferred: highest-impact asset)*
- **Console session identity**: the `client_id`/`cid`, which works as a bearer token. Raw cids reach every member of a lobby in `lobby` messages, but never reach browsers (HMAC `mid`).
- **Admin token** `MP_WEB_TOKEN`: full control of every lobby from the web.
- **Lobby integrity**: who is owner or leader, kicks, round results and standings.
- **Uploaded portraits**: user content relayed to other consoles and served on the web (moderation via `avatar_hide`).
- **Server availability**: a single asyncio process on Render free tier.
- Player display names and IP addresses (logged). These are low sensitivity.

## Attackers
- A1 Anonymous internet user reaching `mikumod.evanthomas.dev` (online mode).
- A2 Another player: a malicious console, Ryujinx instance or bot in the same lobby or on the same LAN.
- A3 A malicious or impersonating server that consoles connect to (LAN discovery spoofing, a changed `cloud_url`, DNS).
- A4 A network-path attacker on the plaintext console link.
- A5 A local process on the PC hosting a LAN server.

## Existing controls
- Browsers see `mid = HMAC(secret, cid)[:12]`, never raw cids.
- Admin token compared with `hmac.compare_digest`; the WS closes after 5 bad tokens.
- `/ws` Origin must match Host.
- Static files: realpath containment. Avatar PNGs: nosniff, `CSP: sandbox`, CORP.
- Avatar gate: size/format checks, 3 attempts per cid per 60 s, normalized to 96x96, at most 256 stored; `avatar_get` limited to the same lobby, 20 per cid per 60 s.
- Header size limit, head timeout, max WS message size, `hello` within 5 s, protocol version check.
- Admin port bound to 127.0.0.1 and disabled in cloud mode.
- OFF mode: the console opens no socket.

## Known weaknesses (documented by the project in `docs/ACCOUNTS_DESIGN.md`)
- `cid` is a bearer token and is predictable (tick-seeded PRNG). A `hello` with an existing cid replaces that connection.
- Web owner login only checks a claimed display name.
- No TLS to consoles; the console does not authenticate the server.

## Out of scope / accepted risk
- **Dev Tooling is out of scope / low priority (owner).** This covers `server/admin.py`, `bot.py`, `sim.py`, `fake_client.py`, `tools/`, and the 127.0.0.1-only admin port with its unauthenticated `dbg_cmd` relay (attacker A5). These are developer-only. Findings reachable only through them are deprioritized. This does **not** cover the console's own handling of `dbg_cmd` when a malicious server (A3) sends it: that path is the Client Mod and is in scope.
- Nothing else is accepted risk. The documented interim weaknesses (bearer/predictable `cid`, name-claim web login, no TLS to consoles) are to be **reported at full severity** (owner).

## User-provided facts
- Deployment priority: **the online Render server (`mikumod.evanthomas.dev`) matters most; LAN is secondary.**
- Attackers that matter: **all of A1 (anonymous internet), A2 (other players / malicious console or bot in the lobby or on the LAN), A3 (malicious server sending crafted messages to consoles) and A4 (network-path sniffing or tampering on the plaintext console link).**
- The interim weaknesses in `docs/ACCOUNTS_DESIGN.md` are not accepted risk; report them at full severity.
- Dev Tooling is out of scope / low priority (see above).
- Finding tags wanted: `Component` and `Deployment` (online / LAN / both).

## Triage rules for scan findings
- Findings reachable only through Dev Tooling or the local admin port (A5): lower, usually to `low`, or `none` when they only matter for a developer running the tool locally.
- Findings that only matter in LAN mode (TCP :7777, UDP :7778 discovery, LAN web port): may drop one level only when the impact is limited. A2 on the LAN is still an attacker that matters, so LAN findings against console integrity keep their severity.
- Findings on the public online surface (`/switch`, `/ws`, `/api/*`, avatar endpoints, Cloudflare Worker, `render.yaml`): keep the scanner's severity. Raise one only rarely, when the scanner confirmed an easy path from A1 to the admin token, console integrity or lobby takeover on the online server.
- The interim weaknesses are never lowered.

## Open questions
- None outstanding. Update this file when the deployment, attackers or accepted risks change.
