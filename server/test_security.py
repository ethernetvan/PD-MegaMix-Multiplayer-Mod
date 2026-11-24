#!/usr/bin/env python3
"""Hostile-input tests: what a client (or anyone who can reach a port) sends
must never stop the server, another lobby, or another client's session."""
import asyncio
import json
import os
import tempfile
import unittest

import mp_server
import web_bridge
from lobby import (LobbyManager, MAX_NAME, MAX_REASON, MAX_SESSION_KEPT,
                   MAX_SONG)
from mp_server import DiscoveryProtocol, Server, parse_msg
from test_web_bridge import FakeConn, FakeServer, mk_lobby
from web_bridge import WebBridge

NAN = float("nan")
INF = float("inf")


def playing_lobby(lm, cids=("A", "B")):
    """A lobby in PLAYING with every cid a loaded player; returns (code, round)."""
    for c in cids:
        lm.on_hello(c, c.lower(), "v1", 100.0)
    lm.on_message(cids[0], {"t": "lobby_create"}, 100.0)
    code = next(reversed(lm.lobbies))
    for c in cids[1:]:
        lm.on_message(c, {"t": "lobby_join", "code": code}, 100.0)
    lm.on_message(cids[0], {"t": "sel", "pv": 1, "name": "song"}, 100.0)
    lm.on_message(cids[0], {"t": "start_req", "pv": 1, "name": "song"}, 100.0)
    rid = lm.lobbies[code].round.id
    for c in cids:
        lm.on_message(c, {"t": "loaded", "round": rid}, 100.0)
    assert lm.lobbies[code].phase == "PLAYING"
    return code, rid


def all_json(lm):
    """Everything queued for clients must be strict JSON."""
    for _, m in lm.outbox:
        json.dumps(m, allow_nan=False)


class EngineInput(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()

    def test_poisoned_state_cannot_stop_the_ticker(self):
        code, rid = playing_lobby(self.lm)
        for bad in ("x", None, [], {"a": 1}, NAN, INF, -INF, True, 1e400):
            self.lm.on_message("A", {"t": "state", "round": rid, "score": bad,
                                     "combo": bad, "pct": bad, "life": bad,
                                     "border": bad}, 101.0)
            self.lm.tick(102.0)                      # sorts the board
            st = self.lm.lobbies[code].members[0].state
            for k in ("score", "combo", "pct", "life", "border"):
                self.assertIsInstance(st[k], (int, float), (k, bad))
                self.assertFalse(isinstance(st[k], bool), (k, bad))
        all_json(self.lm)

    def test_poisoned_final_cannot_break_standings(self):
        code, rid = playing_lobby(self.lm)
        self.lm.on_message("A", {"t": "final", "round": rid, "score": "9",
                                 "pct": NAN, "max_combo": [1], "life": {},
                                 "border": INF, "jc": "nope"}, 101.0)
        self.lm.on_message("B", {"t": "final", "round": rid, "score": 10 ** 30,
                                 "pct": 1e9, "max_combo": -5, "life": 999,
                                 "jc": [1, 2, 3, 4, 5]}, 101.0)
        self.lm.tick(102.0)
        rows, complete = self.lm._standings(self.lm.lobbies[code], 102.0)
        self.assertTrue(complete)
        self.assertEqual([r["cid"] for r in rows], ["B", "A"])
        self.assertLessEqual(rows[0]["score"], 2000000000)
        self.assertLessEqual(rows[0]["pct"], 200.0)
        self.assertEqual(rows[0]["life"], 255)
        all_json(self.lm)

    def test_names_are_bounded_text(self):
        self.lm.on_hello("A", {"evil": 1}, "v1", 100.0)
        self.lm.on_message("A", {"t": "lobby_create"}, 100.0)
        m = next(iter(self.lm.lobbies.values())).members[0]
        self.assertIsInstance(m.name, str)
        for bad in ({"a": ["x"]}, ["x"], 7, None, ""):
            self.lm.on_message("A", {"t": "set_name", "name": bad}, 100.0)
            self.assertIsInstance(m.name, str)
        self.lm.on_message("A", {"t": "set_name", "name": "N" * 200000}, 100.0)
        self.assertEqual(len(m.name), MAX_NAME)
        self.lm.on_message("A", {"t": "set_name",
                                 "name": "a\nb\x00c\x1b[31m\u2028d"}, 100.0)
        self.assertEqual(m.name, "abc[31md")

    def test_selection_fields_are_typed(self):
        self.lm.on_hello("A", "a", "v1", 100.0)
        self.lm.on_hello("B", "b", "v1", 100.0)
        self.lm.on_message("A", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        lobby = self.lm.lobbies[code]
        self.lm.on_message("B", {"t": "lobby_join", "code": code}, 100.0)
        self.lm.on_message("A", {"t": "sel", "pv": {"x": 1}, "name": "s"}, 100.0)
        self.assertIsNone(lobby.sel_pv)
        self.lm.on_message("A", {"t": "sel", "pv": 5, "name": "S" * 5000}, 100.0)
        self.assertEqual((lobby.sel_pv, len(lobby.sel_name)), (5, MAX_SONG))
        # a follower's junk ack used to raise inside the LEADER's start_req
        self.lm.on_message("B", {"t": "sel_ack", "pv": 5, "has": False,
                                 "ok": False, "fb_pv": "zzz",
                                 "fb_name": ["x"] * 9}, 100.0)
        self.lm.on_message("B", {"t": "ready", "ready": False,
                                 "reason": "r" * 9999, "pv": [1]}, 100.0)
        self.assertEqual(len(lobby.members[1].ready_reason), MAX_REASON)
        self.lm.on_message("A", {"t": "start_req", "pv": "x",
                                 "name": 123}, 100.0)
        self.assertEqual(lobby.phase, "LOADING")
        self.assertEqual((lobby.round.pv, lobby.round.name), (5, "S" * MAX_SONG))
        all_json(self.lm)

    def test_join_code_of_any_type(self):
        self.lm.on_hello("A", "a", "v1", 100.0)
        for code in (["AAAA"], {"a": 1}, 5, None, "A" * 100000):
            self.lm.on_message("A", {"t": "lobby_join", "code": code}, 100.0)
        errs = [m for c, m in self.lm.outbox if m.get("t") == "lobby_error"]
        self.assertEqual([e["code"] for e in errs], ["not_found"] * 5)

    def test_room_count_is_capped(self):
        self.lm.max_lobbies = 3
        for i in range(5):
            self.lm.on_hello(f"c{i}", "n", "v1", 100.0)
            self.lm.on_message(f"c{i}", {"t": "lobby_create"}, 100.0)
        self.assertEqual(len(self.lm.lobbies), 3)
        full = [m for c, m in self.lm.outbox if m.get("code") == "server_full"]
        self.assertEqual(len(full), 2)

    def test_one_lobbys_error_leaves_the_others_ticking(self):
        a, _ = playing_lobby(self.lm, ("A", "B"))
        b, rid = playing_lobby(self.lm, ("C", "D"))
        self.lm.lobbies[a].members[0].state["pct"] = "boom"   # as if unvalidated
        self.lm.lobbies[a].board_dirty = True
        self.lm.on_message("C", {"t": "state", "round": rid, "score": 5,
                                 "combo": 1, "pct": 1.5, "life": 100}, 101.0)
        self.lm.outbox.clear()
        self.lm.tick(102.0)
        self.assertTrue(any(m.get("t") == "board" and c == "D"
                            for c, m in self.lm.outbox))

    def test_gone_clients_are_forgotten(self):
        self.lm.on_hello("A", "a", "v1", 100.0)
        self.lm.on_message("A", {"t": "avatar", "module": 3}, 100.0)
        self.lm.on_message("A", {"t": "diff", "d": 2}, 100.0)
        self.lm.on_message("A", {"t": "lobby_create"}, 100.0)
        self.lm.on_disconnect("A", 100.0)
        self.lm.forget("A")                       # still a member: kept
        self.assertIn("A", self.lm._client_names)
        self.lm.tick(100.0 + 61.0)                # removed after 60 s offline
        self.assertEqual(self.lm.lobbies, {})
        for d in (self.lm._client_names, self.lm._client_modules,
                  self.lm._client_avatar, self.lm._client_plat,
                  self.lm._client_diff, self.lm.by_cid):
            self.assertNotIn("A", d)

    def test_leavers_kept_per_lobby_are_capped(self):
        self.lm.max_members = 500
        self.lm.on_hello("own", "o", "v1", 100.0)
        self.lm.on_message("own", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        for i in range(MAX_SESSION_KEPT + 40):
            self.lm.on_hello(f"g{i}", "g", "v1", 100.0)
            self.lm.on_message(f"g{i}", {"t": "lobby_join", "code": code}, 100.0)
            self.lm.on_message(f"g{i}", {"t": "lobby_leave"}, 100.0)
        self.assertEqual(len(self.lm.lobbies[code].session_kept),
                         MAX_SESSION_KEPT)


class W:
    """StreamWriter stand-in."""
    def __init__(self, backlog=0, hang=False, peer="127.0.0.1"):
        self.peer = (peer, 4242)
        self.data = b""
        self.closed = self.aborted = False
        self._backlog = backlog
        self._hang = hang
        self.transport = self

    def write(self, b):
        self.data += b

    async def drain(self):
        if self._hang:
            await asyncio.Event().wait()         # a peer that never reads

    def close(self):
        self.closed = True

    def abort(self):
        self.aborted = True

    def get_write_buffer_size(self):
        return self._backlog

    def get_extra_info(self, name, default=None):
        return self.peer if name == "peername" else default

    def msgs(self):
        return [json.loads(x) for x in self.data.split(b"\n") if x.strip()]


def feed(*lines):
    r = asyncio.StreamReader(limit=1 << 17)
    for ln in lines:
        r.feed_data((ln if isinstance(ln, bytes) else
                     json.dumps(ln).encode()) + b"\n")
    r.feed_eof()
    return r


def hello(**kw):
    h = {"t": "hello", "proto": 1, "cid": "c1", "name": "Miku", "mod": "m1"}
    h.update(kw)
    return {k: v for k, v in h.items() if v is not ...}


class Transport(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        mp_server.log = lambda m: None
        self.srv = Server(0, 0)

    async def session(self, *lines, writer=None):
        w = writer or W()
        await asyncio.wait_for(self.srv.handle(feed(*lines), w), 5)
        return w

    def test_parse_msg(self):
        self.assertEqual(parse_msg(b'{"t":"x"}'), {"t": "x"})
        for bad in (b"[1]", b"1", b'"s"', b"null", b"{", b"",
                    b'{"pct":NaN}', b'{"pct":Infinity}', b'{"p":-Infinity}'):
            self.assertIsNone(parse_msg(bad), bad)

    async def test_wrong_typed_hello_fields_do_not_poison_later_hellos(self):
        # mod=123 used to make every LATER hello raise in the version check
        w = W()
        r = asyncio.StreamReader()
        r.feed_data(json.dumps(hello(cid="first", mod=123, name=[1])).encode()
                    + b"\n")
        task = asyncio.create_task(self.srv.handle(r, w))   # stays connected
        for _ in range(100):
            if "first" in self.srv.clients:
                break
            await asyncio.sleep(0.01)
        c = self.srv.clients["first"]
        self.assertEqual((c.mod, c.name), ("?", "?"))
        w2 = await self.session(hello(cid="second"))
        self.assertEqual(w2.msgs()[0]["t"], "welcome")
        r.feed_eof()
        await asyncio.wait_for(task, 5)

    async def test_bad_ids_are_refused(self):
        for cid in (["a"], {"a": 1}, 5, None, "", "x" * 25, "has space",
                    'q"uote', "a\nb", ...):
            w = await self.session(hello(cid=cid))
            self.assertEqual(w.msgs()[0].get("code"), "bad_hello", cid)
            self.assertTrue(w.closed)
        for key in ("short", 5, "bad key with spaces!", "k" * 65):
            w = await self.session(hello(key=key))
            self.assertEqual(w.msgs()[0].get("code"), "bad_hello", key)
        self.assertEqual(self.srv.clients, {})
        self.assertEqual(self.srv.known_cids, set())
        self.assertEqual(self.srv._cid_keys, {})

    async def live(self, **kw):
        """A session that stays connected; returns (writer, close())."""
        w, r = W(), asyncio.StreamReader()
        r.feed_data(json.dumps(hello(**kw)).encode() + b"\n")
        task = asyncio.create_task(self.srv.handle(r, w))
        for _ in range(200):
            if w.data or task.done():
                break
            await asyncio.sleep(0.005)

        async def close():
            r.feed_eof()
            await asyncio.wait_for(task, 5)
        return w, r, close

    async def test_key_ties_a_cid_to_its_console(self):
        key = "0123456789abcdef0123456789abcdef"
        w, r, close = await self.live(cid="victim", key=key)
        self.assertEqual(w.msgs()[0], {"t": "welcome", "proto": 1,
                                       "cid": "victim", "rejoin": False,
                                       "warn": None})
        r.feed_data(b'{"t":"lobby_create"}\n')
        for _ in range(200):
            if self.srv.lobby.lobbies:
                break
            await asyncio.sleep(0.005)
        self.assertNotIn(key, w.data.decode())          # the key never leaves
        # another console (or an old client) saying hello as that cid: refused,
        # and the victim's session is untouched
        for other in ({"key": key[::-1]}, {}, {"key": key + "x"}):
            bad = await self.session(hello(cid="victim", **other))
            self.assertEqual(bad.msgs()[0].get("code"), "cid_in_use", other)
            self.assertTrue(bad.closed)
        self.assertIs(self.srv.clients["victim"].writer, w)
        self.assertFalse(w.closed)
        # the console itself reconnecting (old socket still open): accepted
        w2, r2, close2 = await self.live(cid="victim", key=key)
        self.assertTrue(w2.msgs()[0]["rejoin"])
        self.assertTrue(w.closed)
        await close()
        # offline but still a room member: the seat can't be taken either
        await close2()
        self.assertIn("victim", self.srv.lobby.by_cid)
        bad = await self.session(hello(cid="victim", key=key[::-1]))
        self.assertEqual(bad.msgs()[0].get("code"), "cid_in_use")

    async def test_key_is_kept_past_the_owner_rejoin_window(self):
        key = "k" * 32
        await self.session(hello(cid="own", key=key))        # came and went
        now = mp_server.time.time()
        self.assertFalse(self.srv.claim_cid("own", "z" * 32, now + 121))
        self.assertFalse(self.srv.claim_cid("own", None, now + 121))
        self.assertTrue(self.srv.claim_cid("own", key, now + 121))
        self.srv._forget("own")
        self.assertTrue(self.srv.claim_cid("own", "z" * 32,
                                           mp_server.time.time()
                                           + mp_server.KEY_KEPT_S + 1))

    async def test_old_clients_keep_working_without_a_key(self):
        w = await self.session(hello(cid="legacy"))
        self.assertEqual(w.msgs()[0]["t"], "welcome")
        w = await self.session(hello(cid="legacy"))
        self.assertTrue(w.msgs()[0]["t"] == "welcome")
        self.assertEqual(self.srv._cid_keys, {})

    async def test_key_table_is_bounded(self):
        mp_server.MAX_KEYS, keep = 5, mp_server.MAX_KEYS
        try:
            for i in range(12):
                await self.session(hello(cid=f"c{i}", key="k" * 20))
            self.assertLessEqual(len(self.srv._cid_keys), 5)
        finally:
            mp_server.MAX_KEYS = keep

    async def test_junk_lines_cost_nothing_but_themselves(self):
        w = await self.session(hello(), b"[1,2]", b"7", b'"str"', b"null",
                               b'{"t":"state","pct":NaN}', b"{bad",
                               {"t": ["list"]}, {"t": "lobby_join", "code": [1]},
                               {"t": "ping", "seq": 9})
        self.assertEqual(w.msgs()[-1], {"t": "pong", "seq": 9, "ct": None})

    async def test_engine_error_keeps_the_connection(self):
        def boom(cid, msg, now):
            if msg.get("t") == "scene":
                raise RuntimeError("engine bug")
            return orig(cid, msg, now)
        orig, self.srv.lobby.on_message = self.srv.lobby.on_message, boom
        w = await self.session(hello(), {"t": "scene", "where": "sel"},
                               {"t": "lobby_create"})
        self.assertEqual(w.msgs()[-1]["t"], "lobby")

    async def test_overlong_line_ends_only_that_session(self):
        w = await self.session(hello(), b"x" * (1 << 18))
        self.assertTrue(w.closed)
        self.assertEqual(self.srv.clients, {})

    async def test_a_client_that_never_reads_blocks_nobody(self):
        slow, ok = W(hang=True), W()
        self.srv.clients = {"slow": mp_server.Client(None, slow, ("a", 1)),
                            "ok": mp_server.Client(None, ok, ("b", 2))}
        self.srv.lobby.outbox += [("slow", {"t": "x"}), ("ok", {"t": "y"})]
        await asyncio.wait_for(self.srv.drain_outbox(), 1)   # used to hang
        self.assertEqual(ok.msgs(), [{"t": "y"}])
        self.assertEqual(self.srv.lobby.outbox, [])

    async def test_backlogged_client_is_cut_off(self):
        w = W(backlog=mp_server.MAX_CLIENT_BACKLOG + 1)
        c = mp_server.Client(None, w, ("a", 1))
        c.cid, c.name = "big", "n"
        self.srv.send(c, {"t": "x"})
        self.assertTrue(w.aborted)
        w = W(backlog=1000)
        self.srv.send(mp_server.Client(None, w, ("a", 1)), {"t": "x"})
        self.assertFalse(w.aborted)

    async def test_outbox_batches_are_not_shared_between_callers(self):
        seen = []

        class Web:
            def observe(self, out):
                seen.append(list(out))
        self.srv.web = Web()
        self.srv.lobby.outbox.append(("nobody", {"t": "a"}))
        await asyncio.gather(self.srv.drain_outbox(), self.srv.drain_outbox())
        self.assertEqual(seen, [[("nobody", {"t": "a"})]])

    async def test_ticker_survives_an_engine_error(self):
        calls = []

        def tick(now):
            calls.append(now)
            if len(calls) == 1:
                raise RuntimeError("tick bug")
        self.srv.lobby.tick = tick
        t = asyncio.create_task(self.srv.ticker())
        await asyncio.sleep(0.45)
        t.cancel()
        self.assertGreaterEqual(len(calls), 2)

    async def test_disconnect_forgets_a_client_without_a_room(self):
        await self.session(hello(cid="gone"), {"t": "avatar", "module": 2})
        self.assertNotIn("gone", self.srv.known_cids)
        self.assertNotIn("gone", self.srv.lobby._client_names)
        self.assertNotIn("gone", self.srv.lobby._client_modules)

    def test_discovery_ignores_junk(self):
        sent = []

        class T:
            def sendto(self, data, addr):
                sent.append((data, addr))
        p = DiscoveryProtocol(self.srv)
        p.connection_made(T())
        for junk in (b"[1]", b"7", b"null", b'{"t":NaN}', b"\xff\xfe",
                     b'{"t":"discover","pad":"' + b"a" * 600 + b'"}'):
            p.datagram_received(junk, ("203.0.113.5", 9))     # must not raise
        self.assertEqual(sent, [])
        p.datagram_received(b'{"t":"discover","proto":1}', ("10.0.0.9", 9))
        self.assertEqual(json.loads(sent[0][0])["t"], "server")


class Web(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.logs = []
        web_bridge.log = self.logs.append
        self.tmp = tempfile.mkdtemp()
        with open(os.path.join(self.tmp, "index.html"), "w") as f:
            f.write("<h1>ui</h1>")
        self.srv = FakeServer()
        self.web = WebBridge(self.srv, "127.0.0.1", 0, "s3cret-admin-token",
                             static_dir=self.tmp)

    async def start(self):
        await self.web.start()
        self.addAsyncCleanup(self.stop)

    async def stop(self):
        self.web._httpd.close()
        self.web._task.cancel()

    async def http(self, target):
        r, w = await asyncio.open_connection("127.0.0.1", self.web.port)
        w.write(f"GET {target} HTTP/1.1\r\nHost: x\r\n\r\n".encode("latin-1"))
        await w.drain()
        data = await asyncio.wait_for(r.read(), 5)
        w.close()
        head, _, body = data.partition(b"\r\n\r\n")
        return head.decode("latin-1").lower(), body

    async def test_static_never_asks_the_os_about_foreign_paths(self):
        await self.start()
        asked = []
        real = os.path.realpath

        def spy(p, *a, **k):
            asked.append(p)
            return real(p, *a, **k)
        os.path.realpath = spy
        try:
            bs = "%5C"
            for target in (f"/{bs}{bs}127.0.0.1{bs}share{bs}x",   # UNC -> SMB
                           "/\\\\127.0.0.1\\share\\x",
                           "/C:/Windows/win.ini", "/c%3A/x", "/a%00b",
                           "/..%2F..%2Fsecret", f"/..{bs}..{bs}secret",
                           "/assets/..%2F..%2Fx", "/index.html::$DATA",
                           "/con.", "/x%20"):
                head, _ = await self.http(target)
                self.assertIn(" 404 ", head.split("\r\n")[0] + " ", target)
        finally:
            os.path.realpath = real
        root = real(self.tmp)
        self.assertTrue(all(p == self.tmp or p == root or
                            p.startswith(root + os.sep) for p in asked), asked)
        head, body = await self.http("/index.html")
        self.assertIn(" 200 ", head.split("\r\n")[0] + " ")
        self.assertEqual(body, b"<h1>ui</h1>")
        head, body = await self.http("/lobby/ABCD")           # SPA fallback
        self.assertEqual(body, b"<h1>ui</h1>")

    async def test_security_headers(self):
        await self.start()
        head, _ = await self.http("/")
        for h in ("x-content-type-options: nosniff", "x-frame-options: deny",
                  "referrer-policy: no-referrer",
                  "content-security-policy: default-src 'self'",
                  "frame-ancestors 'none'"):
            self.assertIn(h, head)
        head, _ = await self.http("/api/state")
        self.assertIn("cache-control: no-store", head)
        self.assertIn("x-content-type-options: nosniff", head)
        self.assertEqual(head.count("x-content-type-options"), 1)

    async def test_name_login_can_be_switched_off(self):
        code = mk_lobby(self.srv.lobby)
        conn = FakeConn()
        r = await self.web._auth(conn, {"t": "auth", "id": 1, "name": "alice"})
        self.assertEqual(r["you"]["owns"], [code])              # LAN default
        self.web.name_login = False
        conn = FakeConn()
        r = await self.web._auth(conn, {"t": "auth", "id": 2, "name": "alice"})
        self.assertEqual((r["ok"], r["you"]["owns"], r["you"]["name"]),
                         (True, [], None))
        r = await self.web._action(conn, {"t": "action", "id": 3, "lobby": code,
                                          "op": "kick", "member": "x"})
        self.assertEqual(r["error"], "forbidden")
        r = await self.web._auth(conn, {"t": "auth", "id": 4,
                                        "token": "s3cret-admin-token"})
        self.assertTrue(r["you"]["admin"])                       # still works

    async def test_cloud_mode_turns_name_login_off(self):
        import sys
        argv, run = sys.argv, asyncio.run
        made = {}
        orig_init = mp_server.Server.__init__

        def init(s, *a, **k):
            orig_init(s, *a, **k)
            made["srv"] = s
        mp_server.Server.__init__ = init
        asyncio.run = lambda coro: coro.close()
        try:
            for args, want in ((["--cloud", "--web-port", "1"], False),
                               (["--web-port", "1"], True),
                               (["--cloud", "--web-port", "1",
                                 "--owner-name-login", "on"], True),
                               (["--web-port", "1",
                                 "--owner-name-login", "off"], False)):
                sys.argv = ["mp_server.py"] + args
                mp_server.main()
                self.assertEqual(made["srv"].web.name_login, want, args)
        finally:
            sys.argv, asyncio.run = argv, run
            mp_server.Server.__init__ = orig_init

    async def test_chosen_admin_token_stays_out_of_the_log(self):
        self.web.announce_token = False
        await self.start()
        self.assertFalse(any("s3cret-admin-token" in m for m in self.logs))
        self.assertTrue(any("not shown" in m for m in self.logs))

    async def test_login_name_is_capped(self):
        conn = FakeConn()
        await self.web._auth(conn, {"t": "auth", "name": "n" * 5000})
        self.assertEqual(len(conn.name), 32)


if __name__ == "__main__":
    unittest.main()
