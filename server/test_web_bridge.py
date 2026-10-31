#!/usr/bin/env python3
import asyncio
import base64
import json
import os
import struct
import tempfile
import unittest
from types import SimpleNamespace

from lobby import LobbyManager
from web_bridge import (WebBridge, WsError, encode_frame, read_frame,
                        ws_accept)


def cframe(op, payload, fin=True):
    """Masked client->server frame."""
    mask = b"\x11\x22\x33\x44"
    n = len(payload)
    b0 = (0x80 if fin else 0) | op
    if n < 126:
        head = struct.pack("!BB", b0, 0x80 | n)
    elif n < 65536:
        head = struct.pack("!BBH", b0, 0x80 | 126, n)
    else:
        head = struct.pack("!BBQ", b0, 0x80 | 127, n)
    return head + mask + bytes(payload[i] ^ mask[i & 3] for i in range(n))


def reader_of(data):
    r = asyncio.StreamReader()
    r.feed_data(data)
    r.feed_eof()
    return r


class FakeConn:
    def __init__(self):
        self.name = None
        self.admin = False
        self.bad_tokens = 0
        self.alive = True
        self.last_sig = None
        self.last_sent = 0.0
        self.last_ping = 0.0
        self.sent = []

    async def send_json(self, obj):
        self.sent.append(obj)

    async def send_raw(self, data):
        pass


class FakeServer:
    def __init__(self):
        self.lobby = LobbyManager()
        self.clients = {}
        self.forwarded = []          # outbox copies drained

    async def drain_outbox(self):
        self.forwarded += list(self.lobby.outbox)
        self.lobby.outbox.clear()


def mk_lobby(lm):
    lm.on_hello("cid_alice_01", "Alice", "v1", 100.0)
    lm.on_hello("cid_bob_0002", "Bob", "v1", 100.0)
    lm.on_hello("cid_carol_03", "Carol", "v1", 100.0)
    lm.on_message("cid_alice_01", {"t": "lobby_create"}, 100.0)
    code = next(iter(lm.lobbies))
    lm.on_message("cid_bob_0002", {"t": "lobby_join", "code": code}, 100.0)
    lm.on_message("cid_carol_03", {"t": "lobby_join", "code": code}, 100.0)
    return code


class Codec(unittest.TestCase):
    def test_accept(self):
        self.assertEqual(ws_accept("dGhlIHNhbXBsZSBub25jZQ=="),
                         "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_encode_lengths(self):
        self.assertEqual(encode_frame(1, b"hello"), b"\x81\x05hello")
        f = encode_frame(1, b"x" * 200)
        self.assertEqual(f[:4], b"\x81\x7e\x00\xc8")
        self.assertEqual(len(f), 204)
        f = encode_frame(1, b"x" * 70000)
        self.assertEqual(f[:2], b"\x81\x7f")
        self.assertEqual(struct.unpack("!Q", f[2:10])[0], 70000)
        self.assertEqual(len(f), 70010)


class CodecIo(unittest.IsolatedAsyncioTestCase):
    async def test_read_masked(self):
        op, data = await read_frame(reader_of(cframe(1, b"hi")))
        self.assertEqual((op, data), (1, b"hi"))

    async def test_read_len126(self):
        op, data = await read_frame(reader_of(cframe(1, b"y" * 200)))
        self.assertEqual((op, len(data)), (1, 200))

    async def test_read_len127(self):
        op, data = await read_frame(reader_of(cframe(1, b"z" * 65536)))
        self.assertEqual((op, len(data)), (1, 65536))

    async def test_continuation(self):
        r = reader_of(cframe(1, b"hel", fin=False) + cframe(0, b"lo"))
        op, data = await read_frame(r)
        self.assertEqual((op, data), (1, b"hello"))

    async def test_too_big(self):
        with self.assertRaises(WsError) as cm:
            await read_frame(reader_of(cframe(1, b"z" * 65537)))
        self.assertEqual(cm.exception.code, 1009)

    async def test_declared_len_too_big(self):
        # header claims a huge payload; rejected before any payload read
        head = bytes([0x81, 0xFF]) + struct.pack("!Q", 1 << 60)
        with self.assertRaises(WsError) as cm:
            await read_frame(reader_of(head))
        self.assertEqual(cm.exception.code, 1009)

    async def test_unmasked_rejected(self):
        with self.assertRaises(WsError) as cm:
            await read_frame(reader_of(b"\x81\x02hi"))
        self.assertEqual(cm.exception.code, 1002)

    async def test_ping(self):
        op, data = await read_frame(reader_of(cframe(9, b"pp")))
        self.assertEqual((op, data), (9, b"pp"))


class Snap(unittest.TestCase):
    def setUp(self):
        self.srv = FakeServer()
        self.lm = self.srv.lobby
        self.web = WebBridge(self.srv, "127.0.0.1", 0, "tok",
                             static_dir="nope")

    def drive_to_playing(self):
        code = mk_lobby(self.lm)
        self.lm.on_message("cid_alice_01",
                           {"t": "sel", "pv": 100, "name": "Song X"}, 101.0)
        self.lm.on_message("cid_alice_01",
                           {"t": "start_req", "pv": 100, "name": "Song X"},
                           101.0)
        l = self.lm.lobbies[code]
        rid = l.round.id
        for c in ("cid_alice_01", "cid_bob_0002", "cid_carol_03"):
            self.lm.on_message(c, {"t": "loaded", "round": rid}, 102.0)
        return code, rid

    def test_snapshot(self):
        code, rid = self.drive_to_playing()
        self.lm.on_message("cid_alice_01",
                           {"t": "state", "round": rid, "score": 12345,
                            "combo": 60, "pct": 95.5, "life": 200}, 103.0)
        self.lm.on_message("cid_bob_0002",
                           {"t": "final", "round": rid, "score": 9000,
                            "pct": 90.0, "max_combo": 42, "jc": [1, 2, 3],
                            "life": 100}, 104.0)
        s = self.web.snapshot()
        self.assertEqual(s["t"], "snapshot")
        self.assertEqual(s["server"]["clients_online"], 0)
        self.assertEqual(len(s["lobbies"]), 1)
        l = s["lobbies"][0]
        self.assertEqual(l["code"], code)
        self.assertEqual(l["phase"], "PLAYING")
        self.assertEqual(l["sel"], {"pv": 100, "name": "Song X"})
        self.assertEqual(len(l["members"]), 3)
        names = {m["name"] for m in l["members"]}
        self.assertEqual(names, {"Alice", "Bob", "Carol"})
        self.assertTrue(all(len(m["mid"]) == 12 for m in l["members"]))
        self.assertEqual(l["round"]["id"], rid)
        self.assertTrue(l["round"]["started"])
        self.assertEqual(len(l["round"]["players"]), 3)
        st = l["standings"]                     # first final -> standings on
        self.assertIsNotNone(st)
        self.assertFalse(st["complete"])
        self.assertEqual(len(st["e"]), 3)
        alice = next(m for m in l["members"] if m["name"] == "Alice")
        self.assertEqual(alice["score"], 12345)
        self.assertEqual(alice["combo"], 60)
        bob_row = next(r for r in st["e"] if r["name"] == "Bob")
        self.assertTrue(bob_row["final"])
        # finish the round -> RESULTS + complete standings
        for c in ("cid_alice_01", "cid_carol_03"):
            self.lm.on_message(c, {"t": "final", "round": rid, "score": 1,
                                   "pct": 1.0, "max_combo": 1, "jc": [],
                                   "life": 1}, 105.0)
        s = self.web.snapshot()
        l = s["lobbies"][0]
        self.assertEqual(l["phase"], "RESULTS")
        self.assertTrue(l["standings"]["complete"])
        # no raw cid anywhere
        blob = json.dumps(s)
        for c in ("cid_alice_01", "cid_bob_0002", "cid_carol_03"):
            self.assertNotIn(c, blob)


class Actions(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.srv = FakeServer()
        self.lm = self.srv.lobby
        self.web = WebBridge(self.srv, "127.0.0.1", 0, "sekret",
                             static_dir="nope")
        self.code = mk_lobby(self.lm)
        self.lm.outbox.clear()
        self.lobby = self.lm.lobbies[self.code]

    def conn(self):
        return FakeConn()

    async def auth(self, conn, name=None, token=None, i=1):
        return await self.web._dispatch(
            conn, {"t": "auth", "id": i, "name": name, "token": token})

    async def action(self, conn, **kw):
        kw.setdefault("id", 9)
        return await self.web._dispatch(conn, dict(t="action", **kw))

    def mid_of(self, cid):
        return self.web.mid(cid)

    async def test_name_login_owns_and_kick(self):
        c = self.conn()
        r = await self.auth(c, name="  ALICE ")
        self.assertTrue(r["ok"])
        self.assertEqual(r["you"]["owns"], [self.code])
        self.assertFalse(r["you"]["admin"])
        r = await self.action(c, op="kick", lobby=self.code,
                              member=self.mid_of("cid_bob_0002"))
        self.assertTrue(r["ok"])
        self.assertNotIn("cid_bob_0002",
                         [m.cid for m in self.lobby.members])
        self.assertFalse([m for _, m in self.srv.forwarded
                          if m.get("t") == "lobby_error"])
        e = self.web.feeds[self.code][-1]
        self.assertEqual(e["kind"], "web_action")
        self.assertEqual(e["op"], "kick")
        self.assertEqual(e["name"], "Bob")

    async def test_non_owner_forbidden(self):
        c = self.conn()
        await self.auth(c, name="Eve")
        r = await self.action(c, op="kick", lobby=self.code,
                              member=self.mid_of("cid_bob_0002"))
        self.assertEqual(r["error"], "forbidden")

    async def test_admin_kicks_owner(self):
        c = self.conn()
        r = await self.auth(c, token="sekret")
        self.assertTrue(r["ok"])
        self.assertTrue(r["you"]["admin"])
        r = await self.action(c, op="kick", lobby=self.code,
                              member=self.mid_of("cid_alice_01"))
        self.assertTrue(r["ok"])
        self.assertNotIn("cid_alice_01",
                         [m.cid for m in self.lobby.members])

    async def test_name_login_cant_kick_owner(self):
        c = self.conn()
        await self.auth(c, name="Alice")
        r = await self.action(c, op="kick", lobby=self.code,
                              member=self.mid_of("cid_alice_01"))
        self.assertEqual(r["error"], "bad_member")

    async def test_bad_token_and_lockout(self):
        c = self.conn()
        for i in range(4):
            r = await self.auth(c, token="nope", i=i + 1)
            self.assertEqual(r["error"], "bad_token")
            self.assertTrue(c.alive)
        await self.auth(c, token="nope", i=9)
        self.assertFalse(c.alive)               # 5 bad tokens -> close

    async def test_logout(self):
        c = self.conn()
        await self.auth(c, name="Alice", token="sekret")
        self.assertTrue(c.admin)
        r = await self.auth(c)                  # both empty -> logout
        self.assertTrue(r["ok"])
        self.assertEqual(r["you"], {"name": None, "admin": False, "owns": []})

    async def test_set_pick_mode(self):
        c = self.conn()
        await self.auth(c, name="Alice")
        r = await self.action(c, op="set_pick_mode", lobby=self.code,
                              mode="bogus")
        self.assertEqual(r["error"], "bad_mode")
        r = await self.action(c, op="set_pick_mode", lobby=self.code,
                              mode="assigned", member="deadbeefdead")
        self.assertEqual(r["error"], "bad_member")
        r = await self.action(c, op="set_pick_mode", lobby=self.code,
                              mode="assigned",
                              member=self.mid_of("cid_bob_0002"))
        self.assertTrue(r["ok"])
        self.assertEqual(self.lobby.pick_mode, "assigned")
        self.assertEqual(self.lobby.assigned_cid, "cid_bob_0002")
        self.assertEqual(self.lobby.leader(), "cid_bob_0002")

    async def test_set_leader(self):
        c = self.conn()
        await self.auth(c, name="Alice")
        r = await self.action(c, op="set_leader", lobby=self.code,
                              member=self.mid_of("cid_carol_03"))
        self.assertTrue(r["ok"])                # rotate is default
        self.assertEqual(self.lobby.leader(), "cid_carol_03")
        # switch to owner mode -> set_leader now bad_mode
        await self.action(c, op="set_pick_mode", lobby=self.code,
                          mode="owner")
        r = await self.action(c, op="set_leader", lobby=self.code,
                              member=self.mid_of("cid_bob_0002"))
        self.assertEqual(r["error"], "bad_mode")

    async def test_bad_lobby_type(self):
        c = self.conn()
        await self.auth(c, token="sekret")
        r = await self.action(c, op="kick", lobby=["x"],
                              member=self.mid_of("cid_bob_0002"))
        self.assertEqual(r["error"], "bad_request")
        r = await self.action(c, op="kick", lobby={"x": 1},
                              member=self.mid_of("cid_bob_0002"))
        self.assertEqual(r["error"], "bad_request")
        # connection still usable
        r = await self.action(c, op="kick", lobby=self.code,
                              member=self.mid_of("cid_bob_0002"))
        self.assertTrue(r["ok"])

    async def test_not_found_and_bad_op(self):
        c = self.conn()
        await self.auth(c, token="sekret")
        r = await self.action(c, op="kick", lobby="ZZZZ", member="x")
        self.assertEqual(r["error"], "not_found")
        r = await self.action(c, op="explode", lobby=self.code)
        self.assertEqual(r["error"], "bad_request")

    async def test_no_lobby_error_on_validation(self):
        c = self.conn()
        await self.auth(c, name="Alice")
        self.lm.outbox.clear()
        await self.action(c, op="kick", lobby=self.code, member="zzz")
        await self.action(c, op="set_pick_mode", lobby=self.code,
                          mode="assigned", member="zzz")
        await self.action(c, op="set_pick_mode", lobby=self.code, mode="x")
        self.assertFalse([m for _, m in self.lm.outbox
                          if m.get("t") == "lobby_error"])


class FeedDedup(unittest.TestCase):
    def test_observe_dedup(self):
        srv = FakeServer()
        lm = srv.lobby
        web = WebBridge(srv, "127.0.0.1", 0, "tok", static_dir="nope")
        code = mk_lobby(lm)
        lm.outbox.clear()
        l = lm.lobbies[code]
        lm._event(l, "joined", "cid_bob_0002", "Bob")   # broadcast to 3
        self.assertEqual(len(lm.outbox), 3)
        web.observe(lm.outbox)
        self.assertEqual(len(web.feeds[code]), 1)
        self.assertEqual(web.feeds[code][0]["kind"], "event")
        self.assertEqual(web.feeds[code][0]["ev"], "joined")
        web.observe(lm.outbox)                          # same outbox again
        self.assertEqual(len(web.feeds[code]), 1)
        # results announced once per round
        lm.outbox.clear()
        lm._send(l.members[0].cid,
                 {"t": "standings", "round": 1, "complete": True, "e": []})
        lm._send(l.members[0].cid,
                 {"t": "standings", "round": 1, "complete": True, "e": []})
        web.observe(lm.outbox)
        self.assertEqual([e["kind"] for e in web.feeds[code]],
                         ["event", "results"])


class Integration(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.mkdtemp()
        with open(os.path.join(self.tmp, "index.html"), "w") as f:
            f.write("<h1>MegaMix UI</h1>")
        os.makedirs(os.path.join(self.tmp, "assets"))
        with open(os.path.join(self.tmp, "assets", "app.js"), "w") as f:
            f.write("console.log(1)")
        self.srv = FakeServer()
        self.web = WebBridge(self.srv, "127.0.0.1", 0, "testtoken",
                             static_dir=self.tmp)
        await self.web.start()
        self.port = self.web.port
        self._wsbufs = {}

    async def asyncTearDown(self):
        self.web._httpd.close()
        self.web._task.cancel()
        try:
            await self.web._task
        except asyncio.CancelledError:
            pass

    async def http(self, path, headers=None, method="GET", port=None):
        r, w = await asyncio.open_connection("127.0.0.1",
                                             port or self.port)
        h = (f"{method} {path} HTTP/1.1\r\n"
             f"Host: 127.0.0.1:{port or self.port}\r\n")
        for k, v in (headers or {}).items():
            h += f"{k}: {v}\r\n"
        w.write((h + "\r\n").encode())
        await w.drain()
        data = await r.read()
        w.close()
        head, _, body = data.partition(b"\r\n\r\n")
        return head.decode("latin-1"), body

    async def test_http_routes(self):
        head, body = await self.http("/api/health")
        self.assertIn("200", head)
        self.assertEqual(json.loads(body), {"ok": True})
        head, body = await self.http("/api/state")
        self.assertIn("200", head)
        s = json.loads(body)
        self.assertEqual(s["t"], "snapshot")
        self.assertEqual(s["you"], {"name": None, "admin": False, "owns": []})
        head, body = await self.http("/")
        self.assertIn("200", head)
        self.assertIn(b"MegaMix UI", body)
        self.assertIn("no-cache", head)
        head, body = await self.http("/lobby/ABCD")
        self.assertIn("200", head)                       # SPA fallback
        self.assertIn(b"MegaMix UI", body)
        head, _ = await self.http("/../mp_server.py")
        self.assertIn("404", head)
        head, _ = await self.http("/%2e%2e/mp_server.py")
        self.assertIn("404", head)
        head, _ = await self.http("/%2e%2e%2f..%2fmp_server.py")
        self.assertIn("404", head)
        head, body = await self.http("/assets/app.js")
        self.assertIn("200", head)
        self.assertIn("text/javascript", head)
        self.assertIn("immutable", head)
        head, body = await self.http("/", method="HEAD")
        self.assertIn("200", head)
        self.assertEqual(body, b"")
        head, _ = await self.http("/", method="POST")
        self.assertIn("405", head)
        head, _ = await self.http("/api/nope")
        self.assertIn("404", head)

    async def test_bind_failure(self):
        import socket as sk
        s = sk.socket()
        s.bind(("127.0.0.1", 0))
        s.listen()
        port = s.getsockname()[1]
        web = WebBridge(self.srv, "127.0.0.1", port, "t",
                        static_dir=self.tmp)
        await web.start()                        # must not raise
        self.assertIsNone(web._httpd)
        self.assertIsNone(web._task)
        s.close()

    async def test_missing_dist(self):
        web = WebBridge(self.srv, "127.0.0.1", 0, "t",
                        static_dir=self.tmp + "_gone")
        await web.start()
        try:
            head, _ = await self.http("/", port=web.port)
            self.assertIn("503", head)
        finally:
            web._httpd.close()
            web._task.cancel()

    async def ws_open(self, headers=None):
        r, w = await asyncio.open_connection("127.0.0.1", self.port)
        key = base64.b64encode(os.urandom(16)).decode()
        h = (f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n")
        for k, v in (headers or {}).items():
            h += f"{k}: {v}\r\n"
        w.write((h + "\r\n").encode())
        await w.drain()
        head = await r.readuntil(b"\r\n\r\n")
        return r, w, head.decode("latin-1"), key

    async def ws_msg(self, r, want=None, pred=None):
        buf = self._wsbufs.setdefault(r, [])

        def match(m):
            return ((want is None or m.get("t") == want)
                    and (pred is None or pred(m)))

        for i, m in enumerate(buf):
            if match(m):
                return buf.pop(i)
        for _ in range(60):
            op, data = await asyncio.wait_for(
                read_frame(r, masked=False), 10)
            if op != 1:
                continue
            m = json.loads(data)
            buf.append(m)
            if match(m):
                return buf.pop()
        raise AssertionError("no matching ws message")

    async def ws_send(self, w, obj):
        w.write(cframe(1, json.dumps(obj).encode()))
        await w.drain()

    async def test_ws_flow(self):
        # a lobby exists before the ws client connects
        code = mk_lobby(self.srv.lobby)
        self.web.observe(self.srv.lobby.outbox)
        self.srv.lobby.outbox.clear()

        r, w, head, key = await self.ws_open()
        self.assertIn("101", head)
        self.assertIn(ws_accept(key), head)
        hello = await self.ws_msg(r, "hello")
        self.assertIn(code, hello["feed"])
        snap = await self.ws_msg(r, "snapshot")
        self.assertEqual(snap["lobbies"][0]["code"], code)
        self.assertEqual(snap["you"]["admin"], False)

        # bad token
        await self.ws_send(w, {"t": "auth", "id": 1, "token": "nope"})
        rep = await self.ws_msg(r, "reply", lambda m: m["id"] == 1)
        self.assertEqual(rep["error"], "bad_token")

        # name login as owner (case-insensitive) -> owns
        await self.ws_send(w, {"t": "auth", "id": 2, "name": "alice"})
        rep = await self.ws_msg(r, "reply", lambda m: m["id"] == 2)
        self.assertTrue(rep["ok"])
        self.assertEqual(rep["you"]["owns"], [code])

        # kick bob -> ok, then a pushed snapshot shows 2 members
        bob_mid = self.web.mid("cid_bob_0002")
        await self.ws_send(w, {"t": "action", "id": 3, "op": "kick",
                               "lobby": code, "member": bob_mid})
        rep = await self.ws_msg(r, "reply", lambda m: m["id"] == 3)
        self.assertTrue(rep["ok"])
        snap = await self.ws_msg(
            r, "snapshot",
            lambda m: len(m["lobbies"][0]["members"]) == 2)
        self.assertEqual(
            {x["name"] for x in snap["lobbies"][0]["members"]},
            {"Alice", "Carol"})

        # token login -> admin
        await self.ws_send(w, {"t": "auth", "id": 4, "token": "testtoken"})
        rep = await self.ws_msg(r, "reply", lambda m: m["id"] == 4)
        self.assertTrue(rep["you"]["admin"])
        w.close()

    async def test_ws_bad_origin(self):
        r, w, head, _ = await self.ws_open(
            {"Origin": "http://evil.example.com"})
        self.assertIn("403", head)
        w.close()

    async def test_ws_same_origin_ok(self):
        r, w, head, _ = await self.ws_open(
            {"Origin": f"http://127.0.0.1:{self.port}"})
        self.assertIn("101", head)
        w.close()


if __name__ == "__main__":
    unittest.main()
