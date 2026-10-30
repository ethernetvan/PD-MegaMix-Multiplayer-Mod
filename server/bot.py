#!/usr/bin/env python3
"""bot.py - scriptable MP client for testing the lobby protocol over TCP.

  python bot.py --name Lead --create --behavior leader --sels 249,1,2 --start-after 8
  python bot.py --name B1 --join ABCD --behavior follower
  python bot.py --name B2 --join ABCD --behavior missing
  python bot.py --name B3 --join ABCD --behavior follower --load-delay 40
  python bot.py --name B4 --join ABCD --behavior follower --fail-at 15
"""
import argparse, asyncio, json, random, sys, time


def ts():
    return time.strftime("%H:%M:%S")


class Bot:
    def __init__(self, args):
        self.name = args.name
        self.cid = ("bot-" + args.name).lower().ljust(16, "0")[:16]
        self.behavior = args.behavior
        self.sels = [int(x) for x in args.sels.split(",")] if args.sels else [249]
        self.sel_names = args.sel_names.split("|") if args.sel_names else []
        self.sel_delay = args.sel_delay
        self.sel_gap = args.sel_gap
        self.start_after = args.start_after
        self.load_delay = args.load_delay
        self.song_secs = args.song_secs
        self.fail_at = args.fail_at
        self.code = None
        self.is_leader = False
        self.role = "player"
        self.round = 0
        self.final_sent = False
        self.leader_cid = None
        self.done = asyncio.Event()

    async def connect(self, host, port):
        self.r, self.w = await asyncio.open_connection(host, port)
        await self.send({"t": "hello", "proto": 1, "mod": "bot/1",
                         "game": "1.0.6", "cid": self.cid, "name": self.name})

    async def send(self, m):
        self.w.write((json.dumps(m) + "\n").encode())
        await self.w.drain()

    def log(self, s):
        print(f"[{ts()}] {self.name}: {s}", flush=True)

    async def on_msg(self, m):
        t = m.get("t")
        if t == "lobby":
            self.code = m["code"]
            self.round = m["round"]
            self.leader_cid = m["leader"]
            self.is_leader = (m["leader"] == self.cid)
            for mm in m["members"]:
                if mm["cid"] == self.cid:
                    self.role = mm["role"]
        elif t == "sel":
            if self.behavior == "missing":
                await self.send({"t": "sel_ack", "pv": m["pv"], "has": False,
                                 "ok": False, "fb_pv": -1, "fb_name": None})
                self.log(f"sel {m['pv']} -> ack has:false (missing)")
            elif not self.is_leader:
                await self.send({"t": "sel_ack", "pv": m["pv"], "has": True,
                                 "ok": True, "fb_pv": -1, "fb_name": None})
                self.log(f"sel {m['pv']} -> ack ok")
        elif t == "sel_status":
            self.log(f"sel_status missing={[x['name'] for x in m['missing']]} "
                     f"cant={[x['name'] for x in m['cant']]}")
        elif t == "launch":
            self.round = m["round"]
            if self.cid in m["players"]:
                d = self.load_delay or random.uniform(2, 4)
                self.log(f"launch r{m['round']} pv={m['pv']} -> loaded in {d:.1f}s")
                asyncio.create_task(self._load(m["round"], d))
            else:
                self.log(f"launch r{m['round']} -> spectator")
        elif t == "start":
            self.round = m["round"]
            if self.cid in (m.get("dropped") or []):
                self.log(f"start r{self.round} -> DROPPED")
            else:
                self.log(f"start r{self.round} -> playing")
                asyncio.create_task(self._play(self.round))
        elif t == "board":
            pass
        elif t == "standings":
            self.log(f"standings complete={m['complete']} "
                     + " ".join(f"{x['rank']}.{x['name']}={x['score']}"
                                for x in m["e"]))
        elif t == "round_end":
            self.log(f"round_end -> next_leader={m['next_leader']}")
            self.done.set()
        elif t == "event":
            self.log(f"event {m['kind']} {m.get('name')} {m.get('value')}")
        elif t == "lobby_error":
            self.log(f"lobby_error {m['code']}: {m['msg']}")
        elif t == "lobby_left":
            self.log(f"lobby_left {m['reason']}")
            self.done.set()

    async def _load(self, rd, delay):
        await asyncio.sleep(delay)
        await self.send({"t": "loaded", "round": rd})
        self.log(f"sent loaded r{rd}")

    async def _play(self, rd):
        score = 0
        end = self.fail_at or self.song_secs
        t0 = time.time()
        while time.time() - t0 < end:
            await asyncio.sleep(0.2)
            score += 500
            life = 100
            await self.send({"t": "state", "round": rd, "score": score,
                             "combo": int((time.time() - t0) * 5),
                             "pct": (time.time() - t0) * 2.5, "life": life})
        if self.fail_at:
            await self.send({"t": "final", "round": rd, "score": score,
                             "pct": 50.0, "max_combo": 10,
                             "jc": [5, 3, 0, 0, 20], "life": 0})
            self.log(f"sent EARLY final r{rd} (fail_at={self.fail_at})")
        else:
            await asyncio.sleep(max(0, self.song_secs - (time.time() - t0)))
            await self.send({"t": "final", "round": rd, "score": score,
                             "pct": 99.0, "max_combo": int(self.song_secs * 5),
                             "jc": [30, 10, 0, 0, 0], "life": 100})
            self.log(f"sent final r{rd}")
        self.final_sent = True
        await asyncio.sleep(4)
        await self.send({"t": "back_in_sel", "round": rd})
        self.log(f"sent back_in_sel r{rd}")

    async def leader_flow(self):
        await asyncio.sleep(1.0)
        await self.send({"t": "lobby_create"})
        await asyncio.sleep(0.5)
        if self.code:
            print(f"LOBBY_CODE {self.code}", flush=True)
        await asyncio.sleep(self.sel_delay)
        for i, pv in enumerate(self.sels):
            name = self.sel_names[i] if i < len(self.sel_names) else f"PV{pv}"
            await self.send({"t": "sel", "pv": pv, "name": name})
            self.log(f"sent sel pv={pv} '{name}'")
            await asyncio.sleep(self.sel_gap)
        await asyncio.sleep(self.start_after)
        pv = self.sels[-1]
        name = self.sel_names[-1] if len(self.sel_names) >= len(self.sels) else f"PV{pv}"
        await self.send({"t": "start_req", "pv": pv, "name": name})
        self.log(f"sent start_req pv={pv}")

    async def run(self, host, port, create, join):
        await self.connect(host, port)
        if join:
            await asyncio.sleep(0.5)
            await self.send({"t": "lobby_join", "code": join})
            self.log(f"sent lobby_join {join}")
        if self.behavior == "leader" and create:
            asyncio.create_task(self.leader_flow())
        try:
            while True:
                line = await self.r.readline()
                if not line:
                    break
                try:
                    m = json.loads(line)
                except Exception:
                    continue
                if m.get("t") == "ping":
                    continue
                await self.on_msg(m)
        except (ConnectionResetError, BrokenPipeError):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="Bot")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--create", action="store_true")
    ap.add_argument("--join", default=None)
    ap.add_argument("--behavior", choices=["leader", "follower", "missing"],
                    default="follower")
    ap.add_argument("--sels", default="249")
    ap.add_argument("--sel-names", default="", help="'|'-separated names matching --sels")
    ap.add_argument("--sel-delay", type=float, default=0, help="seconds before the first sel")
    ap.add_argument("--sel-gap", type=float, default=1.0, help="seconds between sels")
    ap.add_argument("--start-after", type=float, default=8.0)
    ap.add_argument("--load-delay", type=float, default=0)
    ap.add_argument("--song-secs", type=float, default=40)
    ap.add_argument("--fail-at", type=float, default=0)
    args = ap.parse_args()
    asyncio.run(Bot(args).run(args.host, args.port, args.create, args.join))


if __name__ == "__main__":
    main()
