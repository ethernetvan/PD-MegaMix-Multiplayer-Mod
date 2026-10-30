#!/usr/bin/env python3
"""mp_server.py - MegaMix LAN multiplayer server (milestone 1: transport + dbg telemetry).

Protocol v1 (NDJSON, one object per line):
  C->S hello/ping/dbg_state ; S->C welcome/error/pong
UDP discovery on --discovery-port: {"t":"discover"} -> unicast {"t":"server",...}
"""

import argparse
import asyncio
import json
import socket
import sys
import time

from lobby import LobbyManager

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
            "ps", "ip", "iv", "pe", "res"]
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
    def __init__(self, port, disc_port, admin_port=7790):
        self.port = port
        self.disc_port = disc_port
        self.admin_port = admin_port
        self.clients = {}          # cid -> Client
        self.known_cids = set()    # ever seen
        self.first_mod_version = None
        self.lobby = LobbyManager(log=lambda m: log(f"LOBBY {m}"))

    def table(self):
        if not self.clients:
            log("  clients: (none)")
            return
        for c in self.clients.values():
            log(f"  client {c.cid} '{c.name}' mod={c.mod} {c.addr[0]}:{c.addr[1]}")

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

        if self.first_mod_version is None:
            self.first_mod_version = client.mod
        expected = MOD_VERSION_EXPECTED or self.first_mod_version
        warn = None
        if client.mod != expected:
            warn = f"mod version mismatch: you={client.mod} expected={expected}"

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
                           "lobby_leave", "sel", "sel_ack", "start_req",
                           "launch_fail", "loaded", "state", "final",
                           "back_in_sel", "ready"):
                    self.lobby.on_message(client.cid, msg, time.time())
                    await self.drain_outbox()
                elif t == "dbg_pvs":
                    log(f"PVS '{client.name}' i={msg.get('i')} n={msg.get('n')} "
                        f"ids={msg.get('ids')} names={msg.get('names')}")
                elif t == "dbg_jump":
                    log(f"JUMP '{client.name}' {msg}")
                elif t == "dbg_autopause":
                    log(f"AUTOPAUSE '{client.name}' {msg}")
                else:
                    log(f"MSG '{client.name}' {line[:200].decode(errors='replace')}")
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            if self.clients.get(client.cid) is client:
                self.lobby.on_disconnect(client.cid, time.time())
                await self.drain_outbox()
                del self.clients[client.cid]
            log(f"DROPPED {client.cid} '{client.name}'")
            self.table()
            try:
                writer.close()
            except Exception:
                pass

    async def admin_handle(self, reader, writer):
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
            cid = msg.pop("cid", None)
            if msg.get("cmd") in ("lobbies", "kick", "set_leader"):
                res = self.lobby.on_admin(msg, time.time())
                await self.drain_outbox()
                writer.write((json.dumps(res) + "\n").encode())
                await writer.drain()
                continue
            msg["t"] = "dbg_cmd"
            targets = ([self.clients[cid]] if cid in self.clients
                       else list(self.clients.values()))
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
        try:
            writer.close()
        except Exception:
            pass

    async def drain_outbox(self):
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
        asyncio.get_running_loop().create_task(self.ticker())
        transport, _ = await loop.create_datagram_endpoint(
            lambda: DiscoveryProtocol(self), local_addr=("0.0.0.0", self.disc_port))
        srv = await asyncio.start_server(self.handle, "0.0.0.0", self.port)
        adm = await asyncio.start_server(self.admin_handle, "127.0.0.1",
                                         self.admin_port)
        log(f"mp_server listening TCP :{self.port} UDP-discovery :{self.disc_port} admin 127.0.0.1:{self.admin_port}")
        try:
            await srv.serve_forever()
        except asyncio.CancelledError:
            pass
        finally:
            transport.close()
            adm.close()


class DiscoveryProtocol(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server

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
                 "name": socket.gethostname()}
        self.transport.sendto((json.dumps(reply) + "\n").encode(), addr)
        log(f"DISCOVER from {addr[0]}:{addr[1]} -> replied")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--discovery-port", type=int, default=7778)
    ap.add_argument("--admin-port", type=int, default=7790)
    ap.add_argument("--mod-version-expected", default=None)
    args = ap.parse_args()

    global MOD_VERSION_EXPECTED
    MOD_VERSION_EXPECTED = args.mod_version_expected

    srv = Server(args.port, args.discovery_port, args.admin_port)
    try:
        asyncio.run(srv.run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
