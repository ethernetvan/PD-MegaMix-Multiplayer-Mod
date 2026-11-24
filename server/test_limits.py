#!/usr/bin/env python3
"""Connection limits, per-client request budgets and the room web key."""
import asyncio
import json
import unittest

import mp_server
from lobby import (BROADCAST_PER_S, CREATE_PER_MIN, JOIN_PER_MIN,
                   LobbyManager, WEB_KEY_CHARS, norm_web_key)
from mp_server import Server
from ratelimit import ConnGate
from test_security import W, feed, hello
from test_web_bridge import FakeConn, FakeServer, mk_lobby
from web_bridge import WebBridge, forwarded_peer, vouched_ip


class Gate(unittest.TestCase):
    def test_open_connections_per_address(self):
        g = ConnGate(per_ip=3, new_per_min=100)
        self.assertEqual([g.admit("a", 0.0) for _ in range(4)],
                         ["", "", "", "too_many_connections"])
        self.assertEqual(g.admit("b", 0.0), "")          # others unaffected
        g.release("a")
        self.assertEqual(g.admit("a", 0.0), "")

    def test_new_connections_per_minute(self):
        g = ConnGate(per_ip=100, new_per_min=5)
        for i in range(5):
            self.assertEqual(g.admit("a", float(i)), "")
            g.release("a")
        self.assertEqual(g.admit("a", 5.0), "too_many_new_connections")
        # being refused doesn't extend the wait: 60 s after the FIRST one
        for t in (10.0, 30.0, 59.9):
            self.assertEqual(g.admit("a", t), "too_many_new_connections")
        self.assertEqual(g.admit("a", 60.0), "")

    def test_failed_hellos(self):
        g = ConnGate(per_ip=100, new_per_min=100, fails_per_min=3)
        for i in range(3):
            self.assertEqual(g.admit("a", float(i)), "")
            g.failed("a", float(i))
            g.release("a")
        self.assertEqual(g.admit("a", 3.0), "too_many_failures")
        self.assertEqual(g.admit("b", 3.0), "")
        self.assertEqual(g.admit("a", 60.5), "")         # first failure aged out

    def test_total_and_unkeyed(self):
        g = ConnGate(per_ip=1, new_per_min=1, fails_per_min=1, total=3)
        for _ in range(3):                               # None: only the total
            self.assertEqual(g.admit(None, 0.0), "")
            g.failed(None, 0.0)
        self.assertEqual(g.admit(None, 0.0), "server_full")
        self.assertEqual(g.admit("a", 0.0), "server_full")
        g.release(None)
        self.assertEqual(g.admit("a", 0.0), "")
        self.assertEqual(g.open, 3)

    def test_tables_do_not_grow_forever(self):
        g = ConnGate()
        for i in range(5000):
            self.assertEqual(g.admit(f"ip{i}", float(i)), "")
            g.failed(f"ip{i}", float(i))
            g.release(f"ip{i}")
        self.assertLess(len(g._new), 400)
        self.assertLess(len(g._fails), 400)
        self.assertEqual((g.open, g._live), (0, {}))


class Admission(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        mp_server.log = lambda m: None
        self.srv = Server(0, 0)
        self.srv.conn_gate = ConnGate(per_ip=2, new_per_min=4, fails_per_min=3,
                                      total=6)

    async def live(self, peer, cid):
        """A console that stays connected; returns (writer, close())."""
        w, r = W(peer=peer), asyncio.StreamReader()
        r.feed_data(json.dumps(hello(cid=cid)).encode() + b"\n")
        task = asyncio.create_task(self.srv.handle(r, w))
        for _ in range(400):
            if w.data or task.done():
                break
            await asyncio.sleep(0.005)

        async def close():
            r.feed_eof()
            await asyncio.wait_for(task, 5)
        return w, close

    async def once(self, peer, *lines):
        w = W(peer=peer)
        await asyncio.wait_for(self.srv.handle(feed(*lines), w), 5)
        return w

    async def test_third_connection_from_one_address_is_told_why(self):
        a, close_a = await self.live("203.0.113.5", "a")
        b, close_b = await self.live("203.0.113.5", "b")
        c = await self.once("203.0.113.5", hello(cid="c"))
        self.assertEqual(c.msgs(), [{"t": "error", "code": "rate_limited",
                                     "msg": mp_server.REFUSED[
                                         "too_many_connections"]}])
        self.assertTrue(c.closed)
        self.assertEqual(sorted(self.srv.clients), ["a", "b"])
        d, close_d = await self.live("203.0.113.6", "d")     # another address
        self.assertEqual(d.msgs()[0]["t"], "welcome")
        await close_a()
        e = await self.once("203.0.113.5", hello(cid="e"))   # a slot is free
        self.assertEqual(e.msgs()[0]["t"], "welcome")
        await close_b()
        await close_d()
        self.assertEqual(self.srv.conn_gate.open, 0)

    async def test_new_connections_per_minute(self):
        for i in range(4):
            w = await self.once("203.0.113.5", hello(cid=f"c{i}"))
            self.assertEqual(w.msgs()[0]["t"], "welcome")
        w = await self.once("203.0.113.5", hello(cid="c9"))
        self.assertEqual(w.msgs()[0]["code"], "rate_limited")

    async def test_failed_hellos_lock_the_address_out_for_a_minute(self):
        for bad in (b"junk", {"t": "hello", "proto": 99},
                    hello(cid="has space")):
            await self.once("203.0.113.5", bad)
        w = await self.once("203.0.113.5", hello(cid="fine"))
        self.assertEqual(w.msgs(), [{"t": "error", "code": "rate_limited",
                                     "msg": mp_server.REFUSED[
                                         "too_many_failures"]}])
        self.assertNotIn("fine", self.srv.clients)
        w = await self.once("203.0.113.6", hello(cid="fine"))
        self.assertEqual(w.msgs()[0]["t"], "welcome")

    async def test_cid_clash_counts_as_a_failed_hello(self):
        key = "k" * 32
        own, close = await self.live("203.0.113.8", "victim")
        self.srv._cid_keys["victim"] = [self.srv._key_digest(key), None]
        for _ in range(3):
            w = await self.once("203.0.113.5", hello(cid="victim", key="z" * 32))
            self.assertEqual(w.msgs()[0]["code"], "cid_in_use")
        w = await self.once("203.0.113.5", hello(cid="victim", key="y" * 32))
        self.assertEqual(w.msgs()[0]["code"], "rate_limited")    # no more guesses
        await close()

    async def test_server_total(self):
        closers = []
        for i in range(6):
            w, close = await self.live(f"203.0.113.{i}", f"c{i}")
            self.assertEqual(w.msgs()[0]["t"], "welcome")
            closers.append(close)
        w = await self.once("203.0.113.99", hello(cid="late"))
        self.assertEqual(w.msgs()[0]["code"], "server_full")
        for close in closers:
            await close()

    async def test_this_machine_is_never_limited_per_address(self):
        for peer in ("127.0.0.1", "::1", "::ffff:127.0.0.1"):
            self.assertIsNone(self.srv.gate_key((peer, 1)))
        self.srv.conn_gate = ConnGate(per_ip=1, new_per_min=1, fails_per_min=1)
        for i in range(12):
            await self.once("127.0.0.1", b"junk")
            w = await self.once("127.0.0.1", hello(cid=f"bot{i}"))
            self.assertEqual(w.msgs()[0]["t"], "welcome")

    def test_gate_keys(self):
        s = self.srv
        self.assertEqual(s.gate_key(("10.0.0.7", 1)), "10.0.0.7")   # LAN: counted
        self.assertEqual(s.gate_key(("::ffff:10.0.0.7", 1)), "10.0.0.7")
        self.assertEqual(s.gate_key(("203.0.113.5", 1)), "203.0.113.5")
        self.assertEqual(s.gate_key(None), None)
        self.assertEqual(s.gate_key(("not an ip", 1)), "not an ip")

    async def test_a_websocket_console_brings_its_own_key(self):
        # behind the cloud proxy: no vouched address -> not counted per
        # address, however many arrive from the "same" peer
        self.srv.conn_gate = ConnGate(per_ip=1, new_per_min=1)
        for i in range(5):
            w = W(peer="198.51.100.1")
            w.gate_key = None
            await asyncio.wait_for(self.srv.handle(feed(hello(cid=f"n{i}")), w), 5)
            self.assertEqual(w.msgs()[0]["t"], "welcome")
        # vouched: counted against the console's own address
        for i, want in enumerate(("welcome", "error")):
            w = W(peer="198.51.100.1")
            w.gate_key = "203.0.113.77"
            await asyncio.wait_for(self.srv.handle(feed(hello(cid=f"v{i}")), w), 5)
            self.assertEqual(w.msgs()[0]["t"], want)

    def test_console_address_through_the_worker(self):
        h = {"cf-connecting-ip": "2a06:98c0:3600::103",
             "x-console-ip": "203.0.113.9", "x-console-auth": "s3cret"}
        self.assertEqual(forwarded_peer(h, ("10.0.0.2", 5), "s3cret"),
                         ("203.0.113.9", 5))
        self.assertEqual(vouched_ip(h, "s3cret"), "203.0.113.9")
        self.assertIsNone(vouched_ip(h, "other"))
        self.assertIsNone(vouched_ip(h, None))
        # without the shared secret (not configured, wrong, missing) the
        # header is not believed: it could name anyone's address
        for secret, auth in ((None, "s3cret"), ("s3cret", "wrong"),
                             ("s3cret", ""), ("", "")):
            h["x-console-auth"] = auth
            self.assertEqual(forwarded_peer(h, ("10.0.0.2", 5), secret),
                             ("2a06:98c0:3600::103", 5), (secret, auth))


def msgs(lm, cid=None, t=None):
    return [m for c, m in lm.outbox
            if (cid is None or c == cid) and (t is None or m.get("t") == t)]


class Budgets(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()
        for c in "abc":
            self.lm.on_hello(c, c.upper(), "v1", 100.0)

    def room(self):
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        for c in "bc":
            self.lm.on_message(c, {"t": "lobby_join", "code": code}, 100.0)
        self.lm.outbox.clear()
        return code, self.lm.lobbies[code]

    def test_lobby_create_per_minute(self):
        for i in range(CREATE_PER_MIN + 2):
            self.lm.on_message("a", {"t": "lobby_create"}, 100.0 + i)
            self.lm.on_message("a", {"t": "lobby_leave"}, 100.0 + i)
        self.assertEqual(len(msgs(self.lm, "a", "lobby")), CREATE_PER_MIN)
        errs = msgs(self.lm, "a", "lobby_error")
        self.assertEqual([e["code"] for e in errs][:1], ["rate_limited"])
        self.lm.outbox.clear()
        self.lm.on_message("b", {"t": "lobby_create"}, 104.0)   # per client
        self.assertEqual(len(msgs(self.lm, "b", "lobby")), 1)
        self.lm.on_message("a", {"t": "lobby_create"}, 160.5)   # a minute on
        self.assertEqual(len(msgs(self.lm, "a", "lobby")), 1)

    def test_lobby_join_attempts_per_minute(self):
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        for i in range(JOIN_PER_MIN):
            self.lm.on_message("b", {"t": "lobby_join", "code": "ZZZZ"}, 100.0)
        self.lm.on_message("b", {"t": "lobby_join", "code": code}, 101.0)
        codes = [e["code"] for e in msgs(self.lm, "b", "lobby_error")]
        self.assertEqual(codes, ["not_found"] * JOIN_PER_MIN + ["rate_limited"])
        self.assertNotIn("b", self.lm.by_cid)     # even the right code waits
        self.lm.on_message("b", {"t": "lobby_join", "code": code}, 160.5)
        self.assertIn("b", self.lm.by_cid)
        # a reconnect doesn't hand out a fresh budget
        self.lm.on_message("c", {"t": "lobby_join", "code": "ZZZZ"}, 100.0)
        self.lm.on_disconnect("c", 100.0)
        self.lm.on_hello("c", "C", "v1", 100.0)
        for i in range(JOIN_PER_MIN):
            self.lm.on_message("c", {"t": "lobby_join", "code": "ZZZZ"}, 100.0)
        self.assertEqual(msgs(self.lm, "c", "lobby_error")[-1]["code"],
                         "rate_limited")

    def test_broadcast_spam_is_coalesced_not_lost(self):
        code, lobby = self.room()
        for i in range(50):                       # 50 renames in half a second
            self.lm.on_message("b", {"t": "set_name", "name": f"n{i}"},
                               100.0 + i * 0.01)
        self.assertEqual(len(msgs(self.lm, "a", "lobby")), BROADCAST_PER_S)
        self.assertEqual(lobby.members[1].name, "n49")       # state is current
        self.lm.outbox.clear()
        self.lm.tick(100.6)                       # not due yet
        self.assertEqual(msgs(self.lm, "a", "lobby"), [])
        self.lm.tick(101.05)                      # the one catch-up resend
        got = msgs(self.lm, "a", "lobby")
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["members"][1]["name"], "n49")
        self.lm.outbox.clear()
        self.lm.tick(102.0)
        self.assertEqual(msgs(self.lm, "a", "lobby"), [])    # and only one

    def test_every_broadcast_trigger_has_its_own_budget(self):
        code, lobby = self.room()
        kinds = {
            "set_name": lambda i: {"t": "set_name", "name": f"x{i}"},
            "avatar": lambda i: {"t": "avatar", "module": i},
            "diff": lambda i: {"t": "diff", "d": i % 4},
            "autoplay": lambda i: {"t": "autoplay", "on": True, "pct": i},
        }
        for n, (t, make) in enumerate(kinds.items()):
            now = 200.0 + n * 10
            self.lm.outbox.clear()
            for i in range(20):
                self.lm.on_message("b", make(i), now)
            self.assertEqual(len(msgs(self.lm, "c", "lobby")),
                             BROADCAST_PER_S, t)
            self.lm.outbox.clear()
            self.lm.on_message("c", make(1), now)     # another member: own budget
            self.assertEqual(len(msgs(self.lm, "a", "lobby")), 1, t)
            self.lm.tick(now + 1.01)
        self.assertEqual(self.lm._client_modules["b"], 19)
        self.assertEqual(self.lm._client_diff["b"], (3, 0))
        self.assertEqual(lobby.members[1].autoplay_pct, 19)

    def test_ready_spam_towards_the_leader(self):
        code, lobby = self.room()
        self.lm.on_message("a", {"t": "sel", "pv": 3, "name": "s"}, 100.0)
        self.lm.outbox.clear()
        for i in range(30):
            self.lm.on_message("b", {"t": "ready", "ready": i % 2 == 0,
                                     "reason": "modules", "pv": 3}, 100.0)
        self.assertEqual(len(msgs(self.lm, "a", "sel_status")), BROADCAST_PER_S)
        self.lm.outbox.clear()
        self.lm.tick(101.05)
        st = msgs(self.lm, "a", "sel_status")
        self.assertEqual(len(st), 1)
        self.assertEqual([x["cid"] for x in st[0]["not_ready"]], ["b"])

    def test_normal_use_is_never_delayed(self):
        code, lobby = self.room()
        # what a console does on its own: a rename, a module change, a
        # difficulty change, ready flips while browsing - seconds apart
        for i, m in enumerate(({"t": "set_name", "name": "Rin"},
                               {"t": "avatar", "module": 40},
                               {"t": "diff", "d": 3, "e": 1},
                               {"t": "diff", "d": 2},
                               {"t": "autoplay", "on": True})):
            self.lm.outbox.clear()
            self.lm.on_message("b", m, 100.0 + i * 0.3)
            self.assertEqual(len(msgs(self.lm, "a", "lobby")), 1, m)
        self.assertEqual(lobby.deferred, {})

    def test_budget_table_is_swept(self):
        for i in range(3000):
            self.lm.on_message(f"x{i}", {"t": "lobby_join", "code": "ZZZZ"},
                               1000.0 + i)
        self.assertLess(len(self.lm._budget), 700)


class WebKey(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.srv = FakeServer()
        self.lm = self.srv.lobby
        self.web = WebBridge(self.srv, "127.0.0.1", 0, "admin-token-xyz",
                             static_dir="nope")
        self.web.name_login = False              # as on the online server
        self.lm.web_url = "https://example.test"
        self.code = mk_lobby(self.lm)            # Alice owns; Bob, Carol
        self.lobby = self.lm.lobbies[self.code]
        self.lm.outbox.clear()

    def ask(self, cid):
        self.lm.outbox.clear()
        self.lm.on_message(cid, {"t": "web_key"}, 100.0)
        return self.lm.outbox[-1]

    async def auth(self, conn, **kw):
        return await self.web._dispatch(conn, dict(t="auth", id=1, **kw))

    def test_only_the_owner_gets_it(self):
        to, m = self.ask("cid_alice_01")
        self.assertEqual((to, m["t"], m["code"], m["url"]),
                         ("cid_alice_01", "web_key", self.code,
                          "https://example.test"))
        self.assertRegex(m["key"], r"^[A-Z2-9]{5}-[A-Z2-9]{5}$")
        self.assertTrue(set(m["key"].replace("-", "")) <= set(WEB_KEY_CHARS))
        self.assertEqual(self.ask("cid_alice_01")[1]["key"], m["key"])  # stable
        to, e = self.ask("cid_bob_0002")
        self.assertEqual((to, e["t"], e["code"]),
                         ("cid_bob_0002", "lobby_error", "not_owner"))
        self.lm.on_hello("cid_dave", "Dave", "v1", 100.0)
        self.assertEqual(self.ask("cid_dave")[1]["code"], "not_in_lobby")
        # the key is in nothing the other members or browsers are sent
        key = m["key"].replace("-", "")
        self.lm.outbox.clear()
        self.lm._broadcast_lobby(self.lobby)
        blob = json.dumps([x for _, x in self.lm.outbox]) + \
            json.dumps(self.web.snapshot())
        self.assertNotIn(key, blob)
        self.assertNotIn(m["key"], blob)

    def test_what_people_type(self):
        self.assertEqual(norm_web_key("abcde-fghjk"), "ABCDEFGHJK")
        self.assertEqual(norm_web_key(" ABCDE FGHJK "), "ABCDEFGHJK")
        for bad in (None, 5, "", "ABCDE", "ABCDE-FGHJ1", "ABCDE-FGHJO",
                    "A" * 11, "x" * 500, ["ABCDEFGHJK"]):
            self.assertIsNone(norm_web_key(bad), bad)

    async def test_key_gives_host_controls_for_that_room_only(self):
        key = self.ask("cid_alice_01")[1]["key"]
        self.lm.on_hello("cid_erin", "Erin", "v1", 100.0)
        self.lm.on_message("cid_erin", {"t": "lobby_create"}, 100.0)
        other = self.lm.by_cid["cid_erin"].code
        c = FakeConn()
        r = await self.auth(c, key=key.lower())          # as typed
        self.assertEqual((r["ok"], r["you"]),
                         (True, {"name": None, "admin": False,
                                 "owns": [self.code]}))
        bob = self.web.mid("cid_bob_0002")
        r = await self.web._dispatch(c, {"t": "action", "id": 2, "op": "kick",
                                         "lobby": self.code, "member": bob})
        self.assertTrue(r["ok"])
        self.assertNotIn("cid_bob_0002", self.lm.by_cid)
        r = await self.web._dispatch(c, {"t": "action", "id": 3,
                                         "op": "set_opt", "lobby": self.code,
                                         "key": "same_diff", "value": True})
        self.assertTrue(r["ok"] and self.lobby.same_diff)
        r = await self.web._dispatch(c, {"t": "action", "id": 4, "op": "kick",
                                         "lobby": other, "member": bob})
        self.assertEqual(r["error"], "forbidden")
        self.assertFalse(c.admin)                         # host, not server admin
        r = await self.auth(c)                            # logout
        self.assertEqual(r["you"]["owns"], [])

    async def test_wrong_keys(self):
        self.ask("cid_alice_01")
        c = FakeConn()
        for bad in ("AAAAA-AAAAA", "nope", 5, "x" * 300):
            r = await self.auth(c, key=bad)
            self.assertEqual((r["ok"], r["error"]), (False, "bad_key"), bad)
        self.assertTrue(c.alive)
        r = await self.auth(c, key="BBBBB-BBBBB")
        self.assertFalse(c.alive)                         # fifth wrong guess
        # a wrong key next to the right admin token changes nothing
        c = FakeConn()
        r = await self.auth(c, token="admin-token-xyz", key="AAAAA-AAAAA")
        self.assertEqual((r["ok"], c.admin), (False, False))
        # a room nobody asked a key for can't be opened with anything
        c = FakeConn()
        self.lobby.web_key = None
        r = await self.auth(c, key="AAAAA-AAAAA")
        self.assertFalse(r["ok"])

    async def test_key_dies_with_the_ownership(self):
        key = self.ask("cid_alice_01")[1]["key"]
        c = FakeConn()
        await self.auth(c, key=key)
        self.assertEqual(self.web.you_for(c)["owns"], [self.code])
        self.lm.on_message("cid_alice_01", {"t": "lobby_leave"}, 100.0)
        self.assertEqual(self.lobby.owner_cid, "cid_bob_0002")
        self.assertEqual(self.web.you_for(c)["owns"], [])   # at once
        r = await self.web._dispatch(c, {"t": "action", "id": 2, "op": "kick",
                                         "lobby": self.code, "member": "x"})
        self.assertEqual(r["error"], "forbidden")
        self.assertFalse((await self.auth(FakeConn(), key=key))["ok"])
        new = self.ask("cid_bob_0002")[1]["key"]
        self.assertNotEqual(new, key)
        # the old owner coming back within the grace period owns the room
        # again and gets a fresh key; Bob's stops working
        self.lm.on_message("cid_alice_01", {"t": "lobby_join",
                                            "code": self.code}, 110.0)
        self.assertEqual(self.lobby.owner_cid, "cid_alice_01")
        self.assertFalse((await self.auth(FakeConn(), key=new))["ok"])
        again = self.ask("cid_alice_01")[1]["key"]
        self.assertNotIn(again, (key, new))
        self.assertTrue((await self.auth(FakeConn(), key=again))["ok"])

    async def test_admin_token_and_name_login_still_work(self):
        c = FakeConn()
        r = await self.auth(c, token="admin-token-xyz")
        self.assertTrue(r["you"]["admin"])
        self.web.name_login = True                        # LAN default
        key = self.ask("cid_alice_01")[1]["key"]
        c = FakeConn()
        r = await self.auth(c, name="alice", key=key)
        self.assertEqual(r["you"], {"name": "alice", "admin": False,
                                    "owns": [self.code]})


if __name__ == "__main__":
    unittest.main()
