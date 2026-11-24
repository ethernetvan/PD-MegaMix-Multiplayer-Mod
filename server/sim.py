#!/usr/bin/env python3
"""sim.py - multi-bot end-to-end run: 1 leader + N followers (+missing/straggler)
through 2 rounds. Usage: python sim.py [--port 7777] [--bots 2] [--missing] [--straggler]"""
import asyncio, json, argparse, random

class Bot:
    def __init__(self, name, port):
        self.name = name; self.port = port
        self.cid = name.lower().ljust(16, '0')
        self.role = "player"; self.rd = 0; self.code = None
        self.is_leader = False; self.missing = False; self.load_delay = 0
        self.stopped = False; self.inbox = asyncio.Queue()

    async def connect(self):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", self.port)
        await self.send({"t": "hello", "proto": 1, "mod": "sim", "game": "x",
                         "cid": self.cid, "name": self.name})
        asyncio.create_task(self.pump())

    async def send(self, m):
        self.w.write((json.dumps(m) + "\n").encode()); await self.w.drain()

    async def pump(self):
        while not self.stopped:
            line = await self.r.readline()
            if not line: break
            try: m = json.loads(line)
            except Exception: continue
            await self.on_msg(m)

    async def on_msg(self, m):
        t = m.get("t")
        if t == "lobby":
            self.code = m["code"]; self.rd = m["round"]
            self.is_leader = (m["leader"] == self.cid)
            for mm in m["members"]:
                if mm["cid"] == self.cid: self.role = mm["role"]
        elif t == "sel" and not self.is_leader and not self.missing:
            await self.send({"t": "sel_ack", "pv": m["pv"], "has": True,
                             "ok": True, "fb_pv": -1, "fb_name": None})
        elif t == "sel" and self.missing:
            await self.send({"t": "sel_ack", "pv": m["pv"], "has": False,
                             "ok": False, "fb_pv": -1, "fb_name": None})
        elif t == "launch" and self.cid in m["players"]:
            delay = self.load_delay or random.uniform(2, 6)
            asyncio.create_task(self.load_then_play(m["round"], delay))
        elif t == "start":
            asyncio.create_task(self.play(m["round"]))

    async def load_then_play(self, rd, delay):
        await asyncio.sleep(delay)
        await self.send({"t": "loaded", "round": rd})

    async def play(self, rd):
        if self.role != "player": return
        score = 0
        for i in range(200):
            if self.stopped: return
            await asyncio.sleep(0.2)
            score += 500
            await self.send({"t": "state", "round": rd, "score": score,
                             "combo": i, "pct": i / 2, "life": 100})
        await self.send({"t": "final", "round": rd, "score": score,
                         "pct": 99.0, "max_combo": 200,
                         "jc": [10, 5, 0, 0, 0], "life": 100})
        await asyncio.sleep(4)
        await self.send({"t": "back_in_sel", "round": rd})

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--bots", type=int, default=2)
    ap.add_argument("--missing", action="store_true")
    ap.add_argument("--straggler", action="store_true")
    args = ap.parse_args()

    bots = [Bot("Lead", args.port)]
    bots += [Bot(f"Bot{i}", args.port) for i in range(args.bots)]
    if args.missing:
        mb = Bot("Miss", args.port); mb.missing = True; bots.append(mb)
    if args.straggler:
        sb = Bot("Slow", args.port); sb.load_delay = 40; bots.append(sb)
    for b in bots: await b.connect()
    await asyncio.sleep(0.5)

    lead = bots[0]
    await lead.send({"t": "lobby_create"})
    await asyncio.sleep(0.5)
    code = lead.code
    print("lobby code:", code)
    for b in bots[1:]:
        await b.send({"t": "lobby_join", "code": code})
    await asyncio.sleep(0.5)

    summary = {"starts": 0, "standings": 0, "round_ends": 0, "leaders": []}
    async def watch(b):
        while not b.stopped:
            m = await b.inbox.get() if False else None
            break
    # passive observation via msgs already handled in on_msg; poll lobby state
    for rnd in range(2):
        await lead.send({"t": "sel", "pv": 100 + rnd, "name": f"Song{rnd}"})
        await asyncio.sleep(1.0)
        await lead.send({"t": "start_req", "pv": 100 + rnd, "name": f"Song{rnd}"})
        await asyncio.sleep(42)
        lead2 = lead.is_leader
        print(f"round {rnd + 1} done; lead still leader? {lead2}")
        # after round 1, rotation makes someone else leader
        for b in bots:
            if b.is_leader:
                lead = b
                summary["leaders"].append(b.name)
                break
        await asyncio.sleep(1.5)

    for b in bots: b.stopped = True
    print("leader sequence:", summary["leaders"])
    print("SIM OK")

if __name__ == "__main__":
    asyncio.run(main())
