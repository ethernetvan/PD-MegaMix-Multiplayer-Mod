#!/usr/bin/env python3
"""web_bridge.py - HTTP+WebSocket spectator/admin bridge for mp_server.

Serves the built web/ SPA plus:
  GET /api/health -> {"ok":true}
  GET /api/state  -> anonymous snapshot
  GET /ws         -> WebSocket: hello+feed backlog, snapshot pushes (200ms),
                     auth (name claim and/or admin token), lobby actions.
Stdlib asyncio only. Console cids/IPs are never sent to browsers; members
are identified by per-process hmac "mid" tokens.
"""

import asyncio
import base64
import hashlib
import hmac
import json
import mimetypes
import os
import secrets
import socket
import struct
import time
import urllib.parse
from collections import OrderedDict, deque

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HEAD = 16384          # request line + headers cap
MAX_MSG = 65536           # max assembled WS client message
FEED_LEN = 100            # feed entries kept per lobby
SEEN_LEN = 2000           # dedupe map bound for observe()
HEAD_TIMEOUT = 10
PUSH_EVERY = 0.2
PUSH_HEARTBEAT = 10
WS_PING_EVERY = 20

MIME = {".js": "text/javascript", ".css": "text/css", ".svg": "image/svg+xml",
        ".json": "application/json", ".woff2": "font/woff2",
        ".html": "text/html", ".htm": "text/html", ".png": "image/png",
        ".ico": "image/x-icon", ".map": "application/json",
        ".webp": "image/webp", ".txt": "text/plain"}

STATUS = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
          405: "Method Not Allowed", 431: "Request Header Fields Too Large",
          503: "Service Unavailable"}

NOT_BUILT = (b"<!doctype html><meta charset=utf-8><title>MegaMix MP</title>"
             b"<body style='font-family:sans-serif;background:#0f1720;color:#dde'>"
             b"<h1>Web UI not built yet</h1><p>Build the frontend first:</p>"
             b"<pre>cd web &amp;&amp; npm ci &amp;&amp; npm run build</pre>")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return socket.gethostname()


# ------------------------------------------------- websocket codec
def ws_accept(key):
    return base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode()).digest()).decode()


def encode_frame(opcode, payload):
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return head + payload


class WsError(Exception):
    def __init__(self, code):
        super().__init__(f"ws close {code}")
        self.code = code


async def _one_frame(reader, masked=True):
    b0, b1 = await reader.readexactly(2)
    op = b0 & 0x0F
    if masked and not (b1 & 0x80):
        raise WsError(1002)                     # client frames must be masked
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await reader.readexactly(8))[0]
    if n > MAX_MSG:
        raise WsError(1009)                     # don't buffer absurd lengths
    mask = await reader.readexactly(4) if b1 & 0x80 else None
    data = bytearray(await reader.readexactly(n))
    if mask is not None:
        for i in range(n):
            data[i] ^= mask[i & 3]
    return b0 & 0x80, op, bytes(data)


async def read_frame(reader, masked=True):
    """-> (opcode, payload) for one complete message; joins continuations."""
    parts, total, op0 = [], 0, None
    while True:
        fin, op, data = await _one_frame(reader, masked)
        if op >= 8:                             # control frame
            if parts:
                raise WsError(1002)
            return op, data
        if op0 is None:
            if op == 0:
                raise WsError(1002)
            op0 = op
        elif op != 0:
            raise WsError(1002)
        parts.append(data)
        total += len(data)
        if total > MAX_MSG:
            raise WsError(1009)
        if fin:
            return op0, b"".join(parts)


class WsConn:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.name = None
        self.admin = False
        self.bad_tokens = 0
        self.last_sig = None
        self.last_sent = 0.0
        self.last_ping = time.time()
        self.alive = True
        self.lock = asyncio.Lock()

    async def send_raw(self, data):
        async with self.lock:
            try:
                self.writer.write(data)
                await asyncio.wait_for(self.writer.drain(), 5)
            except Exception:
                self.alive = False
                try:
                    self.writer.close()
                except Exception:
                    pass

    async def send_json(self, obj):
        await self.send_raw(encode_frame(1, json.dumps(obj).encode()))


class WebBridge:
    def __init__(self, server, host, port, token, static_dir=None):
        self.server = server
        self.host = host
        self.port = port
        self.token = token
        self.static_dir = static_dir or os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "web", "dist")
        self._secret = secrets.token_bytes(16)
        self.started_at = time.time()
        self.conns = set()
        self.feeds = {}                    # code -> deque(entries)
        self._seen = OrderedDict()         # id(msg) -> msg (dedupe, refs held)
        self._results_sent = set()         # (code, round) results announced
        self._feed_pending = []            # (code, entry) awaiting push
        self._httpd = None
        self._task = None

    async def start(self):
        try:
            self._httpd = await asyncio.start_server(
                self._on_conn, self.host, self.port)
        except OSError as e:
            log(f"web UI disabled: cannot bind {self.host}:{self.port} "
                f"({e}); use --web-port")
            return
        self.port = self._httpd.sockets[0].getsockname()[1]
        self._task = asyncio.create_task(self._push_loop())
        ip = lan_ip()
        log(f"web UI http://{ip}:{self.port}/ (bind {self.host})")
        log(f"admin token: {self.token} "
            f"(open http://{ip}:{self.port}/?token={self.token} "
            f"to manage any lobby)")

    # ------------------------------------------------- ids
    def mid(self, cid):
        return hmac.new(self._secret, str(cid).encode(),
                        hashlib.sha256).hexdigest()[:12]

    @staticmethod
    def _member(lobby, cid):
        for m in lobby.members:
            if m.cid == cid:
                return m
        return None

    def _mid_member(self, lobby, mid):
        if not mid:
            return None
        for m in lobby.members:
            if hmac.compare_digest(self.mid(m.cid), str(mid)):
                return m
        return None

    def you_for(self, conn):
        owns = []
        if conn.name:
            key = conn.name.strip().casefold()
            for code, l in self.server.lobby.lobbies.items():
                o = self._member(l, l.owner_cid)
                if o and o.name.strip().casefold() == key:
                    owns.append(code)
        return {"name": conn.name, "admin": conn.admin, "owns": sorted(owns)}

    # ------------------------------------------------- snapshot
    def snapshot(self):
        lm = self.server.lobby
        now = time.time()
        s = {"t": "snapshot",
             "server": {"name": socket.gethostname(), "now": now,
                        "started_at": self.started_at,
                        "clients_online": len(self.server.clients)},
             "consoles": [], "lobbies": []}
        for c in self.server.clients.values():
            l = lm.by_cid.get(c.cid)
            s["consoles"].append({"mid": self.mid(c.cid), "name": c.name,
                                  "mod": c.mod, "lobby": l.code if l else None})
        for code in sorted(lm.lobbies):
            s["lobbies"].append(self._lobby_snap(lm.lobbies[code], now))
        return s

    def _lobby_snap(self, l, now):
        lm = self.server.lobby
        r = l.round
        d = {"code": l.code, "phase": l.phase, "pick_mode": l.pick_mode,
             "owner": self.mid(l.owner_cid) if l.owner_cid else None,
             "leader": self.mid(l.leader()) if l.leader() else None,
             "assigned": self.mid(l.assigned_cid) if l.assigned_cid else None,
             "pending_pick_mode": None, "group_paused": l.group_paused,
             "vote": None,
             "sel": ({"pv": l.sel_pv, "name": l.sel_name}
                     if l.sel_pv is not None else None),
             "round": None, "members": [], "standings": None}
        if l.pending_pick_mode:
            mode, cid = l.pending_pick_mode
            d["pending_pick_mode"] = {"mode": mode,
                                      "assigned": self.mid(cid) if cid else None}
        if l.vote is not None and r is not None:
            need = sum(1 for c in r.players
                       if self._member(l, c) and self._member(l, c).connected)
            d["vote"] = {"kind": l.vote["kind"],
                         "agreed": sorted(self.mid(c) for c in l.vote["agreed"]),
                         "need": need, "ends_at": l.vote["ends_at"]}
        if r is not None:
            d["round"] = {"id": r.id, "pv": r.pv, "name": r.name,
                          "leader": self.mid(r.leader) if r.leader else None,
                          "players": [self.mid(c) for c in r.players],
                          "spectators": [self.mid(c) for c in r.spectators],
                          "dropped": [self.mid(c) for c in r.dropped],
                          "started": r.started, "launch_at": r.launch_at}
            if l.phase == "RESULTS" or r.first_final_at is not None:
                rows, complete = lm._standings(l, now)
                d["standings"] = {"complete": complete, "e": [
                    {"mid": self.mid(x["cid"]), "name": x["name"],
                     "score": x["score"], "pct": x["pct"],
                     "max_combo": x["max_combo"], "life": x["life"],
                     "rank": x["rank"], "final": x["final"],
                     "connected": x["connected"],
                     "autoplay": (self._member(l, x["cid"]).autoplay
                                if self._member(l, x["cid"]) else False)}
                    for x in rows]}
        for m in l.members:
            d["members"].append({
                "mid": self.mid(m.cid), "name": m.name,
                "connected": m.connected, "role": m.role, "loaded": m.loaded,
                "ready": m.ready, "reason": m.ready_reason,
                "autoplay": m.autoplay, "back_in_sel": m.back_in_sel,
                "score": m.state.get("score", 0),
                "combo": m.state.get("combo", 0),
                "pct": m.state.get("pct", 0.0),
                "life": m.state.get("life", 0),
                "final": m.final})
        return d

    # ------------------------------------------------- activity feed
    def observe(self, outbox):
        """Called at the top of Server.drain_outbox with the pending outbox."""
        lobbies = self.server.lobby.lobbies
        for cid, msg in outbox:
            k = id(msg)
            if k in self._seen:
                continue
            self._seen[k] = msg            # holding the ref keeps ids unique
            while len(self._seen) > SEEN_LEN:
                self._seen.popitem(last=False)
            lobby = self.server.lobby.by_cid.get(cid)
            if lobby is None:
                continue
            e = self._feed_entry(lobby, msg)
            if e is not None:
                self._add_feed(lobby.code, e)
        for code in list(self.feeds):
            if code not in lobbies:
                del self.feeds[code]
        self._results_sent = {k for k in self._results_sent if k[0] in lobbies}

    def _feed_entry(self, lobby, msg):
        t = msg.get("t")
        now = time.time()
        if t == "event":
            return {"ts": now, "kind": "event", "ev": msg.get("kind"),
                    "mid": self.mid(msg.get("cid")),
                    "name": msg.get("name"), "value": msg.get("value")}
        if t == "sel":
            return {"ts": now, "kind": "sel", "pv": msg.get("pv"),
                    "name": msg.get("name")}
        if t == "launch":
            return {"ts": now, "kind": "launch", "round": msg.get("round"),
                    "pv": msg.get("pv"), "name": msg.get("name")}
        if t == "start":
            return {"ts": now, "kind": "start", "round": msg.get("round")}
        if t in ("pause", "resume"):
            by = msg.get("by")
            m = self._member(lobby, by)
            return {"ts": now, "kind": t,
                    "mid": self.mid(by) if by else None,
                    "name": m.name if m else None}
        if t == "pause_vote":
            return {"ts": now, "kind": "pause_vote", "vote": msg.get("kind"),
                    "agreed": len(msg.get("agreed", [])),
                    "need": msg.get("need"),
                    "expired": bool(msg.get("expired"))}
        if t == "standings" and msg.get("complete"):
            key = (lobby.code, msg.get("round"))
            if key in self._results_sent:
                return None
            self._results_sent.add(key)
            return {"ts": now, "kind": "results", "round": msg.get("round")}
        if t == "round_end":
            m = self._member(lobby, msg.get("next_leader"))
            return {"ts": now, "kind": "round_end", "round": msg.get("round"),
                    "name": m.name if m else None}
        return None

    def _add_feed(self, code, entry):
        d = self.feeds.get(code)
        if d is None:
            d = self.feeds[code] = deque(maxlen=FEED_LEN)
        d.append(entry)
        self._feed_pending.append((code, entry))

    # ------------------------------------------------- HTTP
    async def _on_conn(self, reader, writer):
        try:
            head = await asyncio.wait_for(
                reader.readuntil(b"\r\n\r\n"), HEAD_TIMEOUT)
        except Exception:
            writer.close()
            return
        if len(head) > MAX_HEAD:
            await self._respond(writer, 431, b"headers too large",
                                "text/plain")
            return
        try:
            req, hdrs = self._parse_head(head)
        except Exception:
            await self._respond(writer, 400, b"bad request", "text/plain")
            return
        method, target = req
        if method not in ("GET", "HEAD"):
            await self._respond(writer, 405, b"method not allowed",
                                "text/plain", [("Allow", "GET")])
            return
        path = urllib.parse.urlsplit(target).path
        if path == "/ws":
            if "websocket" in hdrs.get("upgrade", "").lower():
                await self._ws_session(reader, writer, hdrs)
            else:
                await self._respond(writer, 400, b"expected websocket upgrade",
                                    "text/plain")
            return
        if path == "/api/health":
            await self._respond(writer, 200, b'{"ok":true}',
                                "application/json", head_only=method == "HEAD")
            return
        if path == "/api/state":
            snap = self.snapshot()
            snap["you"] = {"name": None, "admin": False, "owns": []}
            await self._respond(writer, 200, json.dumps(snap).encode(),
                                "application/json",
                                head_only=method == "HEAD")
            return
        await self._static(writer, path, method == "HEAD")

    @staticmethod
    def _parse_head(head):
        lines = head.decode("latin-1").split("\r\n")
        method, target, _ = lines[0].split(" ", 2)
        hdrs = {}
        for ln in lines[1:]:
            if ":" not in ln:
                continue
            k, v = ln.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
        return (method, target), hdrs

    async def _respond(self, writer, status, body, ctype, extra=(),
                       head_only=False):
        h = [f"HTTP/1.1 {status} {STATUS.get(status, '?')}",
             f"Content-Type: {ctype}",
             f"Content-Length: {len(body)}", "Connection: close"]
        h += [f"{k}: {v}" for k, v in extra]
        writer.write(("\r\n".join(h) + "\r\n\r\n").encode()
                     + (b"" if head_only else body))
        try:
            await writer.drain()
        except Exception:
            pass
        try:
            writer.close()
        except Exception:
            pass

    async def _static(self, writer, path, head_only):
        root = os.path.realpath(self.static_dir)
        index = os.path.join(root, "index.html")
        if not os.path.isfile(index):
            await self._respond(writer, 503, NOT_BUILT, "text/html")
            return
        rel = urllib.parse.unquote(path)
        p = os.path.realpath(os.path.join(root, rel.lstrip("/")))
        if p != root and not p.startswith(root + os.sep):
            await self._respond(writer, 404, b"not found", "text/plain")
            return
        if os.path.isfile(p):
            fpath = p
        elif not os.path.splitext(rel)[1] and not path.startswith("/api"):
            fpath = index                       # SPA fallback
        else:
            await self._respond(writer, 404, b"not found", "text/plain")
            return
        try:
            with open(fpath, "rb") as f:
                body = f.read()
        except OSError:
            await self._respond(writer, 404, b"not found", "text/plain")
            return
        ctype = (MIME.get(os.path.splitext(fpath)[1].lower())
                 or mimetypes.guess_type(fpath)[0]
                 or "application/octet-stream")
        extra = []
        if path.startswith("/assets/"):
            extra.append(("Cache-Control",
                          "public, max-age=31536000, immutable"))
        elif os.path.basename(fpath) == "index.html":
            extra.append(("Cache-Control", "no-cache"))
        await self._respond(writer, 200, body, ctype, extra, head_only)

    # ------------------------------------------------- websocket session
    async def _ws_session(self, reader, writer, hdrs):
        origin = hdrs.get("origin")
        if origin:
            oh = urllib.parse.urlsplit(origin).hostname or ""
            hh = urllib.parse.urlsplit(
                "//" + hdrs.get("host", "")).hostname or ""
            if oh.lower() != hh.lower():
                await self._respond(writer, 403, b"bad origin", "text/plain")
                return
        key = hdrs.get("sec-websocket-key")
        if not key:
            await self._respond(writer, 400, b"missing sec-websocket-key",
                                "text/plain")
            return
        writer.write(("HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {ws_accept(key)}\r\n\r\n"
                      ).encode())
        await writer.drain()
        conn = WsConn(reader, writer)
        self.conns.add(conn)
        try:
            await conn.send_json({
                "t": "hello",
                "feed": {c: list(d) for c, d in self.feeds.items()}})
            await self._push_one(conn, force=True)
            while conn.alive:
                try:
                    op, data = await read_frame(conn.reader)
                except WsError as e:
                    await conn.send_raw(
                        encode_frame(8, struct.pack("!H", e.code)))
                    break
                except (asyncio.IncompleteReadError, ConnectionError,
                        OSError):
                    break
                if op == 8:
                    await conn.send_raw(encode_frame(8, data[:125]))
                    break
                if op == 9:
                    await conn.send_raw(encode_frame(10, data))
                    continue
                if op != 1:
                    continue
                try:
                    msg = json.loads(data)
                except Exception:
                    continue
                if not isinstance(msg, dict):
                    continue
                try:
                    reply = await self._dispatch(conn, msg)
                except Exception as e:
                    log(f"web dispatch error: {e!r}")
                    rid = msg.get("id")
                    reply = {"t": "reply",
                             "id": rid if isinstance(rid, (int, str)) else None,
                             "ok": False, "error": "bad_request"}
                if reply is not None:
                    await conn.send_json(reply)
        finally:
            conn.alive = False
            self.conns.discard(conn)
            try:
                writer.close()
            except Exception:
                pass

    # ------------------------------------------------- client messages
    async def _dispatch(self, conn, msg):
        t = msg.get("t")
        if t == "auth":
            return await self._auth(conn, msg)
        if t == "action":
            return await self._action(conn, msg)
        return {"t": "reply", "id": msg.get("id"), "ok": False,
                "error": "bad_request"}

    async def _auth(self, conn, msg):
        tok = msg.get("token")
        if tok:
            if not hmac.compare_digest(str(tok), self.token):
                conn.bad_tokens += 1
                if conn.bad_tokens >= 5:
                    conn.alive = False
                return {"t": "reply", "id": msg.get("id"), "ok": False,
                        "error": "bad_token"}
            conn.admin = True
        else:
            conn.admin = False
        name = msg.get("name")
        conn.name = (name.strip()
                     if isinstance(name, str) and name.strip() else None)
        await self._push_one(conn, force=True)
        return {"t": "reply", "id": msg.get("id"), "ok": True,
                "you": self.you_for(conn)}

    async def _action(self, conn, msg):
        lm = self.server.lobby
        rid = msg.get("id")

        def err(e):
            return {"t": "reply", "id": rid, "ok": False, "error": e}

        code = msg.get("lobby")
        if not isinstance(code, str):
            return err("bad_request")
        lobby = lm.lobbies.get(code)
        if lobby is None:
            return err("not_found")
        you = self.you_for(conn)
        if not (you["admin"] or code in you["owns"]):
            return err("forbidden")
        op = msg.get("op")
        now = time.time()
        entry = {"ts": now, "kind": "web_action", "op": op,
                 "by": "admin" if conn.admin else conn.name}
        if op == "kick":
            m = self._mid_member(lobby, msg.get("member"))
            if m is None or (m.cid == lobby.owner_cid and not conn.admin):
                return err("bad_member")
            lm.on_admin({"cmd": "kick", "cid": m.cid}, now)
            entry["name"] = m.name
        elif op == "set_pick_mode":
            mode = msg.get("mode")
            if mode not in ("owner", "assigned", "rotate"):
                return err("bad_mode")
            cid = None
            if mode == "assigned":
                m = self._mid_member(lobby, msg.get("member"))
                if m is None:
                    return err("bad_member")
                cid = m.cid
                entry["name"] = m.name
            entry["mode"] = mode
            lm.on_message(lobby.owner_cid,
                          {"t": "set_pick_mode", "mode": mode, "cid": cid},
                          now)
        elif op == "set_leader":
            if lobby.pick_mode != "rotate":
                return err("bad_mode")
            m = self._mid_member(lobby, msg.get("member"))
            if m is None:
                return err("bad_member")
            lm.on_admin({"cmd": "set_leader", "code": code, "cid": m.cid},
                        now)
            entry["name"] = m.name
        else:
            return err("bad_request")
        await self.server.drain_outbox()
        self._add_feed(code, entry)
        await self._push_one(conn, force=True)
        return {"t": "reply", "id": rid, "ok": True}

    # ------------------------------------------------- snapshot push
    @staticmethod
    def _sig(base, you):
        b = dict(base)
        srv = dict(b["server"])
        srv.pop("now", None)
        b["server"] = srv
        return (json.dumps(b, sort_keys=True) + "|"
                + json.dumps(you, sort_keys=True))

    async def _push_one(self, conn, force=False):
        base = self.snapshot()
        you = self.you_for(conn)
        sig = self._sig(base, you)
        now = time.time()
        if not force and sig == conn.last_sig and \
                now - conn.last_sent < PUSH_HEARTBEAT:
            return
        base["you"] = you
        await conn.send_json(base)
        conn.last_sig = sig
        conn.last_sent = now

    async def _push_loop(self):
        last_err = 0.0
        while True:
            await asyncio.sleep(PUSH_EVERY)
            try:
                await self._push_all()
            except Exception as e:
                if time.time() - last_err > 5:
                    last_err = time.time()
                    log(f"web push error: {e!r}")

    async def _push_all(self):
        now = time.time()
        pend, self._feed_pending = self._feed_pending, []
        if not self.conns:
            return
        base = self.snapshot()
        b = dict(base)
        srv = dict(b["server"])
        srv.pop("now", None)
        b["server"] = srv
        sig0 = json.dumps(b, sort_keys=True) + "|"
        for conn in list(self.conns):
            if not conn.alive:
                self.conns.discard(conn)
                continue
            for code, entry in pend:
                await conn.send_json(
                    {"t": "feed", "lobby": code, "entry": entry})
            if now - conn.last_ping >= WS_PING_EVERY:
                await conn.send_raw(encode_frame(9, b""))
                conn.last_ping = now
            you = self.you_for(conn)
            sig = sig0 + json.dumps(you, sort_keys=True)
            if sig != conn.last_sig or now - conn.last_sent >= PUSH_HEARTBEAT:
                msg = dict(base)
                msg["you"] = you
                await conn.send_json(msg)
                conn.last_sig = sig
                conn.last_sent = now
