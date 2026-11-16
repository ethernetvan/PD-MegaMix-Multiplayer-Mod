#!/usr/bin/env python3
"""mp_server.py - MegaMix LAN multiplayer server (milestone 1: transport + dbg telemetry).

Protocol v1 (NDJSON, one object per line):
  C->S hello/ping/dbg_state ; S->C welcome/error/pong
UDP discovery on --discovery-port: {"t":"discover"} -> unicast {"t":"server",...}

Transports (same handler, same lobby state):
  local (default): raw TCP on --port + UDP discovery on --discovery-port
      for consoles on the LAN, web UI on --web-port (which also accepts
      consoles over WebSocket on /switch).
  cloud (--cloud, e.g. Render): ONE public port (--web-port, default
      $PORT) carrying the web UI, /health, the browser /ws and the
      console WebSocket /switch; no raw TCP/UDP game ports.
"""

import argparse
import asyncio
import base64
import json
import os
import secrets
import socket
import sys
import threading
import time

from lobby import LobbyManager
from avatars import (AvatarStore, UploadGate, RelayGate, IMG_ID,
                     parse_upload)
from web_bridge import WebBridge
import netcheck

sys.stdout.reconfigure(errors="replace")

PROTO = 1
MOD_VERSION_EXPECTED = None  # set via --mod-version-expected


def ts():
    return time.strftime("%H:%M:%S")


def log(msg):
    print(f"[{ts()}] {msg}", flush=True)


def compact_dbg(msg):
    keys = ["f", "loaded", "paused", "score", "combo", "max_combo", "life",
            "pct", "scoring", "type", "diff", "pv", "byte2",
            "ps", "ip", "iv", "pe", "res", "st", "bd", "bde"]
    parts = [f"{k}={msg[k]}" for k in keys if k in msg]
    if "jc" in msg:
        parts.append(f"jc={msg['jc']}")
    return " ".join(parts)


def compact_sel(msg):
    keys = ["f", "state", "pv", "diff", "ed", "busy", "sort", "idx"]
    return " ".join(f"{k}={msg[k]}" for k in keys if k in msg)


def compact_result(msg):
    keys = ["ev", "score", "combo", "max_combo", "pct", "life"]
    parts = [f"{k}={msg[k]}" for k in keys if k in msg]
    if "jc" in msg:
        parts.append(f"jc={msg['jc']}")
    return " ".join(parts)


class Client:
    def __init__(self, reader, writer, addr):
        self.reader = reader
        self.writer = writer
        self.addr = addr
        self.cid = None
        self.name = "?"
        self.mod = "?"
        self.connected_at = time.time()
        self.last_pong_rtt = None


class Server:
    def __init__(self, port, disc_port, admin_port=7790, countdown=0.0,
                 cloud=False, name=None):
        self.port = port
        self.disc_port = disc_port
        self.admin_port = admin_port
        self.cloud = cloud            # single public port, no LAN listeners
        self.name = name or socket.gethostname()
        self.clients = {}          # cid -> Client
        self.known_cids = set()    # ever seen
        self.first_mod_version = None
        self.avatars = AvatarStore()
        self._avatar_gate = UploadGate()
        self._relay_gate = RelayGate()
        self._avatar_logged = set()   # (cid, reason) already logged
        self.client_img = {}          # cid -> (module, portrait id)
        self.avatar_relay = True      # --no-avatar-relay: web only
        self.web = None
        self.lobby = LobbyManager(log=lambda m: log(f"LOBBY {m}"),
                                  countdown=countdown)
        self.lobby.avatar_img_of = self.console_img

    # ------------------------------------------------- portraits
    def web_img(self, cid):
        """Portrait id of `cid`'s current module ("" none/hidden)."""
        mod, img = self.client_img.get(cid, (-1, ""))
        if (img and mod == self.lobby._client_modules.get(cid, -1)
                and self.avatars.has(img)):
            return img
        return ""

    def console_img(self, cid):
        return self.web_img(cid) if self.avatar_relay else ""

    def accept_upload(self, cid, msg, now):
        """avatar_img -> (True, log line) once stored, else (False, reason)."""
        module = msg.get("module")
        if not isinstance(module, int) or isinstance(module, bool):
            module = -1
        announced = self.lobby._client_modules.get(cid, -1)
        if (module == announced and
                self.client_img.get(cid, (-1, ""))[0] == module):
            return False, ""       # already have this module's portrait
        ok, reason = self._avatar_gate.check(cid, module, announced, now)
        if not ok:
            return False, reason
        try:
            module, w, h, rgba, png = parse_upload(msg)
        except ValueError as e:
            return False, str(e)
        img = self.avatars.put(rgba, png)
        self.client_img[cid] = (module, img)
        self.lobby.touch_avatar(cid)
        return True, f"module {module} {w}x{h} -> {img}"

    def avatar_get_reply(self, cid, img, now):
        """avatar_get {img}: only for a portrait of someone in the asker's
        lobby, relay on, not hidden, rate limited."""
        if not isinstance(img, str) or not IMG_ID.fullmatch(img):
            return {"t": "avatar_px", "img": "", "err": "bad_img"}

        def no(e):
            return {"t": "avatar_px", "img": img, "err": e}
        if not self.avatar_relay:
            return no("relay_off")
        if not self._relay_gate.allow(cid, now):
            return no("rate_limited")
        if self.avatars.hidden(img):
            return no("hidden")
        lobby = self.lobby._lobby_of(cid)
        if lobby is None or not any(self.console_img(m.cid) == img
                                    for m in lobby.members):
            return no("not_in_lobby")
        return {"t": "avatar_px", "img": img,
                "rgba": base64.b64encode(self.avatars.rgba(img)).decode()}

    def hide_avatar(self, cid, by="admin"):
        """Moderation: never serve or relay `cid`'s current portrait again."""
        img = self.web_img(cid)
        if not img:
            return {"error": "no_portrait"}
        self.avatars.hide(img)
        log(f"AVATAR_HIDDEN {img} of {cid} by {by}")
        self.lobby.touch_avatar(cid)
        return {"ok": True, "img": img}

    def table(self):
        if not self.clients:
            log("  clients: (none)")
            return
        for c in self.clients.values():
            log(f"  client {c.cid} '{c.name}' mod={c.mod} {c.addr[0]}:{c.addr[1]}")

    def _avatar_reject(self, client, reason):
        key = (client.cid, reason)
        if key not in self._avatar_logged:
            self._avatar_logged.add(key)
            log(f"  avatar_img from '{client.name}' rejected: {reason}")

    async def handle(self, reader, writer):
        addr = writer.get_extra_info("peername")
        client = Client(reader, writer, addr)
        try:
            line = await asyncio.wait_for(reader.readline(), timeout=5)
        except asyncio.TimeoutError:
            log(f"CONN {addr[0]}:{addr[1]} no hello within 5s, closing")
            writer.close()
            return
        try:
            hello = json.loads(line)
        except Exception:
            writer.close()
            return
        if hello.get("t") != "hello":
            writer.close()
            return
        if hello.get("proto") != PROTO:
            writer.write((json.dumps({"t": "error", "code": "proto_mismatch",
                                     "msg": f"expected proto {PROTO}"}) + "\n").encode())
            await writer.drain()
            writer.close()
            log(f"CONN {addr[0]}:{addr[1]} proto mismatch -> rejected")
            return

        client.cid = hello.get("cid", "?")
        client.name = hello.get("name", "?")
        client.mod = hello.get("mod", "?")

        # warn only if the newcomer's mod differs from EVERY other
        # currently connected non-bot console; no warning when alone.
        if MOD_VERSION_EXPECTED:
            expected_set = {MOD_VERSION_EXPECTED}
        else:
            expected_set = {c.mod for cid, c in self.clients.items()
                            if cid != client.cid
                            and not c.mod.startswith("bot")}
        warn = None
        if expected_set and client.mod not in expected_set and                 not client.mod.startswith("bot"):
            warn = (f"mod version mismatch: you={client.mod} "
                    f"expected={sorted(expected_set)[0]}")

        rejoin = client.cid in self.known_cids
        old = self.clients.get(client.cid)
        if old is not None:
            try:
                old.writer.close()
            except Exception:
                pass
        self.clients[client.cid] = client
        self.known_cids.add(client.cid)

        writer.write((json.dumps({"t": "welcome", "proto": PROTO, "cid": client.cid,
                                  "rejoin": rejoin, "warn": warn}) + "\n").encode())
        await writer.drain()
        if rejoin:
            log(f"REJOINED {client.cid} '{client.name}' from {addr[0]}:{addr[1]}")
        else:
            log(f"HELLO {client.cid} '{client.name}' mod={client.mod} game={hello.get('game')} from {addr[0]}:{addr[1]}")
        if warn:
            log(f"  warn sent: {warn}")
        self.table()
        self.lobby.on_hello(client.cid, client.name, client.mod, time.time())
        await self.drain_outbox()

        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception:
                    log(f"  bad json from '{client.name}': {line[:80]!r}")
                    continue
                t = msg.get("t")
                if t == "ping":
                    writer.write((json.dumps({"t": "pong", "seq": msg.get("seq"),
                                              "ct": msg.get("ct")}) + "\n").encode())
                    await writer.drain()
                elif t == "dbg_state":
                    log(f"DBG '{client.name}' {compact_dbg(msg)}")
                elif t == "dbg_sel":
                    log(f"SEL '{client.name}' {compact_sel(msg)}")
                elif t == "dbg_sel_init":
                    log(f"SEL '{client.name}' INIT")
                elif t == "dbg_sel_dest":
                    log(f"SEL '{client.name}' DEST")
                elif t == "dbg_result":
                    log(f"RESULT '{client.name}' {compact_result(msg)}")
                elif t in ("set_name", "lobby_create", "lobby_join",
                           "lobby_leave", "kick", "sel", "sel_ack", "start_req",
                           "launch_fail", "loaded", "state", "final",
                           "back_in_sel", "ready", "set_pick_mode", "autoplay",
                           "pause_req", "resume_req", "avatar", "scene"):
                    self.lobby.on_message(client.cid, msg, time.time())
                    await self.drain_outbox()
                elif t == "avatar_img":
                    ok, info = self.accept_upload(client.cid, msg,
                                                  time.time())
                    if ok:
                        log(f"AVATAR_IMG {info} from '{client.name}'")
                        await self.drain_outbox()
                    elif info:
                        self._avatar_reject(client, info)
                elif t == "avatar_get":
                    reply = self.avatar_get_reply(client.cid, msg.get("img"),
                                                  time.time())
                    writer.write((json.dumps(reply) + "\n").encode())
                    await writer.drain()
                elif t == "dbg_pvs":
                    log(f"PVS '{client.name}' i={msg.get('i')} n={msg.get('n')} "
                        f"ids={msg.get('ids')} names={msg.get('names')}")
                elif t == "dbg_jump":
                    log(f"JUMP '{client.name}' {msg}")
                elif t == "dbg_autopause":
                    log(f"AUTOPAUSE '{client.name}' {msg}")
                else:
                    log(f"MSG '{client.name}' {line[:520].decode(errors='replace')}")
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            if self.clients.get(client.cid) is client:
                self.lobby.on_disconnect(client.cid, time.time())
                await self.drain_outbox()
                del self.clients[client.cid]
            log(f"DROPPED {client.cid} '{client.name}'")
            self._avatar_gate.forget(client.cid)
            self._relay_gate.forget(client.cid)
            self._avatar_logged = {k for k in self._avatar_logged
                                   if k[0] != client.cid}
            self.table()
            try:
                writer.close()
            except Exception:
                pass

    async def admin_handle(self, reader, writer):
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    msg = json.loads(line.strip())
                except Exception:
                    writer.write(b"bad json\n")
                    await writer.drain()
                    continue
                cid = msg.get("cid")
                if msg.get("cmd") == "avatar_hide":
                    res = self.hide_avatar(msg.get("target"))
                    await self.drain_outbox()
                    writer.write((json.dumps(res) + "\n").encode())
                    await writer.drain()
                    continue
                if msg.get("cmd") in ("lobbies", "kick", "set_leader"):
                    res = self.lobby.on_admin(msg, time.time())
                    await self.drain_outbox()
                    writer.write((json.dumps(res) + "\n").encode())
                    await writer.drain()
                    continue
                msg.pop("cid", None)
                msg["t"] = "dbg_cmd"
                # no cid: every console; an unknown/offline cid: nobody
                targets = ([self.clients[cid]] if cid in self.clients
                           else [] if cid else list(self.clients.values()))
                n = 0
                for c in targets:
                    try:
                        c.writer.write((json.dumps(msg) + "\n").encode())
                        await c.writer.drain()
                        n += 1
                    except Exception:
                        pass
                log(f"ADMIN {msg.get('cmd')}: forwarded to {n} client(s)")
                writer.write(f"ok {n} clients\n".encode())
                await writer.drain()
        except (ConnectionResetError, ConnectionAbortedError,
                BrokenPipeError):
            pass
        try:
            writer.close()
        except Exception:
            pass

    async def drain_outbox(self):
        if self.web:
            self.web.observe(self.lobby.outbox)
        for cid, m in self.lobby.outbox:
            c = self.clients.get(cid)
            if c is not None:
                try:
                    c.writer.write((json.dumps(m) + "\n").encode())
                    await c.writer.drain()
                except Exception:
                    pass
        self.lobby.outbox.clear()

    async def ticker(self):
        while True:
            await asyncio.sleep(0.1)
            self.lobby.tick(time.time())
            await self.drain_outbox()

    async def run(self):
        loop = asyncio.get_running_loop()
        loop.create_task(self.ticker())
        closers = []
        if not self.cloud:
            transport, _ = await loop.create_datagram_endpoint(
                lambda: DiscoveryProtocol(self),
                local_addr=("0.0.0.0", self.disc_port))
            srv = await asyncio.start_server(self.handle, "0.0.0.0", self.port,
                                            limit=1 << 17)
            closers += [transport, srv]
        try:
            closers.append(await asyncio.start_server(
                self.admin_handle, "127.0.0.1", self.admin_port))
        except OSError as e:
            if not self.cloud:
                raise
            log(f"admin port 127.0.0.1:{self.admin_port} unavailable ({e})")
        if self.web:
            await self.web.start()
        if self.cloud:
            if not (self.web and self.web._httpd):
                raise SystemExit("cloud mode needs the web port (--web-port/$PORT)")
            log(f"mp_server cloud mode: one port :{self.web.port} - web UI, "
                f"/health, browser /ws, consoles on WebSocket /switch "
                f"(no LAN TCP/UDP listeners)")
        else:
            log(f"mp_server listening TCP :{self.port} UDP-discovery :{self.disc_port} admin 127.0.0.1:{self.admin_port}")
            log(f"LAN addresses: {', '.join(netcheck.lan_ipv4()) or 'none'} - "
                f"consoles on the same network find this server automatically "
                f"(UDP {self.disc_port})")
            threading.Thread(target=netcheck.check_firewall, args=(log,),
                             daemon=True).start()
        try:
            await asyncio.Event().wait()
        except asyncio.CancelledError:
            pass
        finally:
            for c in closers:
                c.close()


class DiscoveryProtocol(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server
        self._logged = {}   # source ip -> last log time

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        try:
            msg = json.loads(data.decode(errors="replace").strip())
        except Exception:
            return
        if msg.get("t") != "discover":
            return
        reply = {"t": "server", "proto": PROTO, "port": self.server.port,
                 "name": getattr(self.server, "name", None)
                 or socket.gethostname()}
        self.transport.sendto((json.dumps(reply) + "\n").encode(), addr)
        # a searching console probes several addresses per round
        now = time.time()
        if now - self._logged.get(addr[0], 0) > 30:
            self._logged[addr[0]] = now
            log(f"DISCOVER from {addr[0]}:{addr[1]} -> replied")


def env_flag(name):
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cloud", action="store_true", default=env_flag("MP_CLOUD"),
                    help="single-port cloud mode (Render): only --web-port is "
                         "opened; consoles connect over WebSocket /switch "
                         "(also $MP_CLOUD=1)")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--discovery-port", type=int, default=7778)
    ap.add_argument("--admin-port", type=int, default=7790)
    ap.add_argument("--mod-version-expected", default=None)
    ap.add_argument("--web-port", type=int,
                    default=int(os.environ.get("PORT", 8080)),
                    help="web UI port, also /switch for consoles over "
                         "WebSocket (default $PORT or 8080; 0 disables)")
    ap.add_argument("--web-host", default="0.0.0.0")
    ap.add_argument("--web-token", default=os.environ.get("MP_WEB_TOKEN"),
                    help="admin token (default $MP_WEB_TOKEN, else random "
                         "each start)")
    ap.add_argument("--server-name", default=os.environ.get("MP_SERVER_NAME"),
                    help="name shown in the web UI (default: host name)")
    ap.add_argument("--public-url",
                    default=(os.environ.get("MP_PUBLIC_URL")
                             or os.environ.get("RENDER_EXTERNAL_URL")),
                    help="public base URL printed in the log")
    ap.add_argument("--lobby-max", type=int, default=8,
                    help="max members per lobby (consoles support 8)")
    ap.add_argument("--no-avatar-relay", action="store_true",
                    help="never send uploaded portraits to consoles (the "
                         "web UI still shows them)")
    ap.add_argument("--countdown", type=float, default=3.0,
                    help="seconds of 3-2-1 countdown before each round "
                         "start (0 disables)")
    args = ap.parse_args()

    global MOD_VERSION_EXPECTED
    MOD_VERSION_EXPECTED = args.mod_version_expected

    if args.cloud and not args.web_port:
        ap.error("--cloud needs a web port (--web-port or $PORT)")
    srv = Server(args.port, args.discovery_port, args.admin_port,
                 countdown=args.countdown, cloud=args.cloud,
                 name=args.server_name)
    srv.lobby.max_members = args.lobby_max
    srv.avatar_relay = not args.no_avatar_relay
    if args.web_port:
        srv.web = WebBridge(srv, args.web_host, args.web_port,
                            args.web_token or secrets.token_urlsafe(9))
        srv.web.public_url = (args.public_url or "").rstrip("/") or None
    try:
        asyncio.run(srv.run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
