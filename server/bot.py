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

BASE_PVS = [249, 1, 3, 8, 15, 21, 30, 44, 55, 100]


def load_pvs():
    out = []
    try:
        for ln in open(r"C:\Users\cl672\Documents\Coding\mm-re\re\pv_list.txt",
                       encoding="utf-8", errors="replace"):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split(None, 1)
            if parts[0].isdigit():
                out.append((int(parts[0]),
                            parts[1] if len(parts) > 1 else parts[0]))
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
        self.auto = a.behavior == "leader" or a.behavior == "follower"
        self.playing = False
        self.round = 0
        self.pvs = load_pvs()
        self.pv_idx = 0
        self.w = None
        self._song_task = None
        self.ready = True

    def send(self, m):
        self.w.write((json.dumps(m) + "\n").encode())

    async def run(self):
        r, self.w = await asyncio.open_connection(self.a.host, self.a.port)
        asyncio.ensure_future(self.reader(r))
        self.send({"t": "hello", "cid": self.cid, "name": self.name,
                   "mod": "bot/1", "game": "1.0.6"})
        await asyncio.sleep(0.3)
        if self.a.create:
            self.send({"t": "lobby_create"})
        elif self.a.join:
            self.send({"t": "lobby_join", "code": self.a.join.upper()})
        await asyncio.Event().wait()          # run forever

    # ---------------------------------------------------- incoming
    async def reader(self, r):
        async for raw in r:
            try:
                m = json.loads(raw)
            except ValueError:
                continue
            asyncio.ensure_future(self.on_msg(m))
        print("!! disconnected")
        os._exit(1) if False else sys.exit(1)

    def members_line(self, m):
        return ", ".join(
            f"{x['name']}{'(L)' if x['cid'] == self.leader else ''}"
            f"{'[dc]' if not x['connected'] else ''}"
            f"[{x.get('role', '?')}]" for x in m.get("members", []))

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
        elif t == "lobby_left":
            print(f"-- lobby_left {m.get('reason')}")
            self.code = None
        elif t == "lobby_error":
            print(f"-- lobby_error {m.get('code')} {m.get('msg', '')}")
        elif t == "sel":
            print(f"-- sel pv={m['pv']} '{m.get('name', '')}'")
            if self.auto and self.leader != self.cid:
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
            if self.auto and me_player:
                asyncio.ensure_future(self._load_and_play())
        elif t == "start":
            print(f"-- start r{m['round']} dropped={m.get('dropped')}")
            if self.auto and self.cid not in m.get("dropped", []):
                self.playing = True
        elif t == "board":
            top = ", ".join(f"{e['name']}={e['score']}"
                            for e in m.get("e", [])[:3])
            print(f"-- board r{m['round']}: {top}")
        elif t == "pause_vote":
            print(f"-- {m['kind']} vote {len(m.get('agreed', []))}/"
                  f"{m['need']} ends_in={m.get('ends_in')}"
                  f"{' EXPIRED' if m.get('expired') else ''}")
            if self.auto and not m.get("expired") and \
                    self.cid not in m.get("agreed", []):
                self.send({"t": f"{m['kind']}_req", "round": self.round})
        elif t in ("pause", "resume"):
            print(f"-- {t} by {m.get('by')}")
            if t == "pause":
                self.playing = False
            elif self.auto:
                self.playing = True
        elif t == "event":
            print(f"-- event {m.get('kind')} {m.get('name')} {m.get('value', '')}")
        elif t == "standings":
            print(f"-- standings r{m['round']} complete={m['complete']}: "
                  + ", ".join(f"#{e['rank']} {e['name']}={e['score']}"
                              for e in m.get("e", [])))
            if self.auto and self.playing:
                await asyncio.sleep(2)
                self.send({"t": "back_in_sel", "round": m["round"]})
        elif t == "round_end":
            print(f"-- round_end r{m['round']} next={m.get('next_leader')}")
            self.playing = False
            if self.auto and m.get("next_leader") == self.cid:
                await self._leader_pick()
        elif t == "dbg_cmd":
            pass
        else:
            print(f"-- {t}: {m}")

    async def _load_and_play(self):
        await asyncio.sleep(self.a.load_delay)
        self.send({"t": "loaded", "round": self.round})
        print(f"-- sent loaded r{self.round}")

    async def _leader_pick(self):
        await asyncio.sleep(1)
        pv, name = self.pvs[self.pv_idx % len(self.pvs)]
        self.send({"t": "sel", "pv": pv, "name": name})
        print(f">> sel {pv} '{name}'")
        await asyncio.sleep(self.a.start_after)
        self.send({"t": "start_req", "pv": pv, "name": name})
        print(f">> start_req {pv}")

    async def _play_song(self):
        score, combo = 0, 0
        end = time.time() + self.a.song_secs
        while self.playing and time.time() < end:
            score += random.randint(400, 900)
            combo += random.randint(1, 4)
            life = 127 if random.random() > 0.05 else random.randint(10, 60)
            self.send({"t": "state", "round": self.round, "score": score,
                       "combo": combo, "pct": min(100.0, score / 3000.0),
                       "life": life})
            await asyncio.sleep(0.2)
        if self.playing:
            self.playing = False
            self.send({"t": "final", "round": self.round, "score": score,
                       "pct": min(100.0, score / 3000.0), "max_combo": combo,
                       "jc": [5, 10, 8, 4, 2], "life": 80})
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
                  "mode owner|assigned|rotate [cid] next prev pick X start")
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
                       "pct": 0, "max_combo": 0, "jc": [0]*5, "life": 0})
        elif c == "final":
            self.send({"t": "final", "round": self.round, "score": 9999,
                       "pct": 42.0, "max_combo": 10, "jc": [1]*5, "life": 90})
        elif c == "pause":
            self.send({"t": "pause_req", "round": self.round})
        elif c == "resume":
            self.send({"t": "resume_req", "round": self.round})
        elif c == "mode" and arg:
            m = {"t": "set_pick_mode", "mode": arg[0]}
            if len(arg) > 1:
                m["cid"] = arg[1]
            self.send(m)
        elif c == "next":
            self.pv_idx += 1
            pv, name = self.pvs[self.pv_idx % len(self.pvs)]
            self.send({"t": "sel", "pv": pv, "name": name})
            print(f">> sel {pv} '{name}'")
        elif c == "prev":
            self.pv_idx -= 1
            pv, name = self.pvs[self.pv_idx % len(self.pvs)]
            self.send({"t": "sel", "pv": pv, "name": name})
            print(f">> sel {pv} '{name}'")
        elif c == "pick" and arg:
            q = " ".join(arg).lower()
            hit = next(((pv, n) for pv, n in self.pvs
                        if q == str(pv) or q in n.lower()), None)
            if hit:
                pv, name = hit
                self.send({"t": "sel", "pv": pv, "name": name})
                print(f">> sel {pv} '{name}'")
            else:
                print("no match")
        elif c == "start":
            pv, name = self.pvs[self.pv_idx % len(self.pvs)]
            self.send({"t": "start_req", "pv": pv, "name": name})
            print(f">> start_req {pv}")
        else:
            print("? - type help")


async def amain(a):
    bot = Bot(a)
    await bot.run()


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
    a = ap.parse_args()

    bot = Bot(a)
    if a.sels:
        try:
            a._sels = [int(x) for x in a.sels.split(",")]
        except ValueError:
            a._sels = []

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.create_task(amain(a))
    loop.create_task(bot.state_loop())
    threading.Thread(target=stdin_thread, args=(loop, bot), daemon=True).start()
    print("bot ready - type 'help'")
    loop.run_forever()


if __name__ == "__main__":
    import os
    main()
