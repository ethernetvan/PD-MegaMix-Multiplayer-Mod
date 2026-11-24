#!/usr/bin/env python3
"""Interactive MP test bot for the MegaMix server.

Startup flags become defaults; while running it reads one-line commands
from stdin (see `help`). Prints every server message in readable form.
"""

import argparse
import asyncio
import json
import random
import sys
import threading
import time

# song names contain characters (e.g. "♪") that a cp1252 console can't print
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

BASE_PVS = [249, 1, 3, 8, 15, 21, 30, 44, 55, 100]
BOT_BORDER = 60.0   # clear % a bot reports (real consoles send their song's)


def load_pvs():
    out = []
    try:
        for ln in open(r"C:\Users\cl672\Documents\Coding\mm-re\re\pv_list.txt",
                       encoding="utf-8", errors="replace"):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split(None, 1)
            if parts[0].isdigit() and len(parts) > 1 and parts[1]:
                out.append((int(parts[0]), parts[1]))
    except OSError:
        pass
    return out or [(p, f"PV{p}") for p in BASE_PVS]


class Bot:
    def __init__(self, a):
        self.a = a
        self.name = a.name
        self.cid = "bot-" + "".join(random.choices("0123456789abcdef", k=14))
        self.code = None
        self.lobby = None
        self.phase = "?"
        self.leader = None
        self.auto = a.behavior in ("leader", "follower", "missing")
        self.diff = (a.diff, 1 if a.extra and a.diff == 3 else 0)
        self.playing = False
        self.round = 0
        self.pvs = load_pvs()
        self.pv_idx = 0
        self.w = None
        self._song_task = None
        self.ready = True
        self.paused = False        # paused inside a round, clock held
        self.scene = None          # last reported scene: sel | game | results
        self.sel_pv = 0            # current pick for `start`
        self.sel_name = ""
        self.open_vote = None      # (kind, round) while a vote is open
        self.final_round = -1    # last round we sent `final` for
        self.back_round = -1     # last round we sent `back_in_sel` for

    def send(self, m):
        self.w.write((json.dumps(m) + "\n").encode())

    def hello(self):
        h = {"t": "hello", "cid": self.cid, "name": self.name,
             "proto": 1, "mod": "bot-1", "game": "1.0.6"}
        if self.a.plat:
            h["plat"] = self.a.plat      # pose as a console of that version
        self.send(h)
        self.send({"t": "avatar", "module": self.a.module})
        self.send_diff()

    def send_diff(self):
        if self.diff[0] >= 0:
            self.send({"t": "diff", "d": self.diff[0], "e": self.diff[1]})

    def diff_fields(self):
        return ({"diff": self.diff[0], "ed": self.diff[1]}
                if self.diff[0] >= 0 else {})

    def set_scene(self, where):
        if where != self.scene:
            self.scene = where
            self.send({"t": "scene", "where": where})

    async def _reconnect(self):
        try:
            r, self.w = await asyncio.open_connection(self.a.host,
                                                      self.a.port)
        except OSError as e:
            print(f"!! reconnect failed: {e} - retrying in 3s")
            await asyncio.sleep(3)
            asyncio.ensure_future(self._reconnect())
            return
        asyncio.ensure_future(self.reader(r))
        self.hello()
        print("-- reconnected")

    async def run(self):
        try:
            r, self.w = await asyncio.open_connection(self.a.host,
                                                      self.a.port)
        except OSError as e:
            print(f"!! connect failed: {e} - retrying in 3s")
            await asyncio.sleep(3)
            asyncio.ensure_future(self._reconnect())
            await asyncio.Event().wait()
            return
        asyncio.ensure_future(self.reader(r))
        self.hello()
        await asyncio.sleep(0.3)
        if self.a.create:
            self.send({"t": "lobby_create"})
        elif self.a.join:
            self.send({"t": "lobby_join", "code": self.a.join.upper()})
        await asyncio.Event().wait()          # run forever

    # ---------------------------------------------------- incoming
    async def reader(self, r):
        try:
            async for raw in r:
                try:
                    m = json.loads(raw)
                except ValueError:
                    continue
                asyncio.ensure_future(self.on_msg(m))
        except (ConnectionError, asyncio.IncompleteReadError) as e:
            print(f"!! conn error: {e}")
        print("!! disconnected - reconnecting in 3s "
              "(type 'reconnect' to retry now)")
        await asyncio.sleep(3)
        asyncio.ensure_future(self._reconnect())

    def members_line(self, m):
        return ", ".join(
            f"{x['name']}{'(L)' if x['cid'] == self.leader else ''}"
            f"{'[dc]' if not x['connected'] else ''}"
            f"[{x.get('role', '?')}]"
            f"{'[auto]' if x.get('autoplay') else ''}"
            for x in m.get("members", []))

    async def on_msg(self, m):
        t = m.get("t")
        if t == "welcome":
            print(f"-- welcome, cid={self.cid}")
        elif t == "lobby":
            self.code = m["code"]
            self.lobby = m
            self.phase = m["phase"]
            self.leader = m["leader"]
            print(f"-- lobby {self.code} {self.phase} "
                  f"leader={self.leader} owner={m.get('owner')} "
                  f"mode={m.get('pick_mode')} :: {self.members_line(m)}")
            if self.a.create:
                print(f"LOBBY_CODE {self.code}", flush=True)
            if self.scene is None:
                self.set_scene("sel")
        elif t == "lobby_left":
            print(f"-- lobby_left {m.get('reason')}")
            self.code = None
        elif t == "lobby_error":
            print(f"-- lobby_error {m.get('code')} {m.get('msg', '')}")
        elif t == "sel":
            print(f"-- sel pv={m['pv']} '{m.get('name', '')}'")
            if self.a.behavior == "missing" and self.leader != self.cid:
                # a console without this song: no chart to jump to
                self.send({"t": "sel_ack", "pv": m["pv"], "has": False,
                           "ok": False, "fb_pv": -1, "fb_name": ""})
                self.send({"t": "ready", "pv": m["pv"], "ready": False,
                           "reason": "wrong_song"})
            elif self.auto and self.leader != self.cid:
                self.send({"t": "sel_ack", "pv": m["pv"], "has": True,
                           "ok": True, "fb_pv": -1, "fb_name": ""})
                self.send({"t": "ready", "pv": m["pv"], "ready": self.ready,
                           "reason": "" if self.ready else "not_in_sel"})
        elif t == "sel_status":
            print(f"-- sel_status missing={m.get('missing')} "
                  f"cant={m.get('cant')} not_ready={m.get('not_ready')}")
        elif t == "launch":
            self.round = m["round"]
            me_player = self.cid in m.get("players", [])
            print(f"-- launch r{self.round} pv={m['pv']} "
                  f"players={m['players']} me={'player' if me_player else 'spec'}")
            if (self.a.behavior == "missing" and me_player and
                    self.leader != self.cid):
                self.send({"t": "launch_fail", "round": self.round})
            elif self.auto and me_player:
                asyncio.ensure_future(self._load_and_play())
        elif t == "start":
            print(f"-- start r{m['round']} dropped={m.get('dropped')}")
            if self.auto and self.cid not in m.get("dropped", []):
                self.playing = True
                self.paused = False
                self.set_scene("game")
        elif t == "board":
            top = ", ".join(f"{e['name']}={e['score']}"
                            for e in m.get("e", [])[:3])
            print(f"-- board r{m['round']}: {top}")
        elif t == "pause_vote":
            print(f"-- {m['kind']} vote {len(m.get('agreed', []))}/"
                  f"{m['need']} ends_in={m.get('ends_in')}"
                  f"{' EXPIRED' if m.get('expired') else ''}")
            self.open_vote = (None if m.get("expired")
                              else (m["kind"], self.round))
            if self.auto and not m.get("expired") and \
                    self.cid not in m.get("agreed", []):
                if self.a.vote == "instant":
                    self.send({"t": f"{m['kind']}_req", "round": self.round})
                elif self.a.vote == "delay":
                    asyncio.ensure_future(
                        self._vote_later(m["kind"], self.round))
        elif t in ("pause", "resume"):
            print(f"-- {t} by {m.get('by')}")
            self.open_vote = None
            if t == "pause":
                self.paused = True
            elif self.auto:
                self.paused = False
        elif t == "event":
            print(f"-- event {m.get('kind')} {m.get('name')} {m.get('value', '')}")
        elif t == "standings":
            print(f"-- standings r{m['round']} complete={m['complete']}: "
                  + ", ".join(f"#{e['rank']} {e['name']}={e['score']}"
                              for e in m.get("e", [])))
            if self.auto and \
                    (self.playing or self.final_round == m["round"]) and \
                    self.back_round != m["round"]:
                self.back_round = m["round"]
                await asyncio.sleep(2)
                self.send({"t": "back_in_sel", "round": m["round"]})
                self.set_scene("sel")
        elif t == "round_end":
            print(f"-- round_end r{m['round']} next={m.get('next_leader')}")
            self.playing = False
            self.paused = False
            self.open_vote = None
            self.set_scene("sel")
            if self.auto and m.get("next_leader") == self.cid:
                await self._leader_pick()
        elif t == "error":
            print(f"!! server error: {m}")
        elif t == "dbg_cmd":
            pass
        else:
            print(f"-- {t}: {m}")

    async def _load_and_play(self):
        await asyncio.sleep(self.a.load_delay)
        self.send({"t": "loaded", "round": self.round, **self.diff_fields()})
        print(f"-- sent loaded r{self.round}")

    async def _leader_pick(self):
        await asyncio.sleep(1)
        self.pv_idx += 1                  # next named song every round
        pv, name = self.pvs[self.pv_idx % len(self.pvs)]
        self.sel_pv, self.sel_name = pv, name
        self.send({"t": "sel", "pv": pv, "name": name})
        print(f">> sel {pv} '{name}'")
        await asyncio.sleep(self.a.start_after)
        self.send({"t": "start_req", "pv": self.sel_pv,
                   "name": self.sel_name})
        print(f">> start_req {self.sel_pv}")

    async def _vote_later(self, kind, rnd):
        await asyncio.sleep(self.a.vote_delay)
        if self.open_vote == (kind, rnd):
            self.send({"t": f"{kind}_req", "round": rnd})
            print(f">> {kind}_req r{rnd} (after {self.a.vote_delay:.1f}s)")

    async def _play_song(self):
        score, combo, elapsed = 0, 0, 0.0
        last = time.time()
        while self.playing and elapsed < self.a.song_secs:
            now = time.time()
            if not self.paused:
                elapsed += now - last
                score += int(random.randint(400, 900) * self.a.speed)
                combo += random.randint(1, 4)
                life = 127 if random.random() > 0.05 else random.randint(10, 60)
                self.send({"t": "state", "round": self.round, "score": score,
                           "combo": combo, "pct": min(100.0, score / 3000.0),
                           "life": life, "border": BOT_BORDER})
            last = now
            await asyncio.sleep(0.2)
        if self.playing:
            self.playing = False
            self.set_scene("results")
            pct = min(100.0, score / 3000.0)
            self.send({"t": "final", "round": self.round, "score": score,
                       "pct": pct, "max_combo": combo,
                       "jc": [5, 10, 8, 4, 2], "life": 80,
                       "border": BOT_BORDER, "clear": pct >= BOT_BORDER,
                       **self.diff_fields()})
            self.final_round = self.round
            print(f"-- sent final r{self.round}")

    async def state_loop(self):
        while True:
            if self.playing and self._song_task is None:
                self._song_task = asyncio.ensure_future(self._play_song())
                self._song_task.add_done_callback(
                    lambda _: setattr(self, "_song_task", None))
            await asyncio.sleep(0.2)

    # ---------------------------------------------------- commands
    async def cmd(self, line):
        p = line.split()
        if not p:
            return
        c, arg = p[0].lower(), p[1:]
        if c == "help":
            print("help status name NEW create join CODE leave auto on|off "
                  "ready on|off load fail final pause resume "
                  "mode owner|assigned|rotate [cid] next prev pick X start "
                  "diff 0..3 [extra] samediff on|off")
        elif c == "status":
            print(f"code={self.code} phase={self.phase} "
                  f"leader={self.leader} auto={self.auto} cid={self.cid}")
        elif c == "name" and arg:
            self.name = arg[0]
            self.send({"t": "set_name", "name": self.name})
        elif c == "create":
            self.send({"t": "lobby_create"})
        elif c == "join" and arg:
            self.send({"t": "lobby_join", "code": arg[0].upper()})
        elif c == "leave":
            self.send({"t": "lobby_leave"})
        elif c == "auto" and arg:
            self.auto = arg[0] == "on"
            print(f">> auto={self.auto}")
        elif c == "ready" and arg:
            self.ready = arg[0] == "on"
            self.send({"t": "ready", "pv": 0, "ready": self.ready,
                       "reason": "" if self.ready else "not_in_sel"})
        elif c == "load":
            self.send({"t": "loaded", "round": self.round})
        elif c == "fail":
            self.send({"t": "final", "round": self.round, "score": 0,
                       "pct": 0, "max_combo": 0, "jc": [0]*5, "life": 0,
                       "border": BOT_BORDER, "clear": False, "drop": True})
        elif c == "final":
            self.send({"t": "final", "round": self.round, "score": 9999,
                       "pct": 42.0, "max_combo": 10, "jc": [1]*5, "life": 90,
                       "border": BOT_BORDER, "clear": 42.0 >= BOT_BORDER})
        elif c == "pause":
            self.send({"t": "pause_req", "round": self.round})
        elif c == "resume":
            self.send({"t": "resume_req", "round": self.round})
        elif c == "diff" and arg and arg[0].lstrip("-").isdigit():
            d = int(arg[0])
            self.diff = (d, 1 if len(arg) > 1 and d == 3 else 0)
            self.send_diff()
        elif c == "samediff" and arg:
            self.send({"t": "set_opt", "key": "same_diff",
                       "value": arg[0] == "on"})
        elif c == "mode" and arg:
            m = {"t": "set_pick_mode", "mode": arg[0]}
            if len(arg) > 1:
                m["cid"] = arg[1]
            self.send(m)
        elif c == "next":
            self.pv_idx += 1
            pv, name = self.pvs[self.pv_idx % len(self.pvs)]
            self.sel_pv, self.sel_name = pv, name
            self.send({"t": "sel", "pv": pv, "name": name})
            print(f">> sel {pv} '{name}'")
        elif c == "prev":
            self.pv_idx -= 1
            pv, name = self.pvs[self.pv_idx % len(self.pvs)]
            self.sel_pv, self.sel_name = pv, name
            self.send({"t": "sel", "pv": pv, "name": name})
            print(f">> sel {pv} '{name}'")
        elif c == "pick" and arg:
            q = " ".join(arg).lower()
            hit = next(((pv, n) for pv, n in self.pvs
                        if q == str(pv) or q in n.lower()), None)
            if hit:
                pv, name = hit
                self.sel_pv, self.sel_name = pv, name
                self.send({"t": "sel", "pv": pv, "name": name})
                print(f">> sel {pv} '{name}'")
            else:
                print("no match")
        elif c == "reconnect":
            asyncio.ensure_future(self._reconnect())
        elif c == "start":
            pv, name = (self.sel_pv, self.sel_name) if self.sel_pv \
                else self.pvs[self.pv_idx % len(self.pvs)]
            self.send({"t": "start_req", "pv": pv, "name": name})
            print(f">> start_req {pv}")
        else:
            print("? - type help")


def stdin_thread(loop, bot):
    for line in sys.stdin:
        asyncio.run_coroutine_threadsafe(bot.cmd(line.strip()), loop)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="Bot")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--create", action="store_true")
    ap.add_argument("--join", metavar="CODE")
    ap.add_argument("--behavior", choices=["leader", "follower", "missing",
                                           "manual"], default="manual")
    ap.add_argument("--sels", default="")
    ap.add_argument("--start-after", type=float, default=8)
    ap.add_argument("--load-delay", type=float, default=3)
    ap.add_argument("--song-secs", type=float, default=60)
    ap.add_argument("--fail-at", type=float, default=0)
    ap.add_argument("--speed", type=float, default=1.0,
                    help="score/percentage gain multiplier (race tests)")
    ap.add_argument("--vote", choices=["delay", "never", "instant"],
                    default="delay",
                    help="how auto bots answer pause/resume votes")
    ap.add_argument("--vote-delay", type=float, default=3.0,
                    help="seconds to wait before agreeing an open vote")
    ap.add_argument("--module", type=int, default=0,
                    help="avatar module id (Shared Set A slot 1); 0 = Miku")
    ap.add_argument("--plat", choices=["switch", "pc"], default=None,
                    help="announce this mod version (party-screen icon); "
                         "default: a bot")
    ap.add_argument("--diff", type=int, default=2, choices=[-1, 0, 1, 2, 3],
                    help="difficulty the bot plays: 0 EASY .. 3 EXTREME, "
                         "-1 = don't report one (old client)")
    ap.add_argument("--extra", action="store_true",
                    help="with --diff 3: EXTRA EXTREME")
    a = ap.parse_args()

    bot = Bot(a)
    if a.sels:
        try:
            a._sels = [int(x) for x in a.sels.split(",")]
        except ValueError:
            a._sels = []

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.create_task(bot.run())
    loop.create_task(bot.state_loop())
    threading.Thread(target=stdin_thread, args=(loop, bot), daemon=True).start()
    print("bot ready - type 'help'")
    loop.run_forever()


if __name__ == "__main__":
    import os
    main()
