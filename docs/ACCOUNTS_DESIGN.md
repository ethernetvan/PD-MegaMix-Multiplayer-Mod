# Accounts — design for a future implementation

Status: **design only, not implemented.** Read `AGENTS.md` and `docs/MULTIPLAYER_STATUS.md` first.
This replaces the interim "type the owner's name" web login with real accounts that are tied to consoles,
so that "the lobby creator" on the web provably is the person holding that console.

---

## 1. What exists today (interim)

| Piece | Behaviour | Weakness |
|---|---|---|
| Console identity | `client_id` (16 hex chars) in `MikuMikuSwitchPlugin/config.toml`, sent in `hello.cid`. Generated in `Config.cpp` from `exl::util::GetRandomU64()` (= `mt19937_64` seeded with the system tick). | It is a **bearer token**: `mp_server.py` accepts any `hello` with any `cid` and *replaces* the existing connection, so anyone who learns a cid can hijack that console's lobby seat. It's also predictable (tick-seeded PRNG). |
| Display name | `player_name` in config, changed in-game via `set_name`. | Not unique, not verified. |
| Web spectating | `server/web_bridge.py` serves `web/dist` + `/ws`; snapshots use opaque `mid = HMAC(process_secret, cid)[:12]`, never raw cids. | — |
| Web "login" | WS `auth {name}`: if `name.strip().casefold()` equals the **current lobby owner's display name**, that connection gets owner controls for that lobby (`you.owns`). | Anyone can type someone else's name. Name collisions grant rights to whoever matches. |
| Server admin | WS `auth {token}` with the token printed at startup (`--web-token`, default random); also accepted via `?token=` in the URL. | Fine as a break-glass mechanism; keep it. |

Goals of the real system:
1. A web user can only control a lobby if the **console that owns it is linked to their account**.
2. A console's identity can't be spoofed by copying its id (device proof-of-possession).
3. Server stays **Python stdlib only** (`sqlite3`, `hashlib.scrypt`, `hmac`, `secrets`); client stays dependency-free C/C++.
4. Typing on a Switch stays minimal: no passwords on the console, ever.
5. Backwards compatible rollout: old (proto 1) consoles keep working as *guests* during the transition.

Non-goals (for now): email, password reset by email, OAuth, cross-server/global accounts, TLS on the LAN
(called out where it matters; required before any internet deployment / the planned Go server).

---

## 2. Concepts

- **Account** — a person. Username + password, created on the web. May be a server admin.
- **Device** — a console (or Ryujinx install). Identified publicly by its existing `client_id` (kept as the
  lobby engine's `cid`, so `lobby.py` doesn't change), and *authenticated* by a per-device secret key.
- **Link** — device → account (many devices per account, at most one account per device). Created by a
  short pairing code shown on the console and typed into the web UI.
- **Session** — a logged-in browser. Random token in an HttpOnly cookie.
- **Guest** — a device that isn't linked (or a proto-1 console). Guests play normally; nobody can manage a
  guest-owned lobby from the web except server admins.

Permission rule that replaces the name match (single place, `web_bridge.py`):

```
can_manage(session, lobby) = session.account.is_admin
                          or store.account_of_device(lobby.owner_cid) == session.account.id
```

Because ownership is resolved dynamically from `lobby.owner_cid`, it automatically follows the engine's
existing owner hand-off (owner leaves → next member in join order) with no `lobby.py` changes.

---

## 3. Threat model (LAN)

| Threat | Mitigation |
|---|---|
| Web user claims to own someone's lobby | Account-based permission (above); interim name login removed. |
| Copying a console's `client_id` to hijack it | Challenge–response with a per-device key (§5). Server refuses a `cid` whose proof fails; known devices can never be "re-registered" without an admin reset. |
| Password guessing | scrypt hashing, per-IP + per-account rate limiting, generic error messages. |
| Stolen DB | Passwords are scrypt hashes; session tokens stored as SHA-256 hashes; device keys are per-server derived keys (a leaked DB can't be replayed against another server). |
| Cross-site WebSocket hijacking / CSRF | Session cookie `SameSite=Strict; HttpOnly`; WS upgrade already rejects foreign `Origin`; JSON-only POST endpoints with the same Origin check. |
| Malicious server operator learning a device key usable elsewhere | Per-server key derivation `K_s = HMAC(device_secret, "mmmp-server|" + server_id)`. |
| Passive sniffing on the LAN | **Accepted for LAN**: first device registration (TOFU) and web passwords travel in clear text. Document it in the UI ("LAN only — don't reuse an important password"). Requirement before any internet use: TLS (reverse proxy or the Go server). |

---

## 4. Server design

### 4.1 Storage — `server/data/accounts.db` (sqlite3, WAL mode; add `server/data/` to `.gitignore`)

```sql
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
-- rows: schema_version = '1', server_id = 32 random hex chars (generated once)

CREATE TABLE accounts(
  id            INTEGER PRIMARY KEY,
  username      TEXT NOT NULL,                 -- as typed, for display
  username_norm TEXT NOT NULL UNIQUE,          -- casefold(); uniqueness + lookup
  display_name  TEXT NOT NULL,                 -- shown in lobbies; defaults to username
  pw_hash       BLOB NOT NULL,
  pw_salt       BLOB NOT NULL,
  pw_params     TEXT NOT NULL,                 -- e.g. 'scrypt:n=32768,r=8,p=1,dk=32'
  is_admin      INTEGER NOT NULL DEFAULT 0,
  disabled      INTEGER NOT NULL DEFAULT 0,
  created_at    REAL NOT NULL
);

CREATE TABLE sessions(
  token_hash  BLOB PRIMARY KEY,                -- sha256(token); raw token only in the cookie
  account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  created_at  REAL NOT NULL,
  expires_at  REAL NOT NULL,                   -- created + 30 days, sliding on use
  last_seen   REAL NOT NULL,
  user_agent  TEXT
);

CREATE TABLE devices(
  cid         TEXT PRIMARY KEY,                -- existing client_id
  key         BLOB NOT NULL,                   -- K_s (32 bytes), per-server derived device key
  account_id  INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
  label       TEXT,                            -- user-editable, e.g. "Evan's Switch"
  last_name   TEXT,                            -- last in-game name seen
  first_seen  REAL NOT NULL,
  last_seen   REAL NOT NULL
);
```

Pairing codes are short-lived and live **in memory** only (`dict code -> (cid, expires_at)`).

### 4.2 New module `server/accounts.py` (pure, unit-testable; no asyncio)

```python
class AccountStore:
    def __init__(self, path): ...            # opens/creates DB, runs migrations, loads/creates server_id
    server_id: str
    # accounts
    def create_account(self, username, password, now) -> int     # raises AccountError(code)
    def verify_password(self, username, password) -> Account|None
    def set_display_name(self, account_id, name)
    def set_admin(self, account_id, flag)
    # sessions
    def new_session(self, account_id, now, user_agent) -> str     # returns raw token
    def session_account(self, token, now) -> Account|None          # slides expiry, None if expired/disabled
    def end_session(self, token)
    # devices
    def device_key(self, cid) -> bytes|None
    def register_device(self, cid, key, name, now)                 # TOFU; raises if cid already known
    def touch_device(self, cid, name, now)
    def account_of_device(self, cid) -> int|None
    def link_device(self, cid, account_id) / unlink_device(self, cid)
    def devices_of(self, account_id) -> list
    def reset_device(self, cid)                                    # admin: forget key so it can re-register
```

Rules:
- Username: 3–20 chars, `[A-Za-z0-9_.-]`, unique by `casefold()`. Display name: 1–20 printable chars
  (the console UI budget; `MpLobby` member names are `char[40]`).
- Password: 8–128 chars. `hashlib.scrypt(pw.encode(), salt=os.urandom(16), n=2**15, r=8, p=1,
  maxmem=64*2**20, dklen=32)`; compare with `hmac.compare_digest`. Store params so they can be raised later
  (rehash on next successful login if params are outdated).
- **scrypt takes ~50–100 ms: always call it via `loop.run_in_executor`** from the asyncio server so the
  100 ms lobby ticker and game traffic don't stall.
- Session token: `secrets.token_urlsafe(32)`; DB stores `sha256(token)`.
- Rate limiting (in memory): per IP and per username, 5 failed logins / 60 s → reject with
  `rate_limited` and exponential back-off up to 15 min. Same for pair-code attempts (codes are short).
- Generic login error (`bad_credentials`) for unknown user *and* wrong password.

### 4.3 Bootstrap / admin
- First account ever created gets `is_admin = 1` (print a log line). Afterwards admins are managed from the
  web (admins only) or a CLI: `python server/accounts_cli.py list|make-admin NAME|disable NAME|
  reset-password NAME|reset-device CID`.
- Keep `--web-token` as break-glass full admin (no account needed).
- `--registration open|admin` (default `open` on LAN): `admin` = only admins can create accounts.

### 4.4 Device authentication — console protocol v2

Key material on the console:
- `device_secret`: 32 random bytes, generated once, stored in its own file (not `config.toml`, which people
  share and which the game holds open).
- Per server: `K_s = HMAC-SHA256(device_secret, "mmmp-server|" + server_id)`.

Handshake (replaces the current `hello` → `welcome`):

```
C->S  hello      {proto:2, cid, name, mod, game}
S->C  challenge  {nonce:<32 hex>, server_id:<32 hex>, known:bool}
C->S  auth       {mac: hex(HMAC-SHA256(K_s, "mmmp-auth-v2|" + server_id + "|" + cid + "|" + nonce)),
                  key?: hex(K_s)}            # key ONLY when known == false (first contact, TOFU)
S->C  welcome    {proto:2, cid, rejoin, warn, account: {username, display_name}|null}
   or error      {code: "auth_failed" | "proto_mismatch" | "device_reset_required", msg}
```

Server logic in `mp_server.Server.handle` (keep it in a helper, e.g. `device_auth.py`, so `handle` stays short):
- `known == true` → verify mac with stored key (`hmac.compare_digest`); failure → `auth_failed`, close,
  log loudly. **The existing "replace old connection" path only runs after a successful proof** — this is
  what fixes hijacking.
- `known == false` → require `key`, check `mac` with it, then `register_device`.
- Proto 1 hello → accepted as **guest** while `--allow-proto1` (default on during rollout; turn off later).
  Guests can't be linked and their lobbies can't be web-managed except by admins.
- On success: `touch_device`, and if the device is linked, use the account's `display_name` as the client's
  name (see §7 open question 2) and include `account` in `welcome`.

### 4.5 Pairing (console shows code, web user types it)

```
C->S  pair_begin  {}
S->C  pair_code   {code:"K7Q-4MZ", expires_in:300}       # 6 chars from "ABCDEFGHJKLMNPQRSTUVWXYZ23456789", 5 min
C->S  pair_cancel {}
web   POST /api/devices/link {code} (logged in)  ->  server links cid to session.account
S->C  account     {username, display_name}               # to the console, immediately
C->S  unlink      {}                                    # console-side unlink
S->C  account     {username:null, display_name:null}
```

- One active code per device; a new `pair_begin` replaces the old code. Codes are single use.
- Linking a device that's already linked to another account is refused (`device_linked`) unless that
  account unlinks it first or an admin forces it.
- Only proto-2 (authenticated) devices can be paired.

### 4.6 Web API (additions to `server/web_bridge.py`)

The HTTP parser currently only handles GET/HEAD. Add POST with `Content-Length` bodies (cap 8 KiB,
`Content-Type: application/json` required, same Origin check as the WS upgrade, `Cache-Control: no-store`).

| Method / path | Body | Result |
|---|---|---|
| `POST /api/register` | `{username, password}` | creates account + session; `Set-Cookie` |
| `POST /api/login` | `{username, password}` | `Set-Cookie: mmmp_session=<token>; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000` |
| `POST /api/logout` | `{}` | ends session, clears cookie |
| `GET /api/me` | — | `{account:{username,display_name,admin}|null}` |
| `GET /api/devices` | — | `[{label, last_name, last_seen, online, mid}]` for my devices (never raw cids) |
| `POST /api/devices/link` | `{code}` | links the pairing device; pushes `account` to the console |
| `POST /api/devices/unlink` | `{mid}` | unlinks one of my devices |
| `POST /api/devices/label` | `{mid, label}` | rename a device |
| `POST /api/account/display_name` | `{name}` | updates display name (+ live `set_name` for online linked consoles) |
| admin: `GET /api/admin/accounts`, `POST /api/admin/{make_admin,disable,reset_device}` | | |

WebSocket: on upgrade, read the `mmmp_session` cookie → `session_account()`; store the account on the
connection. Re-check on every action (sessions can be revoked). The snapshot's `you` becomes:

```json
"you": {"account": {"username": "evan", "display_name": "Evan", "admin": false} | null,
        "admin": false, "owns": ["ABCD"]}
```

`owns` = lobbies where `can_manage` is true via device linkage (not admin-wide). Also add per-member
`"linked": bool` (and optionally `"account": username`) to snapshot members so spectators can see who is
verified.

### 4.7 Exact interim code to replace / remove
- `server/web_bridge.py`: `WebBridge._auth` (the `name` claim stored on `WsConn.name`) and
  `WebBridge.you_for` (the owner-name `casefold()` match that computes `you.owns`) → replaced by the cookie
  session + `can_manage`. `_action` already authorizes via `you_for`, so it only needs the new `you`. Keep the
  `token` branch of `_auth`.
- `web/src/auth.ts`: drop the `mmmp.name` localStorage key; keep `mmmp.token` + `?token=` handling.
- `web/src/components/LoginDialog.tsx`: replace "Lobby creator's username" with Log in / Create account tabs.
- `web/src/types.ts`: `You` gains `account`; `name` goes away.
- `docs/MULTIPLAYER_STATUS.md` / `AGENTS.md`: update the web section.

---

## 5. Client (Switch mod) design

All new files go in `switch-client-mod/source/program/mp/` and must have globally unique names (flat object
dir, case-insensitive FS) — use the `Mp` prefix.

### 5.1 `MpCrypto.hpp/.cpp` (new)
- SHA-256 + HMAC-SHA256 (~150 lines, plain C++; e.g. a from-scratch implementation following FIPS 180-4;
  no heap, no exceptions). API:
  `void Sha256(const void*, size_t, uint8_t out[32]);`
  `void HmacSha256(const uint8_t* key, size_t klen, const void* msg, size_t mlen, uint8_t out[32]);`
  `void ToHex(const uint8_t*, size_t, char* out);  bool FromHex(const char*, uint8_t*, size_t);`
- Verify against RFC 4231 test vectors. The file has no Switch dependencies, so also compile it on the PC
  (`g++ -DMP_CRYPTO_SELFTEST`) and cross-check against Python's `hmac` for the exact v2 auth string.

### 5.2 Device secret
- File: `ExlSD:/MikuMikuSwitchPlugin/mp_device.key` (64 hex chars + newline), next to `config.toml`.
  Generate once if missing or malformed.
- Entropy: **do not use `exl::util::GetRandomU64()`** (tick-seeded `mt19937_64`). Use kernel entropy:
  `svcGetInfo(&v, InfoType_RandomEntropy, INVALID_HANDLE, i)` for `i = 0..3` (declared in
  `lib/nx/kernel/svc.h`), SHA-256 it together with `nn::os::GetSystemTick()` and the old client_id, use the
  32-byte digest as `device_secret`.
- Keep `client_id` as the public id (unchanged, so existing lobbies/tests keep working). Optionally switch
  new installs to derive `client_id = hex(sha256("mmmp-cid|" + device_secret))[:16]`.
- Never log the secret or `K_s`.

### 5.3 `MpNet.cpp` — `SayHello()`
Current code sends `hello` and blocks (non-blocking recv loop, 5 s) for one line that must be `welcome` or
`error`. Change to a small state machine within the same loop:
1. send `hello {proto:2,...}`;
2. on `challenge`: compute `K_s` (cache per `server_id` in memory), mac; send `auth` (+ `key` if
   `known:false`);
3. on `welcome`: existing handling (hand the rest of the buffer to `HandleLine`), plus store `account`;
4. on `error auth_failed`: show a clear status ("This console's key doesn't match the server — ask the host
   to reset it") and back off 30 s instead of the generic 10 s.
`rbuf[512]` / `jsmntok_t tok[48]` are enough for these messages. Bump the `mod` version string.

### 5.4 `MpLobby.cpp` — message handling (dispatcher is the `strcmp(t, ...)` chain in `HandleLine`)
- `welcome.account`, `account` → `s_accountUser[24]`, `s_accountDisplay[40]` (empty = not linked).
- `pair_code` → `s_pairCode[8]`, `s_pairExpiresMs`.
- Commands: `CmdPairBegin()`, `CmdPairCancel()`, `CmdUnlink()` (same pattern as `CmdSetName`).
- Expose via the existing state snapshot struct consumed by `MpUi`.

### 5.5 `MpUi.cpp` — overlay
- Home screen (Edit Name / CREATE LOBBY / JOIN LOBBY): add a status strip at the top: "Guest — Link
  account" (button) or "Linked as **evan**".
- New screen `SC_LINK`: huge pairing code, countdown, instructions "On your phone/PC open
  `http://<server-ip>:8080/` → Log in → My consoles → Link console", Cancel button. Auto-closes to the home
  screen on `account` received ("Linked as evan").
- When linked: "Unlink" button with a confirm step. Large touch targets, consistent with the round-5 UI rules
  (X-only close, Switch-style rounded buttons).
- If linked, "Edit Name" edits the account display name (server applies it to the account).

### 5.6 Test bots
- `server/bot.py`, `server/sim.py`: add `--proto 2` with a persisted per-bot key file so bots exercise the
  same handshake; keep proto 1 for guest tests.

---

## 6. Web frontend design
- Header: "Log in" opens a dialog with **Log in** / **Create account** tabs (username, password; show the
  LAN-plaintext warning). Logged in: avatar initial + display name, menu: My consoles, Account, Log out.
- **My consoles** page (`#/consoles`): "Link a console" input (6-char code, auto-uppercase, tolerant of the
  dash), list of linked devices (label editable, last name, online dot, Unlink).
- Lobby view: owner controls appear when `you.owns` includes the lobby (same components as today); a small
  "verified" check next to linked members.
- Admin (`you.account.admin` or token): an Admin page listing accounts/devices with make-admin / disable /
  reset-device.
- Fetch helpers send JSON with `credentials: "same-origin"`; after login/logout reconnect the WS so the
  server re-reads the cookie.

---

## 7. Open questions for the owner (ask before implementing)
1. Should joining a lobby require a linked account, or stay open to guests? (Proposal: open; guests allowed.)
2. For linked consoles, is the account display name authoritative (overrides `player_name`)? (Proposal: yes.)
3. Registration open to anyone on the LAN, or admin-only? (Proposal: open; first account = admin.)
4. Should lobby ownership be transferable from the web? (Needs a new engine op `set_owner`.)
5. Accounts per server (LAN) now; when the backend moves to Go for online play, do accounts become global?
   If so, keep the `devices`/`accounts` shape and add TLS + email-based recovery then.

---

## 8. Implementation plan (phased, each phase shippable)
1. **Server accounts + web login** — `accounts.py`, `accounts_cli.py`, POST support + endpoints in
   `web_bridge.py`, Login/Create account UI. The interim name login stays enabled for lobby ownership.
   Tests: `test_accounts.py` (hash/verify, rehash on param change, username rules, session expiry/sliding,
   rate limiter with an injected clock), web bridge HTTP tests for cookies/Origin/JSON-only.
2. **Device auth (proto 2)** — `MpCrypto`, device secret, `SayHello` state machine; server handshake with
   proto-1 guests still allowed. Tests: Python HMAC vectors vs the C++ self-test output; handshake unit tests
   (known/unknown device, bad mac, replayed nonce rejected because nonces are per-connection); bot proto 2.
   Device check: Ryujinx + real Switch connect, reconnect, and a spoofed `cid` from `fake_client.py` is
   rejected.
3. **Pairing + account-based permissions** — pairing messages, `SC_LINK` UI, My consoles page,
   `can_manage` via linkage, **remove the interim name login**. Tests: pairing expiry/single-use/
   already-linked, permission matrix (owner-linked, other account, admin, guest lobby).
4. **Hardening** — `--allow-proto1` off by default, admin page, DB backup note, docs.

Verification commands (existing): `cd server && python -B -m unittest test_lobby.py test_web_bridge.py -v`,
`cd web && npm run typecheck && npm test && npm run build`, client build `cd switch-client-mod && ./exlaunch.sh build`.
