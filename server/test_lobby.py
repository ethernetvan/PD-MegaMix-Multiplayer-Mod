#!/usr/bin/env python3
import unittest
from lobby import LobbyManager


class T(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()

    def msgs(self, cid=None, t=None):
        out = self.lm.outbox
        self.lm.outbox = []
        return [(c, m) for c, m in out
                if (cid is None or c == cid) and (t is None or m.get("t") == t)]

    def mk(self, cid, name="P"):
        self.lm.on_hello(cid, name, "v", 100.0)

    def create(self, cid):
        self.lm.on_message(cid, {"t": "lobby_create"}, 100.0)

    def join(self, cid, code):
        self.lm.on_message(cid, {"t": "lobby_join", "code": code}, 100.0)

    def code_of(self, cid):
        for c, l in self.lm.lobbies.items():
            if any(m.cid == cid for m in l.members):
                return c
        return None

    # ----- create/join/leave -----
    def test_create_join_leave(self):
        self.mk("a", "A"); self.mk("b", "B")
        self.create("a")
        code = self.code_of("a")
        self.assertTrue(code)
        self.create("a")   # already in lobby
        errs = self.msgs("a", "lobby_error")
        self.assertEqual(errs[0][1]["code"], "already_in_lobby")
        self.join("b", "NOPE")
        self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"], "not_found")
        self.join("b", code)
        l = self.lm.lobbies[code]
        self.assertEqual([m.cid for m in l.members], ["a", "b"])
        self.assertEqual(l.leader(), "a")
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "joined"]
        self.assertTrue(ev)
        # join mid-create -> second create blocked
        self.lm.on_message("b", {"t": "lobby_leave"}, 101.0)
        self.assertEqual([m.cid for m in l.members], ["a"])
        self.lm.on_message("a", {"t": "lobby_leave"}, 102.0)
        self.assertNotIn(code, self.lm.lobbies)

    def test_codes_unique(self):
        for i in range(3):
            self.mk(f"u{i}"); self.create(f"u{i}")
        self.assertEqual(len(set(self.lm.lobbies)), 3)

    def test_leader_handoff_leave(self):
        self.mk("a"); self.mk("b"); self.mk("c")
        self.create("a")
        code = self.code_of("a")
        self.join("b", code); self.join("c", code)
        l = self.lm.lobbies[code]
        self.lm.on_message("a", {"t": "lobby_leave"}, 101.0)
        self.assertEqual(l.leader(), "b")
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "leader"]
        self.assertEqual(ev[0]["cid"], "b")

    # ----- sel -----
    def test_sel_leader_only_and_fanout(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.msgs()
        self.lm.on_message("b", {"t": "sel", "pv": 1, "name": "x"}, 100.0)
        self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"], "not_leader")
        self.lm.on_message("a", {"t": "sel", "pv": 100, "name": "S"}, 101.0)
        sels = self.msgs(t="sel")
        self.assertEqual(len(sels), 1)         # only to follower, not echo to leader
        self.assertEqual(sels[0][0], "b")
        # acks -> sel_status to leader
        self.lm.on_message("b", {"t": "sel_ack", "pv": 100, "has": False,
                                 "ok": False, "fb_pv": 50, "fb_name": "F"}, 102.0)
        st = self.msgs("a", "sel_status")
        self.assertEqual(st[0][1]["missing"][0]["cid"], "b")
        self.assertEqual(st[0][1]["missing"][0]["fb_name"], "F")
        self.lm.on_message("b", {"t": "sel_ack", "pv": 100, "has": True,
                                 "ok": False, "fb_pv": -1, "fb_name": None}, 103.0)
        st = self.msgs("a", "sel_status")
        self.assertEqual(st[0][1]["cant"][0]["cid"], "b")
        # stale pv ack ignored
        self.lm.on_message("b", {"t": "sel_ack", "pv": 999, "has": False,
                                 "ok": False, "fb_pv": -1, "fb_name": None}, 104.0)
        self.assertFalse(self.msgs("a", "sel_status"))

    # ----- start_req roles -----
    def test_start_roles(self):
        for c in "abcde": self.mk(c, c.upper())
        self.create("a"); code = self.code_of("a")
        for c in "bcde": self.join(c, code)
        self.lm.on_message("a", {"t": "sel", "pv": 7, "name": "S"}, 100.0)
        self.lm.on_message("b", {"t": "sel_ack", "pv": 7, "has": True, "ok": True,
                                 "fb_pv": -1, "fb_name": None}, 100.0)
        self.lm.on_message("c", {"t": "sel_ack", "pv": 7, "has": False, "ok": True,
                                 "fb_pv": 3, "fb_name": "x"}, 100.0)
        self.lm.on_message("d", {"t": "sel_ack", "pv": 7, "has": False, "ok": True,
                                 "fb_pv": -1, "fb_name": None}, 100.0)
        self.lm.on_disconnect("e", 100.5)
        self.lm.on_message("a", {"t": "start_req", "pv": 7, "name": "S"}, 101.0)
        l = self.lm.lobbies[code]; r = l.round
        # a=leader, b=has&ok, c=fallback, d=no song, e=disconnected, (no-ack opt)
        self.assertEqual(set(r.players), {"a", "b", "c"})
        self.assertEqual(set(r.spectators), {"d", "e"})
        self.assertEqual(l.phase, "LOADING")

    # ----- loading gating -----
    def test_all_loaded_start(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.lm.on_message("a", {"t": "loaded", "round": 1}, 101.0)
        self.lm.on_message("b", {"t": "loaded", "round": 1}, 102.0)
        st = self.msgs(t="start")
        self.assertEqual(st[0][1]["dropped"], [])
        self.assertEqual(self.lm.lobbies[code].phase, "PLAYING")

    def test_leader_loaded_30s_start_drops(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.lm.on_message("a", {"t": "loaded", "round": 1}, 101.0)
        self.lm.tick(100.0 + 30.0 + 1.0)   # 31s after leader load
        st = self.msgs(t="start")
        self.assertEqual(st[0][1]["dropped"], ["b"])
        self.assertEqual(self.lm.lobbies[code].leader(), "a")

    def test_no_leader_load_90s_abort(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.lm.tick(100.0 + 91.0)
        end = self.msgs(t="round_end")
        self.assertEqual(end[0][1]["next_leader"], "a")   # abort: no rotation
        self.assertEqual(self.lm.lobbies[code].phase, "SELECT")

    def test_no_leader_load_90s_start_loaded(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.lm.on_message("b", {"t": "loaded", "round": 1}, 105.0)
        self.lm.tick(100.0 + 91.0)
        st = self.msgs(t="start")
        self.assertEqual(st[0][1]["dropped"], ["a"])

    # ----- playing -----
    def test_board_and_events(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
        self.msgs()
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 100,
                                 "combo": 49, "pct": 1.0, "life": 100}, 102.0)
        self.lm.tick(102.01)
        n1 = len(self.msgs(t="board"))
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 110,
                                 "combo": 49, "pct": 1.0, "life": 100}, 102.03)
        self.lm.tick(102.05)
        n2 = len(self.msgs(t="board"))
        self.assertEqual(n2, 0)                      # throttled (<0.1s)
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 200,
                                 "combo": 51, "pct": 2.0, "life": 100}, 102.2)
        self.lm.tick(102.25)
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "combo"]
        self.assertEqual(ev[0]["value"], 51)
        # low life
        self.lm.on_message("b", {"t": "state", "round": 1, "score": 500,
                                 "combo": 10, "pct": 9.0, "life": 39}, 103.0)
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "low_life"]
        self.assertEqual(ev[0]["cid"], "b")
        self.lm.on_message("b", {"t": "state", "round": 1, "score": 500,
                                 "combo": 10, "pct": 9.0, "life": 30}, 103.5)
        self.assertFalse([m for _, m in self.msgs(t="event")
                          if m["kind"] == "low_life"])   # no dup
        self.lm.tick(103.6)
        b = self.msgs(t="board")
        self.assertEqual(b[0][1]["e"][0]["cid"], "b")    # sorted desc

    def test_midround_disconnect_board(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
        self.lm.on_disconnect("b", 105.0)
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 9,
                                 "combo": 1, "pct": 1, "life": 100}, 105.5)
        self.lm.tick(105.6)
        e = self.msgs(t="board")[0][1]["e"]
        self.assertFalse(next(r for r in e if r["cid"] == "b")["connected"])
        # reconnect resync
        self.lm.on_hello("b", "B", "v", 106.0)
        got = {m["t"] for c, m in self.msgs("b")}
        self.assertTrue({"lobby", "launch", "start", "board"} <= got)
        # leader unchanged mid-round
        self.assertEqual(self.lm.lobbies[code].leader(), "a")

    # ----- finals / standings / round_end -----
    def test_results_flow(self):
        for c in "abc": self.mk(c, c.upper())
        self.create("a"); code = self.code_of("a")
        self.join("b", code); self.join("c", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        for c in "abc":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
        self.lm.on_message("b", {"t": "final", "round": 1, "score": 500,
                                 "pct": 50, "max_combo": 9, "jc": [0]*5,
                                 "life": 100}, 110.0)
        self.assertEqual(self.lm.lobbies[code].phase, "PLAYING")   # new: waits
        st = self.msgs(t="standings")
        self.assertFalse(st[0][1]["complete"])
        self.lm.on_message("a", {"t": "final", "round": 1, "score": 900,
                                 "pct": 90, "max_combo": 20, "jc": [0]*5,
                                 "life": 127}, 111.0)
        self.lm.on_disconnect("c", 112.0)
        self.assertEqual(self.lm.lobbies[code].phase, "RESULTS")   # c dc = done
        st = self.msgs(t="standings")
        self.assertTrue(st[-1][1]["complete"])
        self.assertEqual(st[-1][1]["e"][0]["cid"], "a")     # rank 1
        self.assertTrue(st[-1][1]["e"][0]["final"])
        # all players back or dc -> round_end rotation
        self.lm.on_message("a", {"t": "back_in_sel", "round": 1}, 113.0)
        self.lm.on_message("b", {"t": "back_in_sel", "round": 1}, 113.5)
        self.lm.tick(114.0)
        end = self.msgs(t="round_end")
        self.assertEqual(end[0][1]["next_leader"], "b")   # c dc -> skipped
        self.assertEqual(self.lm.lobbies[code].phase, "SELECT")
        self.assertIsNone(self.lm.lobbies[code].sel_pv)

    def test_results_90s_fallback(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "final", "round": 1, "score": 1,
                                     "pct": 1, "max_combo": 1, "jc": [0]*5,
                                     "life": 1}, 110.0)
        self.lm.tick(111.0)   # completes
        self.lm.tick(111.0 + 91.0)
        self.assertTrue(self.msgs(t="round_end"))

    # ----- select disconnects -----
    def test_select_dc_handoff_remove_reconnect(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_disconnect("a", 100.0)
        l = self.lm.lobbies[code]
        self.lm.tick(109.0)
        self.assertEqual(l.leader(), "a")                  # <10s, no handoff
        self.lm.tick(111.0)
        self.assertEqual(l.leader(), "b")                  # 10s handoff
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "leader"]
        self.assertEqual(ev[0]["cid"], "b")
        # reconnect within 60s restores
        self.lm.on_hello("a", "A", "v", 130.0)
        self.assertTrue(self._member(code, "a").connected)
        self.assertEqual(self.lm.lobbies[code].leader(), "b")
        # removal after 60s
        self.lm.on_disconnect("a", 200.0)
        self.lm.tick(200.0 + 61.0)
        self.assertIsNone(self._member(code, "a"))

    def _member(self, code, cid):
        l = self.lm.lobbies.get(code)
        return next((m for m in l.members if m.cid == cid), None) if l else None

    # ----- kick / set_leader / join mid-round -----
    def test_kick_and_admin(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        r = self.lm.on_admin({"cmd": "kick", "cid": "b"}, 100.0)
        self.assertTrue(r["ok"])
        self.assertEqual([m.cid for m in self.lm.lobbies[code].members], ["a"])
        kicked = self.msgs("b", "lobby_left")
        self.assertEqual(kicked[0][1]["reason"], "kicked")
        dump = self.lm.on_admin({"cmd": "lobbies"}, 101.0)
        self.assertIn(code, dump)
        self.lm.on_admin({"cmd": "set_leader", "code": code, "cid": "a"}, 102.0)

    def test_join_midround_spectator_then_player(self):
        self.mk("a"); self.mk("b"); self.mk("c")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.join("c", code)
        m = self._member(code, "c")
        self.assertEqual(m.role, "spectator")
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
            self.lm.on_message(c, {"t": "final", "round": 1, "score": 1,
                                   "pct": 1, "max_combo": 1, "jc": [0]*5,
                                   "life": 1}, 105.0)
            self.lm.on_message(c, {"t": "back_in_sel", "round": 1}, 106.0)
        self.lm.tick(107.0)
        self.assertEqual(self.lm.lobbies[code].phase, "SELECT")
        self.assertEqual(self._member(code, "c").role, "player")

    def test_pending_admin_leader(self):
        self.mk("a"); self.mk("b")
        self.create("a"); code = self.code_of("a"); self.join("b", code)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        self.lm.on_admin({"cmd": "set_leader", "code": code, "cid": "b"}, 100.5)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
            self.lm.on_message(c, {"t": "final", "round": 1, "score": 1,
                                   "pct": 1, "max_combo": 1, "jc": [0]*5,
                                   "life": 1}, 105.0)
            self.lm.on_message(c, {"t": "back_in_sel", "round": 1}, 106.0)
        self.lm.tick(107.0)
        end = self.msgs(t="round_end")
        self.assertEqual(end[0][1]["next_leader"], "b")


if __name__ == "__main__":
    unittest.main()

class T2(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()

    def msgs(self, cid=None, t=None):
        out = self.lm.outbox
        self.lm.outbox = []
        return [(c, m) for c, m in out
                if (cid is None or c == cid) and (t is None or m.get("t") == t)]

    def mk(self, cid):
        self.lm.on_hello(cid, cid.upper(), "v", 100.0)

    def lobby3(self):
        for c in "abc": self.mk(c)
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        self.join("b", code); self.join("c", code)
        return code

    def join(self, cid, code):
        self.lm.on_message(cid, {"t": "lobby_join", "code": code}, 100.0)

    def start3(self, code):
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 100.0)
        for c in "abc":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 101.0)
        self.msgs()

    def test_early_finisher_keeps_playing(self):
        code = self.lobby3(); self.start3(code)
        l = self.lm.lobbies[code]
        self.lm.on_message("b", {"t": "final", "round": 1, "score": 5,
                                 "pct": 5, "max_combo": 5, "jc": [0]*5,
                                 "life": 0}, 110.0)
        self.assertEqual(l.phase, "PLAYING")          # not RESULTS yet
        st = self.msgs(t="standings")
        self.assertFalse(st[0][1]["complete"])
        # a still sends state -> board still flows
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 9,
                                 "combo": 1, "pct": 1, "life": 100}, 111.0)
        self.lm.tick(111.2)
        self.assertTrue(self.msgs(t="board"))
        # b's state ignored after final
        self.lm.on_message("b", {"t": "state", "round": 1, "score": 999,
                                 "combo": 9, "pct": 9, "life": 9}, 111.5)
        l2 = self.lm.lobbies[code]
        b = next(m for m in l2.members if m.cid == "b")
        self.assertEqual(b.state["score"], 5)
        # a + c finish -> RESULTS + complete standings
        for c in "ac":
            self.lm.on_message(c, {"t": "final", "round": 1, "score": 9,
                                     "pct": 9, "max_combo": 9, "jc": [0]*5,
                                     "life": 9}, 112.0)
        self.assertEqual(self.lm.lobbies[code].phase, "RESULTS")
        self.assertTrue(self.msgs(t="standings")[-1][1]["complete"])

    def test_stale_30s_completes(self):
        code = self.lobby3(); self.start3(code)
        l = self.lm.lobbies[code]
        self.lm.on_message("b", {"t": "final", "round": 1, "score": 5,
                                 "pct": 5, "max_combo": 5, "jc": [0]*5,
                                 "life": 0}, 110.0)
        # a sent its last state at 111; c still playing normally
        self.lm.on_message("a", {"t": "state", "round": 1, "score": 9,
                                 "combo": 1, "pct": 1, "life": 60}, 111.0)
        self.lm.on_message("c", {"t": "final", "round": 1, "score": 7,
                                 "pct": 7, "max_combo": 7, "jc": [0]*5,
                                 "life": 7}, 140.0)
        self.lm.tick(141.5)
        self.assertEqual(self.lm.lobbies[code].phase, "RESULTS")  # a stale>30s

    def test_disconnect_completes(self):
        code = self.lobby3(); self.start3(code)
        self.lm.on_message("b", {"t": "final", "round": 1, "score": 5,
                                 "pct": 5, "max_combo": 5, "jc": [0]*5,
                                 "life": 0}, 110.0)
        self.lm.on_message("a", {"t": "final", "round": 1, "score": 5,
                                 "pct": 5, "max_combo": 5, "jc": [0]*5,
                                 "life": 0}, 111.0)
        self.lm.on_disconnect("c", 112.0)
        self.assertEqual(self.lm.lobbies[code].phase, "RESULTS")

    def test_disconnect_twice_idempotent(self):
        code = self.lobby3()
        l = self.lm.lobbies[code]
        self.lm.on_disconnect("b", 100.0)
        self.lm.on_disconnect("b", 100.5)   # already disconnected -> no-op
        m = next(m for m in l.members if m.cid == "b")
        self.assertFalse(m.connected)

    def test_full_lobby(self):
        for i in range(9): self.mk(f"u{i}")
        self.lm.on_message("u0", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        for i in range(1, 8):
            self.join(f"u{i}", code)
        self.join("u8", code)
        self.assertEqual(self.msgs("u8", "lobby_error")[0][1]["code"], "full")

    def test_lobby_left_on_dc_removal(self):
        code = self.lobby3()
        self.lm.on_disconnect("b", 100.0)
        self.lm.tick(161.0)
        l = self.lm.lobbies[code]
        self.assertIsNone(next((m for m in l.members if m.cid == "b"), None))

    def test_names_instance_not_shared(self):
        lm2 = LobbyManager()
        lm2.on_hello("x", "X", "v", 1.0)
        self.assertNotIn("x", self.lm._client_names)
        self.lm.on_message("x", {"t": "set_name", "name": "Y"}, 1.0)
        # set_name works even not in lobby (names tracked anyway)
        self.lm.on_hello("x", "X", "v", 2.0)
        self.lm.on_message("x", {"t": "set_name", "name": "Y"}, 3.0)
        self.assertEqual(self.lm._client_names["x"], "Y")

class T3(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()

    def msgs(self, cid=None, t=None):
        out = self.lm.outbox; self.lm.outbox = []
        return [(c, m) for c, m in out
                if (cid is None or c == cid) and (t is None or m.get("t") == t)]

    def test_ready_tracking(self):
        for c in "ab": self.lm.on_hello(c, c, "v", 100.0)
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        self.lm.on_message("b", {"t": "lobby_join", "code": code}, 100.0)
        self.lm.on_message("a", {"t": "sel", "pv": 7, "name": "s"}, 101.0)
        self.msgs()
        self.lm.on_message("b", {"t": "ready", "pv": 7, "ready": False,
                                 "reason": "modules"}, 102.0)
        st = self.msgs("a", "sel_status")
        self.assertEqual(st[0][1]["not_ready"][0]["cid"], "b")
        self.assertEqual(st[0][1]["not_ready"][0]["reason"], "modules")
        # lobby msg carries ready/reason
        self.lm.on_message("b", {"t": "set_name", "name": "B2"}, 103.0)
        lm_ = [m for c, m in self.msgs(t="lobby")][-1]
        b = next(x for x in lm_["members"] if x["cid"] == "b")
        self.assertFalse(b["ready"]); self.assertEqual(b["reason"], "modules")
        self.lm.on_message("b", {"t": "ready", "pv": 7, "ready": True,
                                 "reason": ""}, 104.0)
        st = self.msgs("a", "sel_status")
        self.assertEqual(st[0][1]["not_ready"], [])

class T4(unittest.TestCase):
    def setUp(self):
        self.lm = LobbyManager()
        for c in "ab": self.lm.on_hello(c, c, "v1", 100.0)

    def msgs(self, cid=None, t=None):
        out = self.lm.outbox; self.lm.outbox = []
        return [(c, m) for c, m in out
                if (cid is None or c == cid) and (t is None or m.get("t") == t)]

    def _mk(self):
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        self.lm.on_message("b", {"t": "lobby_join", "code": code}, 100.0)
        return self.lm.lobbies[code]

    def test_owner_and_pick_modes(self):
        lb = self._mk(); self.msgs()
        self.assertEqual(lb.owner_cid, "a")
        self.assertEqual(lb.pick_mode, "rotate")
        # non-owner rejected
        self.lm.on_message("b", {"t": "set_pick_mode", "mode": "owner"}, 101.0)
        self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"],
                         "not_owner")
        # owner sets assigned to b
        self.lm.on_message("a", {"t": "set_pick_mode", "mode": "assigned",
                                 "cid": "b"}, 102.0)
        lm_ = [m for c, m in self.msgs(t="lobby")][-1]
        self.assertEqual(lm_["pick_mode"], "assigned")
        self.assertEqual(lm_["leader"], "b")
        # owner leave -> b becomes owner
        self.lm.on_message("a", {"t": "lobby_leave"}, 103.0)
        self.assertEqual(self.lm.lobbies[lb.code].owner_cid, "b")
        # owner mode keeps owner as leader
        self.lm.on_message("b", {"t": "set_pick_mode", "mode": "owner"}, 104.0)
        lm_ = [m for c, m in self.msgs(t="lobby")][-1]
        self.assertEqual(lm_["leader"], "b")

    def test_pause_owner_immediate(self):
        lb = self._mk()
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 101.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 102.0)
        self.lm.tick(103.0); self.msgs()
        self.lm.on_message("a", {"t": "pause_req", "round": 1}, 104.0)
        p = self.msgs(t="pause")
        self.assertEqual(len(p), 2)  # broadcast to both
        self.lm.on_message("a", {"t": "resume_req", "round": 1}, 105.0)
        self.assertEqual(len(self.msgs(t="resume")), 2)

    def test_pause_vote_pass_and_expire(self):
        lb = self._mk()
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 101.0)
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 102.0)
        self.lm.tick(103.0); self.msgs()
        # non-owner requests pause -> vote opens (a is owner, need both players)
        self.lm.on_message("b", {"t": "pause_req", "round": 1}, 104.0)
        v = self.msgs(t="pause_vote")
        self.assertEqual(v[0][1]["kind"], "pause")
        self.assertEqual(v[0][1]["need"], 2)
        self.assertFalse(self.msgs(t="pause"))
        # second requester (owner via vote path? no - owner triggers immediate)
        # make a non-owner: transfer owner to a third? simplest: a is owner so
        # a's req is immediate. To test a full vote use 2 non-owners:
        lb2 = self.lm.lobbies[lb.code]
        lb2.owner_cid = "zzz"   # pretend owner gone; both members vote
        self.lm.on_message("b", {"t": "pause_req", "round": 1}, 104.5)
        self.msgs()
        self.lm.on_message("a", {"t": "pause_req", "round": 1}, 105.0)
        self.assertEqual(len(self.msgs(t="pause")), 2)   # passed
        # expire path
        self.lm.on_message("b", {"t": "resume_req", "round": 1}, 106.0)
        self.msgs()
        self.lm.tick(122.0)
        v = self.msgs(t="pause_vote")
        self.assertTrue(v[0][1].get("expired"))
