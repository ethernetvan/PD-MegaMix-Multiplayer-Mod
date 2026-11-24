#!/usr/bin/env python3
import unittest
from lobby import LobbyManager, Lobby, Member, Round


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

    def test_owner_kick(self):
        self.mk("a"); self.mk("b"); self.mk("c")
        self.create("a"); code = self.code_of("a")
        self.join("b", code); self.join("c", code)
        l = self.lm.lobbies[code]
        self.msgs()
        self.lm.on_message("b", {"t": "kick", "cid": "c"}, 101.0)
        self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"], "not_owner")
        self.lm.on_message("a", {"t": "kick", "cid": "a"}, 101.0)
        self.assertEqual(self.msgs("a", "lobby_error")[0][1]["code"], "bad_member")
        self.lm.on_message("a", {"t": "kick", "cid": "c"}, 102.0)
        self.assertEqual([m.cid for m in l.members], ["a", "b"])
        left = self.msgs("c", "lobby_left")
        self.assertEqual(left[0][1]["reason"], "kicked")
        self.assertIsNone(self.code_of("c"))

    def test_avatar_carries_and_rebroadcasts(self):
        self.mk("a"); self.mk("b")
        self.lm.on_message("a", {"t": "avatar", "module": 40}, 100.0)
        self.create("a"); code = self.code_of("a")
        self.join("b", code)
        lob = self.msgs("b", "lobby")[-1][1]
        self.assertEqual({m["cid"]: m["module"] for m in lob["members"]},
                         {"a": 40, "b": -1})
        self.msgs()
        self.lm.on_message("b", {"t": "avatar", "module": 7}, 101.0)
        lob = self.msgs("a", "lobby")[-1][1]
        self.assertEqual({m["cid"]: m["module"] for m in lob["members"]}["b"], 7)
        self.lm.on_message("b", {"t": "avatar", "module": 7}, 102.0)
        self.assertEqual(self.msgs(t="lobby"), [])
        for bad in ("x", True, -2, 100000, None):
            self.lm.on_message("b", {"t": "avatar", "module": bad}, 103.0)
            self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"], "bad_avatar")

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
        m = dump[code]["members"][0]
        for k in ("score", "combo", "pct", "life", "final", "autoplay_pct"):
            self.assertIn(k, m)
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


    def test_autoplay_pct(self):
        self.mk("a"); self.create("a")
        lob = self.lm.lobbies[self.code_of("a")]
        m = lob.members[0]
        for msg, want in (
            ({"t": "autoplay", "on": True, "pct": 50}, 50),
            ({"t": "autoplay", "on": True}, 100),
            ({"t": "autoplay", "on": True, "pct": True}, 100),
            ({"t": "autoplay", "on": True, "pct": "50"}, 100),
            ({"t": "autoplay", "on": True, "pct": 150}, 100),
            ({"t": "autoplay", "on": True, "pct": -5}, 0),
        ):
            self.lm.on_message("a", msg, 100.0)
            self.assertEqual(m.autoplay_pct, want, msg)
            self.assertTrue(m.autoplay)


class CountdownTests(unittest.TestCase):
    """--countdown N: countdown broadcast, then start at +N s."""
    def setUp(self):
        self.m = LobbyManager(countdown=3.0)
        self.sent = []
        self.m._send = lambda cid, msg: self.sent.append((cid, msg))
        self.m._broadcast = lambda lb, msg: self.sent.append(("*", msg))
        self.m._broadcast_lobby = lambda lb: None
    def _mk(self):
        lb = Lobby("ABCD")
        self.m.lobbies["ABCD"] = lb
        for cid in ("a", "b"):
            m = Member(cid, cid)
            m.loaded = True
            lb.members.append(m)
        r = Round(1, 700, "song", "a")
        r.players = ["a", "b"]
        lb.round = r
        return lb, r
    def test_countdown_then_start(self):
        lb, r = self._mk()
        self.m._do_start(lb, 100.0, [])
        types = [m["t"] for _, m in self.sent]
        self.assertIn("countdown", types)
        self.assertNotIn("start", types)     # no start before +3s
        cd = next(m for _, m in self.sent if m["t"] == "countdown")
        self.assertEqual(cd["secs"], 3.0)
        self.sent.clear()
        self.m._tick_lobby(lb, 102.0)
        self.assertNotIn("start", [m["t"] for _, m in self.sent])
        self.m._tick_lobby(lb, 103.0)
        self.assertIn("start", [m["t"] for _, m in self.sent])
    def test_no_start_if_round_ends(self):
        lb, r = self._mk()
        self.m._do_start(lb, 100.0, [])
        lb.round = None                    # round cancelled mid-countdown
        self.sent.clear()
        self.m._tick_lobby(lb, 103.0)
        self.assertNotIn("start", [m["t"] for _, m in self.sent])
    def test_zero_countdown_unchanged(self):
        self.m.countdown = 0.0
        lb, r = self._mk()
        self.m._do_start(lb, 100.0, [])
        types = [m["t"] for _, m in self.sent]
        self.assertIn("start", types)
        self.assertNotIn("countdown", types)


class ReArmTests(unittest.TestCase):
    def setUp(self):
        self.m = LobbyManager()
        self.ev = []
        self.m._event = lambda lb, kind, cid, name, val: self.ev.append((kind, val))
        lb = Lobby("ABCD"); self.m.lobbies["ABCD"] = lb; lb.phase = "PLAYING"
        m = Member("a", "a"); lb.members.append(m)
        self.m.by_cid["a"] = lb
        r = Round(1, 700, "s", "a"); r.players = ["a"]; lb.round = r
        self.lb, self.m_m, self.r = lb, m, r
    def _state(self, score=0, combo=0, life=100, pct=0.0):
        self.m._state("a", {"round": 1, "score": score, "combo": combo,
                            "pct": pct, "life": life}, 0.0)
    def test_combo_rearm_after_break(self):
        self._state(combo=100); self.assertIn(("combo", 100), self.ev)
        self.ev.clear()
        self._state(combo=3)   # break
        self._state(combo=50)
        self.assertIn(("combo", 50), self.ev)  # fired again
    def test_low_life_rearm(self):
        self._state(life=30); self.assertIn(("low_life", 30), self.ev)
        self.ev.clear()
        self._state(life=70)   # recover
        self._state(life=35)
        self.assertIn(("low_life", 35), self.ev)  # fired again


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

    def test_pause_vote_ignores_finished(self):
        # three players; c already sent a final -> need = the other two
        self.lm.on_hello("c", "c", "v1", 100.0)
        lb = self._mk()
        self.lm.on_message("c", {"t": "lobby_join", "code": lb.code}, 100.0)
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 101.0)
        for c in "abc":
            self.lm.on_message(c, {"t": "loaded", "round": 1}, 102.0)
        self.lm.tick(103.0); self.msgs()
        lb.owner_cid = "zzz"   # no immediate-owner path; everyone votes
        self.lm.on_message("c", {"t": "final", "round": 1, "score": 5,
                                 "pct": 10.0, "max_combo": 0,
                                 "jc": [0, 0, 0, 0, 0], "life": 0}, 104.0)
        self.msgs()
        # finished player's reqs are ignored
        self.lm.on_message("c", {"t": "resume_req", "round": 1}, 104.5)
        self.assertFalse(self.msgs(t="pause_vote"))
        self.assertFalse(self.msgs(t="resume"))
        # pause via the two unfinished players
        self.lm.on_message("b", {"t": "pause_req", "round": 1}, 105.0)
        v = self.msgs(t="pause_vote")
        self.assertEqual(v[0][1]["need"], 2)
        self.lm.on_message("a", {"t": "pause_req", "round": 1}, 105.5)
        self.assertEqual(len(self.msgs(t="pause")), 3)   # passed
        # resume: only the two unfinished players needed again
        self.lm.on_message("b", {"t": "resume_req", "round": 1}, 106.0)
        v = self.msgs(t="pause_vote")
        self.assertEqual(v[0][1]["need"], 2)
        self.lm.on_message("a", {"t": "resume_req", "round": 1}, 106.5)
        self.assertEqual(len(self.msgs(t="resume")), 3)  # passed


class T5(unittest.TestCase):
    """scene reporting, leader/owner round end, owner succession,
       competition ranking, pause dedupe (items 7-13)."""
    def setUp(self):
        self.lm = LobbyManager()

    def msgs(self, cid=None, t=None):
        out = self.lm.outbox; self.lm.outbox = []
        return [(c, m) for c, m in out
                if (cid is None or c == cid) and (t is None or m.get("t") == t)]

    def mk(self, cid, mod="v1"):
        self.lm.on_hello(cid, cid, mod, 100.0)

    def _mk(self, n=2):
        self.lm.on_message("a", {"t": "lobby_create"}, 100.0)
        code = next(iter(self.lm.lobbies))
        for c in "bcdef"[:n - 1]:
            self.lm.on_message(c, {"t": "lobby_join", "code": code}, 100.0)
        return self.lm.lobbies[code]

    def _playing(self, lb, players="ab"):
        rid = lb.seq + 1
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 101.0)
        for c in players:
            self.lm.on_message(c, {"t": "loaded", "round": rid}, 102.0)
        self.lm.tick(103.0)
        self.msgs()
        return rid

    def _final(self, c, rid, pct, score):
        self.lm.on_message(c, {"t": "final", "round": rid, "score": score,
                               "pct": pct, "max_combo": 0,
                               "jc": [0, 0, 0, 0, 0], "life": 0}, 104.0)

    def test_board_competition_ranks_and_life(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        rid = self._playing(lb, players="abc")
        # a and b tie on pct+score -> both 2nd (lowest position); c is 3rd
        self.lm.on_message("a", {"t": "state", "round": rid, "score": 100,
                                 "combo": 0, "pct": 50.0, "life": 200}, 105.0)
        self.lm.on_message("b", {"t": "state", "round": rid, "score": 100,
                                 "combo": 0, "pct": 50.0, "life": 90}, 105.1)
        self.lm.on_message("c", {"t": "state", "round": rid, "score": 80,
                                 "combo": 0, "pct": 40.0, "life": 15}, 105.2)
        self.lm.tick(106.0)
        b = [m for _, m in self.msgs(t="board")][-1]
        ranks = {row["cid"]: row["rank"] for row in b["e"]}
        lives = {row["cid"]: row["life"] for row in b["e"]}
        self.assertEqual(ranks["a"], 2)
        self.assertEqual(ranks["b"], 2)
        self.assertEqual(ranks["c"], 3)
        self.assertEqual(lives["a"], 200)
        self.assertEqual(lives["b"], 90)
        self.assertEqual(lives["c"], 15)

    # ---- 7/8: scene -> spectator at launch
    def test_scene_spectator(self):
        for c in "abcd": self.mk(c)
        lb = self._mk(4)
        self.lm.on_message("b", {"t": "scene", "where": "results"}, 100.0)
        self.lm.on_message("d", {"t": "scene", "where": "other"}, 100.0)
        self.lm.on_message("b", {"t": "scene", "where": "bogus"}, 100.0)
        self.assertEqual(self.lm._member(lb, "b").scene, "results")
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"}, 101.0)
        r = lb.round
        self.assertEqual(r.players, ["a", "c"])   # c: unknown scene = sel
        self.assertEqual(r.spectators, ["b", "d"])

    # ---- 10: leader/owner back_in_sel ends the round
    def test_leader_back_results_immediate(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._final("a", rid, 50, 100); self._final("b", rid, 40, 90)
        self.assertEqual(lb.phase, "RESULTS")
        self.msgs()
        self.lm.on_message("a", {"t": "back_in_sel", "round": rid}, 105.0)
        self.assertTrue(self.msgs(t="round_end"))
        self.assertEqual(lb.phase, "SELECT")

    def test_leader_back_playing_deferred(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._final("a", rid, 50, 100)
        self.lm.on_message("a", {"t": "back_in_sel", "round": rid}, 104.5)
        self.assertEqual(lb.phase, "PLAYING")       # b still playing
        self.assertTrue(lb.round.leader_back)
        self.msgs()
        self._final("b", rid, 40, 90)
        self.assertEqual(lb.phase, "SELECT")        # round_end on completion
        self.assertTrue(self.msgs(t="round_end"))

    def test_bot_leader_back_only_marks(self):
        self.mk("a", "botmod x"); self.mk("b")
        lb = self._mk(2)
        rid = self._playing(lb)
        self._final("a", rid, 50, 100); self._final("b", rid, 40, 90)
        self.assertEqual(lb.phase, "RESULTS")
        self.msgs()
        self.lm.on_message("a", {"t": "back_in_sel", "round": rid}, 105.0)
        self.assertFalse(self.msgs(t="round_end"))  # bot: no round end
        self.assertTrue(self.lm._member(lb, "a").back_in_sel)
        self.assertEqual(lb.phase, "RESULTS")

    def test_follower_back_does_not_end(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._final("a", rid, 50, 100); self._final("b", rid, 40, 90)
        self.msgs()
        self.lm.on_message("b", {"t": "back_in_sel", "round": rid}, 105.0)
        self.assertFalse(self.msgs(t="round_end"))
        self.assertTrue(self.lm._member(lb, "b").back_in_sel)

    # ---- 11: owner succession + rejoin grace
    def test_owner_succession_skips_bot(self):
        self.mk("a"); self.mk("b", "bot1"); self.mk("c")
        lb = self._mk(3)
        self.assertEqual(lb.owner_cid, "a")
        self.lm.on_message("a", {"t": "lobby_leave"}, 101.0)
        self.assertEqual(lb.owner_cid, "c")         # bot b skipped
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "owner"]
        self.assertTrue(ev)
        self.assertFalse([m for _, m in self.msgs(t="event")
                          if m["kind"] == "leader"])

    def test_owner_grace_rejoin(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self.lm.on_message("a", {"t": "lobby_leave"}, 101.0)
        self.assertEqual(lb.owner_cid, "b")
        self.lm.on_message("a", {"t": "lobby_join", "code": lb.code}, 104.0)
        self.assertEqual(lb.owner_cid, "a")         # restored within 120s
        self.assertIsNone(lb.prev_owner)

    def test_owner_grace_expired(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self.lm.on_message("a", {"t": "lobby_leave"}, 101.0)
        self.assertEqual(lb.owner_cid, "b")
        self.lm.on_message("a", {"t": "lobby_join", "code": lb.code}, 222.0)
        self.assertEqual(lb.owner_cid, "b")         # grace expired

    # ---- 12: ties share the lowest position of their group
    def test_ties_1_3_3_4(self):
        for c in "abcd": self.mk(c)
        lb = self._mk(4)
        rid = self._playing(lb, "abcd")
        self._final("a", rid, 90, 100)
        self._final("b", rid, 90, 50)
        self._final("c", rid, 90, 50)
        self._final("d", rid, 80, 200)
        rows, complete = self.lm._standings(lb, 104.0)
        self.assertEqual([r["rank"] for r in rows], [1, 3, 3, 4])

    def test_tie_for_first_is_second(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        rid = self._playing(lb, "abc")
        self._final("a", rid, 90, 50)
        self._final("b", rid, 90, 50)
        self._final("c", rid, 80, 200)
        rows, complete = self.lm._standings(lb, 104.0)
        self.assertEqual([r["rank"] for r in rows], [2, 2, 3])

    # ---- 13: pause dedupe + stale
    def test_pause_same_state_sync_only(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self.lm.on_message("a", {"t": "pause_req", "round": rid}, 104.0)
        self.assertEqual(len(self.msgs(t="pause")), 2)
        # b asks pause while paused -> only b gets a sync reply
        self.lm.on_message("b", {"t": "pause_req", "round": rid}, 105.0)
        self.assertEqual(self.msgs("b", "pause"),
                         [("b", {"t": "pause", "by": "", "sync": True})])
        self.assertFalse(self.msgs("a", "pause"))
        # resume while already resumed -> same deal
        self.lm.on_message("a", {"t": "resume_req", "round": rid}, 106.0)
        self.msgs()
        self.lm.on_message("b", {"t": "resume_req", "round": rid}, 107.0)
        self.assertEqual(self.msgs("b", "resume"),
                         [("b", {"t": "resume", "by": "", "sync": True})])

    def test_pause_dup_within_half_sec_ignored(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        lb.owner_cid = "zzz"         # both members go through the vote path
        self.lm.on_message("b", {"t": "pause_req", "round": rid}, 104.0)
        self.lm.on_message("b", {"t": "pause_req", "round": rid}, 104.2)
        v = self.msgs(t="pause_vote")
        self.assertEqual(len(v), 2)   # one broadcast to both; dup ignored
        # different kind within 0.5s is not deduped
        self.lm.on_message("a", {"t": "pause_req", "round": rid}, 104.3)
        self.assertTrue(self.msgs(t="pause"))    # vote passed (need=2)

    def test_stale_suspended_while_paused_and_resume_bump(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        rid = self._playing(lb, players="abc")
        self._final("c", rid, 50, 100)           # starts the stale timer
        mb = self.lm._member(lb, "b")
        mb.last_activity = 10.0                  # long dead
        lb.group_paused = True
        rows, complete = self.lm._standings(lb, 200.0)
        self.assertFalse([r for r in rows if r["cid"] == "b"][0]["final"])
        self.assertFalse(complete)               # b not stale-complete
        lb.group_paused = False
        rows, _ = self.lm._standings(lb, 200.0)
        self.assertTrue([r for r in rows if r["cid"] == "b"][0]["final"])
        # owner resume bumps last_activity of players who had one
        lb.group_paused = True
        self.lm.on_message("a", {"t": "resume_req", "round": rid}, 201.0)
        self.assertEqual(mb.last_activity, 201.0)



class T6(T5):
    """console-reported clear/border, drop_out events, session ranking."""

    def _fin(self, c, rid, pct, score, **kw):
        msg = {"t": "final", "round": rid, "score": score, "pct": pct,
               "max_combo": 0, "jc": [0, 0, 0, 0, 0], "life": 100}
        msg.update(kw)
        self.lm.on_message(c, msg, 104.0)

    def _state(self, c, rid, life, pct=50.0, border=None):
        m = {"t": "state", "round": rid, "score": 1, "combo": 0,
             "pct": pct, "life": life}
        if border is not None:
            m["border"] = border
        self.lm.on_message(c, m, 103.5)

    def test_clear_and_border_pass_through(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._state("a", rid, 120, border=60.0)
        self.lm.tick(103.6)
        b = [m for _, m in self.msgs(t="board")][-1]
        self.assertEqual({r["cid"]: r["border"] for r in b["e"]}["a"], 60.0)
        # a passed 60 % mid-song but ended under it: the console says so
        self._fin("a", rid, 55.0, 100, border=60.0, clear=False)
        self._fin("b", rid, 45.0, 90, border=40.0, clear=True)
        rows, _ = self.lm._standings(lb, 104.0)
        by = {r["cid"]: r for r in rows}
        self.assertIs(by["a"]["clear"], False)
        self.assertEqual(by["a"]["border"], 60.0)
        self.assertIs(by["b"]["clear"], True)      # below 80 % yet cleared
        self.assertEqual(by["b"]["border"], 40.0)

    def test_old_client_clear_unknown(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._fin("a", rid, 90.0, 100)
        rows, _ = self.lm._standings(lb, 104.0)
        a = [r for r in rows if r["cid"] == "a"][0]
        self.assertIsNone(a["clear"])
        self.assertFalse(a["drop"])
        self.assertIsNone([r for r in rows if r["cid"] == "b"][0]["clear"])

    def test_drop_out_event_once(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        rid = self._playing(lb, "abc")
        self._state("a", rid, 0)                # life 0 before any life: ignored
        self.assertFalse([m for _, m in self.msgs(t="event")
                          if m["kind"] == "drop_out"])
        self._state("a", rid, 30)
        self._state("a", rid, 0)
        self._state("a", rid, 0)
        ev = [(c, m) for c, m in self.msgs(t="event") if m["kind"] == "drop_out"]
        self.assertEqual(len(ev), 3)            # once, broadcast to a, b, c
        self.assertEqual(ev[0][1]["cid"], "a")
        self._fin("a", rid, 20.0, 10, life=0, drop=True, clear=False)
        self.assertFalse([m for _, m in self.msgs(t="event")
                          if m["kind"] == "drop_out"])
        # b only reports the drop in its final
        self._fin("b", rid, 20.0, 10, life=0, drop=True, clear=False)
        ev = [m for _, m in self.msgs(t="event") if m["kind"] == "drop_out"]
        self.assertEqual({m["cid"] for m in ev}, {"b"})
        rows, _ = self.lm._standings(lb, 104.0)
        self.assertTrue([r for r in rows if r["cid"] == "b"][0]["drop"])

    def _round(self, lb, results):
        """play one round; results = {cid: (pct, score)} (leader a starts)"""
        lead = lb.leader()
        rid = lb.seq + 1
        self.lm.on_message(lead, {"t": "start_req", "pv": 1, "name": "s"},
                           101.0)
        for c in results:
            self.lm.on_message(c, {"t": "loaded", "round": rid}, 102.0)
        self.lm.tick(103.0)
        for c, (pct, score) in results.items():
            self._fin(c, rid, pct, score)
        self.assertEqual(lb.phase, "RESULTS")
        self.lm.on_message(lead, {"t": "back_in_sel", "round": rid}, 105.0)
        self.assertEqual(lb.phase, "SELECT")
        return rid

    def _st(self, lb, c):
        m = self.lm._member(lb, c)
        return m.wins, round(m.placement_score, 4), list(m.places)

    def test_session_ranking_scores(self):
        for c in "abcd": self.mk(c)
        lb = self._mk(4)
        self._round(lb, {"a": (90, 1), "b": (80, 1), "c": (70, 1), "d": (60, 1)})
        self.assertEqual(self._st(lb, "a"), (1, 1.0, [1, 0, 0]))
        self.assertEqual(self._st(lb, "b"), (0, round(2 / 3, 4), [0, 1, 0]))
        self.assertEqual(self._st(lb, "c"), (0, round(1 / 3, 4), [0, 0, 1]))
        self.assertEqual(self._st(lb, "d"), (0, 0.0, [0, 0, 0]))
        # ties average: b and c tie for 1st -> P = 1.5 each, no win
        self._round(lb, {"a": (50, 1), "b": (90, 5), "c": (90, 5), "d": (60, 1)})
        self.assertEqual(self._st(lb, "b"),
                         (0, round(2 / 3 + 2.5 / 3, 4), [0, 2, 0]))
        # shown rank of the tie is 2 (1-3-3-4 style: 2, 2) -> second places
        self.assertEqual(self._st(lb, "c"),
                         (0, round(1 / 3 + 2.5 / 3, 4), [0, 1, 1]))
        self.assertEqual(self._st(lb, "a"), (1, 1.0, [1, 0, 0]))   # 4th: +0
        self.assertEqual(self._st(lb, "d"), (0, round(1 / 3, 4), [0, 0, 1]))

    def test_session_ranking_tie_for_first_no_win(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self._round(lb, {"a": (90, 5), "b": (90, 5)})
        self.assertEqual(self._st(lb, "a"), (0, 0.5, [0, 1, 0]))
        self.assertEqual(self._st(lb, "b"), (0, 0.5, [0, 1, 0]))

    def test_session_ranking_guardrail_solo(self):
        self.mk("a")
        lb = self._mk(1)
        self._round(lb, {"a": (90, 5)})
        self.assertEqual(self._st(lb, "a"), (0, 0.0, [0, 0, 0]))

    def test_session_ranking_mid_round_leaver_last(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        rid = self._playing(lb, players="abc")
        self.lm.on_message("c", {"t": "lobby_leave"}, 103.8)
        self._fin("a", rid, 50, 1)
        self._fin("b", rid, 40, 1)
        self.assertEqual(lb.phase, "RESULTS")
        # N = 3 (leaver included, last): b (2nd of 3) still scores 0.5
        self.assertEqual(self._st(lb, "a"), (1, 1.0, [1, 0, 0]))
        self.assertEqual(self._st(lb, "b"), (0, 0.5, [0, 1, 0]))
        self.assertEqual([m.cid for m in lb.members], ["a", "b"])

    def test_session_order_and_lobby_sr(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        # b: 1 win; c: 1 win + better placements; a: none
        self._round(lb, {"a": (10, 1), "b": (90, 1), "c": (80, 1)})
        self._round(lb, {"a": (20, 1), "b": (10, 1), "c": (90, 1)})
        self.assertEqual([m.cid for m in self.lm.session_order(lb)],
                         ["c", "b", "a"])
        lob = self.msgs("a", "lobby")[-1][1]
        sr = {m["cid"]: m["sr"] for m in lob["members"]}
        self.assertEqual(sr["c"], [1, 1, 1, 1, 0])
        self.assertEqual(sr["b"], [2, 1, 1, 0, 1])
        self.assertEqual(sr["a"], [3, 0, 0, 1, 1])
        # a late joiner starts at zero, after everyone with points
        self.mk("d")
        self.lm.on_message("d", {"t": "lobby_join", "code": lb.code}, 106.0)
        lob = self.msgs("d", "lobby")[-1][1]
        self.assertEqual({m["cid"]: m["sr"] for m in lob["members"]}["d"],
                         [4, 0, 0, 0, 0])

    def test_ranking_applied_once(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._fin("a", rid, 90, 1)
        self._fin("b", rid, 80, 1)
        self.lm.on_disconnect("b", 104.5)       # re-checks completion
        self._fin("a", rid, 90, 1)              # duplicate final
        self.assertEqual(self._st(lb, "a"), (1, 1.0, [1, 0, 0]))


    def test_session_kept_on_rejoin_until_lobby_empties(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        self._round(lb, {"a": (90, 1), "b": (80, 1), "c": (70, 1)})
        self.lm.on_message("b", {"t": "lobby_leave"}, 106.0)
        self.assertNotIn("b", [m.cid for m in lb.members])
        self.lm.on_message("b", {"t": "lobby_join", "code": lb.code}, 107.0)
        self.assertEqual(self._st(lb, "b"), (0, 0.5, [0, 1, 0]))
        lob = self.msgs("b", "lobby")[-1][1]
        self.assertEqual({m["cid"]: m["sr"] for m in lob["members"]}["b"],
                         [2, 0, 0, 1, 0])
        # everyone leaves: the lobby closes and its memory goes with it
        code = lb.code
        for c in "abc":
            self.lm.on_message(c, {"t": "lobby_leave"}, 108.0)
        self.assertNotIn(code, self.lm.lobbies)
        self.lm.on_message("a", {"t": "lobby_create"}, 109.0)
        lb2 = self.lm.lobbies[self.lm.by_cid["a"].code]
        self.assertEqual(self._st(lb2, "a"), (0, 0.0, [0, 0, 0]))


class T7(T6):
    """platform, per-member difficulty, same-difficulty room option,
       missing-song flags."""

    def _lobby(self, cid):
        return self.msgs(cid, "lobby")[-1][1]

    def _lobbies(self):
        """last lobby message per recipient, draining the outbox"""
        return {c: m for c, m in self.msgs(t="lobby")}

    def test_platform_of(self):
        from lobby import platform_of
        self.assertEqual(platform_of("0.1.0-dev+Nov 1", "1.0.6"), "switch")
        self.assertEqual(platform_of("pc-0.1.0-dev+Nov 1", "mm+1.03"), "pc")
        self.assertEqual(platform_of("0.1.0", "mm+1.03"), "pc")
        self.assertEqual(platform_of("bot-1", "1.0.6"), "bot")
        self.assertEqual(platform_of("bot-1", "1.0.6", "pc"), "pc")
        self.assertEqual(platform_of("pc-x", "mm+1.03", "nonsense"), "pc")
        self.assertEqual(platform_of(None, None), "switch")

    def test_plat_in_lobby_msg(self):
        self.lm.on_hello("a", "a", "0.1.0-dev", 100.0, game="1.0.6")
        self.lm.on_hello("b", "b", "pc-0.1.0-dev", 100.0, game="mm+1.03")
        self.lm.on_hello("c", "c", "bot-1", 100.0)
        self.lm.on_hello("d", "d", "bot-1", 100.0, plat="pc")
        self._mk(4)
        plats = {m["cid"]: m["plat"] for m in self._lobby("a")["members"]}
        self.assertEqual(plats, {"a": "switch", "b": "pc", "c": "bot",
                                 "d": "pc"})

    def test_diff_reported_and_rebroadcast(self):
        for c in "ab": self.mk(c)
        self._mk(2)
        self.msgs()
        self.lm.on_message("b", {"t": "diff", "d": 3, "e": 1}, 100.5)
        mem = {m["cid"]: m for m in self._lobby("a")["members"]}
        self.assertEqual((mem["b"]["diff"], mem["b"]["ed"]), (3, 1))
        self.assertEqual((mem["a"]["diff"], mem["a"]["ed"]), (-1, 0))
        # unchanged or invalid values don't resend the lobby
        self.lm.on_message("b", {"t": "diff", "d": 3, "e": 1}, 100.6)
        self.lm.on_message("b", {"t": "diff", "d": 9}, 100.6)
        self.lm.on_message("b", {"t": "diff", "d": True}, 100.6)
        self.assertEqual(self.msgs(t="lobby"), [])
        # the EXTRA edition only exists on EXTREME
        self.lm.on_message("b", {"t": "diff", "d": 1, "e": 1}, 100.7)
        mem = {m["cid"]: m for m in self._lobby("a")["members"]}
        self.assertEqual((mem["b"]["diff"], mem["b"]["ed"]), (1, 0))

    def test_round_difficulty_frozen_for_board_and_standings(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self.lm.on_message("a", {"t": "diff", "d": 2}, 100.5)
        self.lm.on_message("b", {"t": "diff", "d": 1}, 100.5)
        rid = lb.seq + 1
        self.lm.on_message("a", {"t": "start_req", "pv": 1, "name": "s"},
                           101.0)
        # a reports the chart it loaded, b (older client) reports nothing
        self.lm.on_message("a", {"t": "loaded", "round": rid, "diff": 3,
                                 "ed": 1}, 102.0)
        self.lm.on_message("b", {"t": "loaded", "round": rid}, 102.0)
        self.lm.tick(103.0)
        self._state("a", rid, 120)
        self.lm.tick(103.6)
        b = [m for _, m in self.msgs(t="board")][-1]
        self.assertEqual({r["cid"]: (r["diff"], r["ed"]) for r in b["e"]},
                         {"a": (3, 1), "b": (1, 0)})
        self._fin("b", rid, 80.0, 10)
        # b is back on song select and flips its difficulty: the round's
        # standings keep the chart it played
        self.lm.on_message("b", {"t": "diff", "d": 0}, 104.2)
        self._fin("a", rid, 90.0, 20, diff=3, ed=1)
        rows, _ = self.lm._standings(lb, 104.5)
        self.assertEqual({r["cid"]: (r["diff"], r["ed"]) for r in rows},
                         {"a": (3, 1), "b": (1, 0)})
        # after the round the member list shows the live value again
        self.msgs()
        self.lm.on_message("a", {"t": "back_in_sel", "round": rid}, 105.0)
        mem = {m["cid"]: m for m in self._lobby("a")["members"]}
        self.assertEqual(mem["b"]["diff"], 0)

    def test_old_clients_have_no_difficulty(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._fin("a", rid, 90.0, 20)
        self._fin("b", rid, 80.0, 10)
        rows, _ = self.lm._standings(lb, 104.5)
        self.assertTrue(all(r["diff"] == -1 and r["ed"] == 0 for r in rows))

    def test_same_diff_owner_only(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self.assertIs(self._lobby("b")["same_diff"], False)
        self.lm.on_message("b", {"t": "set_opt", "key": "same_diff",
                                 "value": True}, 101.0)
        self.assertEqual(self.msgs("b", "lobby_error")[0][1]["code"],
                         "not_owner")
        self.assertFalse(lb.same_diff)
        for bad in ({"key": "nope", "value": True},
                    {"key": "same_diff", "value": 1}):
            self.lm.on_message("a", dict(t="set_opt", **bad), 101.0)
            self.assertEqual(self.msgs("a", "lobby_error")[0][1]["code"],
                             "bad_opt")
        self.lm.on_message("a", {"t": "set_opt", "key": "same_diff",
                                 "value": True}, 101.5)
        out = self.msgs()
        self.assertTrue(lb.same_diff)
        self.assertTrue(all(m["same_diff"] for c, m in out
                            if m["t"] == "lobby"))
        ev = [m for c, m in out if m["t"] == "event" and c == "b"]
        self.assertEqual([(e["kind"], e["value"]) for e in ev],
                         [("same_diff", 1)])
        # setting it again is silent
        self.lm.on_message("a", {"t": "set_opt", "key": "same_diff",
                                 "value": True}, 101.6)
        self.assertEqual(self.msgs(), [])
        # the admin port can flip it too
        self.assertEqual(self.lm.on_admin(
            {"cmd": "set_opt", "code": lb.code, "key": "same_diff",
             "value": False}, 102.0), {"ok": True})
        self.assertFalse(lb.same_diff)
        self.assertIn("error", self.lm.on_admin(
            {"cmd": "set_opt", "code": "ZZZZ", "key": "same_diff",
             "value": False}, 102.0))

    def test_nosong_flags_everyone_and_clears_on_new_pick(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        self.lm.on_message("a", {"t": "sel", "pv": 700, "name": "X"}, 101.0)
        self.msgs()
        nack = {"t": "sel_ack", "pv": 700, "has": False, "ok": False,
                "fb_pv": -1}
        self.lm.on_message("b", nack, 101.2)
        got = self._lobbies()
        self.assertEqual(set(got), {"a", "b", "c"})   # not just the picker
        for viewer in "abc":
            self.assertEqual(
                {m["cid"]: m["nosong"] for m in got[viewer]["members"]},
                {"a": False, "b": True, "c": False})
        # a repeat of the same ack doesn't resend the lobby
        self.lm.on_message("b", nack, 101.3)
        self.assertEqual(self.msgs(t="lobby"), [])
        # c has it: nothing changes for the flags
        self.lm.on_message("c", {"t": "sel_ack", "pv": 700, "has": True,
                                 "ok": True, "fb_pv": -1}, 101.4)
        self.assertEqual(self.msgs(t="lobby"), [])
        # the picker moves on: the flag is gone for everyone
        self.lm.on_message("a", {"t": "sel", "pv": 5, "name": "Y"}, 102.0)
        mem = {m["cid"]: m["nosong"] for m in self._lobby("c")["members"]}
        self.assertEqual(mem, {"a": False, "b": False, "c": False})
        # a stale ack for the old song is ignored
        self.lm.on_message("b", nack, 102.1)
        self.assertEqual(self.lm.nosong(lb), ())

    def test_picker_is_never_flagged_nosong(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        self.lm.on_message("a", {"t": "sel", "pv": 700, "name": "X"}, 101.0)
        self.lm.on_message("a", {"t": "sel_ack", "pv": 700, "has": False,
                                 "ok": False, "fb_pv": -1}, 101.2)
        self.assertEqual(self.lm.nosong(lb), ())

    def test_previous_round_activity_is_not_stale(self):
        for c in "ab": self.mk(c)
        lb = self._mk(2)
        rid = self._playing(lb)
        self._state("a", rid, 120)
        self._state("b", rid, 120)
        self._fin("a", rid, 90.0, 20)
        self._fin("b", rid, 80.0, 10)
        self.lm.on_message("a", {"t": "back_in_sel", "round": rid}, 105.0)
        self.assertEqual(lb.phase, "SELECT")
        # round 2, minutes later: b quits at once, a hasn't scored yet
        self.lm.on_message(lb.leader(), {"t": "start_req", "pv": 1,
                                         "name": "s"}, 400.0)
        rid2 = lb.round.id
        for c in "ab":
            self.lm.on_message(c, {"t": "loaded", "round": rid2}, 401.0)
        self.lm.tick(402.0)
        self.lm.on_message("b", {"t": "final", "round": rid2, "score": 0,
                                 "pct": 0.0, "max_combo": 0, "jc": [0] * 5,
                                 "life": 100, "quit": True}, 402.5)
        self.assertEqual(lb.phase, "PLAYING")       # a is still playing
        rows, complete = self.lm._standings(lb, 402.5)
        self.assertFalse(complete)

    def test_standings_carry_judgement_counts(self):
        for c in "abc": self.mk(c)
        lb = self._mk(3)
        self.lm.on_message("b", {"t": "autoplay", "on": True, "pct": 96},
                           100.5)
        rid = self._playing(lb, "abc")
        self._fin("a", rid, 90.0, 20, jc=[400, 30, 5, 2, 3])
        # junk from a client: wrong types, negatives, too long / too short
        self._fin("b", rid, 80.0, 10, jc=[7, "x", -4, True, 2.5, 9, 9])
        st = [m for _, m in self.msgs("a", "standings")][-1]
        by = {r["cid"]: r for r in st["e"]}
        self.assertEqual(by["a"]["jc"], [400, 30, 5, 2, 3])
        self.assertEqual(by["b"]["jc"], [7, 0, 0, 0, 0])
        self.assertEqual(by["c"]["jc"], [])          # no final yet
        self.assertEqual({c: r["autoplay"] for c, r in by.items()},
                         {"a": False, "b": True, "c": False})
        self._fin("c", rid, 70.0, 5, jc=[1, 2])
        rows, _ = self.lm._standings(lb, 104.5)
        self.assertEqual({r["cid"]: r["jc"] for r in rows}["c"],
                         [1, 2, 0, 0, 0])
        # the web member snapshot gets the same sanitized list
        self.assertEqual(self.lm._member(lb, "b").final["jc"], [7, 0, 0, 0, 0])
