#!/usr/bin/env python3
"""mp_room.py - 3 or 4 Ryujinx clients in one room, AutoPlay driven by a script.

For manual testing: starts (or reuses) a detached server on the regular
gameplay ports 7777/7778/7790 (web UI 8080), boots ryu_multi instances
(Ryu1..RyuN, 2x2 grid on the second monitor) to song select and puts them
in one room (Ryu1 creates it, the rest join). It then controls each
console's partial AutoPlay hit rate (admin `autoplay --pct`). No song is
started: pick one yourself on the room's leader (Ryu1 first; the leader
rotates after every song).

Modes:
  rotate   (default) every --period s each console gets one of --levels,
           rotated (Ryu1 96 / Ryu2 98 / Ryu3 100, then 98/100/96, ...);
           --same gives all consoles the same rate.
  compete  a feedback controller that reads the live standings (achievement
           %, as the mod ranks them) once a second and steers the hit rates
           from who is winning. Every song play gets the next scenario
           (--scenarios) and the next featured player:
             seesaw    the lead rotates through everyone; each new leader
                       holds it 6-12 s, then the next one overtakes
             pack      rubber band: the further ahead, the slower; the
                       field stays within a few tenths and swaps often
             comeback  the featured player trails by 1.5-3 points for
                       --comeback-at s, then storms back through the
                       field and holds a small lead
             duel      the featured player and the next one trade the
                       lead; the rest follow 1-2 points behind
             runaway   the featured player pulls away; the others keep
                       overtaking each other for 2nd
             rotate    the static rotation of rotate mode
           Overtakes and a standings line every 5 s are printed. A console
           whose life drops low gets 100 % until it recovers (no fails).

    python tools/mp_room.py                          # 3 players, rotate
    python tools/mp_room.py --mode compete           # 3 players, scenarios
    python tools/mp_room.py -n 4 --mode compete      # 4 players
    python tools/mp_room.py cycle --mode compete     # controller only
    python tools/mp_room.py down                     # close instances + our server

Ctrl+C stops only the controller; emulators and server keep running.
Needs mm-re/tools/ryu_multi.py (MM_RE env var overrides ../mm-re) and a
current mod in %APPDATA%\\Ryujinx\\mods (./exlaunch.sh deploy-ryu): it is
copied into every instance root when the instances are (re)created.
"""
import argparse
import itertools
import os
import random
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MM_RE = os.environ.get("MM_RE", os.path.join(os.path.dirname(REPO), "mm-re"))
sys.path.insert(0, os.path.join(MM_RE, "tools"))
import ryu_multi as rm  # noqa: E402

PORT, DPORT, APORT, WPORT = 7777, 7778, 7790, 8080    # mp_server defaults
TMP = os.path.join(MM_RE, "tmp")
SERVER_PID = os.path.join(TMP, "mp_room_server.pid")
SCENARIOS = ("seesaw", "pack", "comeback", "duel", "runaway")
# CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP (DETACHED_PROCESS makes
# Ryujinx exit at boot without writing a log)
DETACH = 0x08000000 | 0x00000200


def spawn_detached(n):
    """ryu_multi.spawn, but survives Ctrl+C / closing this terminal."""
    return subprocess.Popen([rm.RYU, "-r", os.path.join(rm.BASE, f"ryu{n}"),
                             rm.NSP], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=DETACH)


rm.spawn = spawn_detached


def name(cid):
    return f"Ryu{int(cid, 16) & 0xff}"


# ------------------------------------------------------------ setup
def port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        return True
    except OSError:
        return False


def ensure_server():
    if port_open(APORT):
        rm.log(f"server already listening on admin port {APORT} - reusing it "
               f"(assuming {rm.LIVE_LOG} is its log)")
        return
    stamp = time.strftime("%m%d_%H%M%S")
    out = os.path.join(TMP, f"room_{stamp}_server.log")
    err = os.path.join(TMP, f"room_{stamp}_server.err")
    ps = ("$p = Start-Process -PassThru -WindowStyle Hidden "
          f"-FilePath '{sys.executable}' -WorkingDirectory '{REPO}' "
          "-ArgumentList '-B','-u','server/mp_server.py',"
          f"'--port','{PORT}','--discovery-port','{DPORT}',"
          f"'--admin-port','{APORT}','--web-port','{WPORT}' "
          f"-RedirectStandardOutput '{out}' -RedirectStandardError '{err}'; "
          f"Set-Content -NoNewline -Path '{SERVER_PID}' -Value $p.Id")
    # no stdout pipe: the server inherits PowerShell's handles and would
    # keep a pipe open (run() would then block until the server exits)
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    pid = open(SERVER_PID).read().strip()
    for _ in range(40):
        if port_open(APORT) and os.path.exists(out):
            break
        time.sleep(0.5)
    else:
        raise SystemExit(f"server did not come up, see {err}")
    if os.path.exists(rm.LIVE_LOG):
        os.replace(rm.LIVE_LOG, f"{rm.LIVE_LOG}.stale_{stamp}")
    os.link(out, rm.LIVE_LOG)
    rm.log(f"server pid {pid} up, log {out} (web UI http://127.0.0.1:{WPORT}"
           ", token printed in the log)")


def bring_up(n, stagger):
    insts = rm.load_state().get("instances", {})
    alive = [i for i in range(1, n + 1)
             if insts.get(str(i), {}).get("pid")
             and rm.pid_alive(insts[str(i)]["pid"])]
    if not alive:
        rm.setup(n, PORT, DPORT)
    elif len(alive) < n:
        raise SystemExit(f"only Ryu{alive} are running: run "
                         "`python tools/mp_room.py down` first so all "
                         f"{n} roots get set up together")
    rm.cmd_up(argparse.Namespace(n=n, admin_port=APORT, join=None,
                                 stagger=stagger))


def lobby_of(cid, lob):
    for code, l in lob.items():
        if cid in (m["cid"] for m in l.get("members", [])):
            return code
    return None


def ensure_room(cids):
    lob = rm.lobbies(APORT)
    code = lobby_of(cids[0], lob)
    if code is None:
        rm.admin({"cmd": "lobby_create", "cid": cids[0]}, APORT)
        for _ in range(20):
            time.sleep(1)
            lob = rm.lobbies(APORT)
            code = lobby_of(cids[0], lob)
            if code:
                break
        else:
            raise SystemExit("Ryu1 could not create a room")
        rm.log(f"Ryu1: created room {code}")
    for n, cid in enumerate(cids[1:], 2):
        other = lobby_of(cid, lob)
        if other == code:
            continue
        if other:
            rm.admin({"cmd": "lobby_leave", "cid": cid}, APORT)
            time.sleep(1.5)
        rm.join_lobby(n, code, APORT)
    names = [m["name"] for m in rm.lobbies(APORT)[code]["members"]]
    rm.log(f"room {code}: {names}")


# ------------------------------------------------------------ scenarios
def standings(rows, cids):
    return sorted(cids, key=lambda c: (-rows[c]["pct"], -rows[c]["score"]))


# Each console plays its own difficulty, so equal rates don't mean equal
# gains: every scenario is feedback on the live gaps, built from these rates.
PUSH, HOLD, DRAG = 100, 94, 87


def chase(rows, c, ref, lo, hi):
    """Rate that keeps c between lo and hi points behind ref."""
    gap = rows[ref]["pct"] - rows[c]["pct"]
    return PUSH if gap > hi else DRAG if gap < lo else HOLD


class Seesaw:
    """Make order[k] overtake the rest, let it hold the lead for a few
    seconds, then move on to order[k+1]. While someone leads, the others
    are kept between --margin and --spread behind it, so the next overtake
    is only a few seconds away."""

    def __init__(self, order, a):
        self.order, self.a = order, a
        self.k, self.led_since, self.hold_for = 0, None, 0
        self.tried_since = None

    def target(self):
        return self.order[self.k % len(self.order)]

    def next(self, say, why):
        self.k += 1
        self.led_since = self.tried_since = None
        say(f"{why}: {name(self.target())} goes for the lead")

    def step(self, t, rows, say):
        tgt = self.target()
        mine = rows[tgt]["pct"]
        rivals = [c for c in self.order if c != tgt]
        if not rivals:
            return {tgt: HOLD}
        lead = mine - max(rows[c]["pct"] for c in rivals)
        # hysteresis: once it leads by margin, it holds while still first
        if lead > self.a.margin or (self.led_since is not None and lead > 0):
            self.tried_since = None
            if self.led_since is None:
                self.led_since = t
                self.hold_for = random.uniform(*self.a.hold_secs)
            elif t - self.led_since >= self.hold_for:
                self.next(say, f"{name(tgt)} held it {self.hold_for:.0f} s")
            return {tgt: HOLD, **{c: chase(rows, c, tgt, self.a.margin,
                                           self.a.spread) for c in rivals}}
        self.led_since = None
        if self.tried_since is None:
            self.tried_since = t
        elif t - self.tried_since > self.a.give_up:
            self.next(say, f"{name(tgt)} can't get past")
        return {tgt: PUSH, **{c: DRAG if rows[c]["pct"] >= mine - self.a.margin
                              else HOLD for c in rivals}}


class Pack:
    """Rubber band: the further ahead of last place, the slower; the leader
    is always slowed, so nobody keeps first for long."""

    def __init__(self, cids, a):
        self.cids = cids

    def step(self, t, rows, say):
        low = min(rows[c]["pct"] for c in self.cids)
        lv = {c: max(DRAG, PUSH - round(12 * (rows[c]["pct"] - low)))
              for c in self.cids}
        top, second = standings(rows, self.cids)[:2]
        if rows[top]["pct"] - rows[second]["pct"] > 0.1:
            lv[top] = min(lv[top], DRAG + 2)
        return lv


class Comeback:
    def __init__(self, cids, a, feat):
        self.u, self.a = feat, a
        self.others = [c for c in cids if c != feat]
        self.sub = Seesaw(self.others, a)
        self.late = self.led = False

    def step(self, t, rows, say):
        best = max(self.others, key=lambda c: rows[c]["pct"])
        if t < self.a.comeback_at:          # lag 1.5-3 points behind
            lv = self.sub.step(t, rows, say)
            lv[self.u] = chase(rows, self.u, best, 1.5, 3.0)
            return lv
        if not self.late:
            self.late = True
            say(f"COMEBACK: {name(self.u)} hits the gas")
        mine = rows[self.u]["pct"]
        lead = mine - rows[best]["pct"]
        if lead > self.a.margin:
            self.led = True
        if self.led and lead > 0:           # hold a small lead to the end
            return {self.u: PUSH if lead < 2 * self.a.margin else HOLD,
                    **{c: chase(rows, c, self.u, self.a.margin, 1.2)
                       for c in self.others}}
        return {self.u: PUSH, **{c: DRAG if rows[c]["pct"] >= mine else HOLD
                                 for c in self.others}}


class Duel:
    def __init__(self, cids, a, feat):
        i = cids.index(feat)
        self.pair = [feat, cids[(i + 1) % len(cids)]]
        self.bench = [c for c in cids if c not in self.pair]
        self.sub = Seesaw(self.pair, a)

    def step(self, t, rows, say):
        lv = self.sub.step(t, rows, say)
        low = min(self.pair, key=lambda c: rows[c]["pct"])
        for c in self.bench:                # 0.8-2 points off the duel
            lv[c] = chase(rows, c, low, 0.8, 2.0)
        return lv


class Runaway:
    def __init__(self, cids, a, feat):
        self.r = feat
        self.sub = Seesaw([c for c in cids if c != feat], a)

    def step(self, t, rows, say):
        lv = self.sub.step(t, rows, say)
        for c in self.sub.order:            # never within 1.5 of the leader
            if rows[self.r]["pct"] - rows[c]["pct"] < 1.5:
                lv[c] = DRAG
        return {self.r: PUSH, **lv}


class Rotate:
    def __init__(self, cids, a, offset=0):
        self.cids, self.a, self.t0 = cids, a, offset

    def step(self, t, rows, say):
        tick = int(t // self.a.period) + self.t0
        lv = self.a.levels
        return {c: lv[(tick + (0 if self.a.same else i)) % len(lv)]
                for i, c in enumerate(self.cids)}


def make_scenario(kind, cids, a, feat, rnd):
    i = cids.index(feat)
    if kind == "seesaw":
        order = cids[i:] + cids[:i]
        return Seesaw(order, a), ("LEAD SWAP: the lead rotates " +
                                  " -> ".join(map(name, order)) + " -> ...")
    if kind == "pack":
        return Pack(cids, a), ("PACK: the further ahead, the slower - "
                               "a razor-thin field that keeps swapping")
    if kind == "comeback":
        return Comeback(cids, a, feat), (
            f"COMEBACK: {name(feat)} trails for {a.comeback_at:g} s, "
            "then storms back and holds on")
    if kind == "duel":
        s = Duel(cids, a, feat)
        return s, (f"DUEL: {' vs '.join(map(name, s.pair))} trade the lead, "
                   "the rest follow 1-2 points back")
    if kind == "runaway":
        return Runaway(cids, a, feat), (
            f"RUNAWAY: {name(feat)} pulls away, the rest fight for 2nd")
    return Rotate(cids, a, rnd), f"ROTATE: {a.levels} every {a.period:g} s"


# ------------------------------------------------------------ controller
class Controller:
    def __init__(self, cids, a):
        self.cids, self.a = cids, a
        self.sent, self.sent_at = {}, {}
        self.rescue = set()
        self.round = None
        self.plays = 0
        self.scen = None
        self.play_t0 = None
        self.leader = None
        self.last_board = 0
        self.kinds = itertools.cycle(a.scenarios)

    def send(self, lv):
        now = time.time()
        for cid, pct in lv.items():
            if self.sent.get(cid) == pct and now - self.sent_at[cid] < 15:
                continue
            try:
                rm.admin({"cmd": "autoplay", "on": True, "pct": pct,
                          "cid": cid}, APORT)
                self.sent[cid], self.sent_at[cid] = pct, now
            except OSError as e:
                rm.warn(f"{name(cid)}: {e}")

    def room(self):
        lob = rm.lobbies(APORT)
        code = lobby_of(self.cids[0], lob)
        return lob.get(code) if code else None

    def new_round(self, rid):
        self.round, self.play_t0, self.leader = rid, None, None
        self.rescue.clear()
        kind = next(self.kinds)
        feat = self.cids[self.plays % len(self.cids)]
        self.scen, desc = make_scenario(kind, self.cids, self.a, feat,
                                        self.plays)
        self.plays += 1
        rm.log(f"=== song {self.plays} (round {rid}): {desc}")

    def tick(self):
        l = self.room()
        if l is None:
            rm.warn("Ryu1 is not in a room")
            return
        mem = {m["cid"]: m for m in l["members"]}
        if any("pct" not in m for m in mem.values()):
            raise SystemExit("this server predates the live-state admin "
                             "fields: restart it with the current server/")
        if self.a.mode == "rotate":
            if self.scen is None:
                self.scen, desc = make_scenario("rotate", self.cids, self.a,
                                                self.cids[0], 0)
                self.t0 = time.time()
            lv = self.scen.step(time.time() - self.t0, {}, rm.log)
            if lv != self.sent:
                rm.log("  ".join(f"{name(c)}={p}%" for c, p in lv.items()))
            return self.send(lv)
        phase = l["phase"]
        if phase not in ("LOADING", "PLAYING"):
            self.play_t0 = None
            return self.send({c: 100 for c in self.cids})
        if l["round"] != self.round:
            self.new_round(l["round"])
        now = time.time()
        if phase == "PLAYING" and self.play_t0 is None and \
                any(mem[c]["pct"] > 0 for c in self.cids if c in mem):
            self.play_t0 = now               # first note, not the load
        t = now - self.play_t0 if self.play_t0 else 0.0
        live = [c for c in self.cids if c in mem and
                mem[c]["role"] == "player" and not mem[c]["final"]]
        if not live:
            return
        rows = {c: mem[c] if c in mem else {"pct": 0, "score": 0, "life": 0}
                for c in self.cids}
        lv = {c: 100 for c in self.cids}
        lv.update(self.scen.step(t, rows, lambda s: rm.log(f"  {s}")))
        for c in live:
            life = rows[c]["life"]
            if c not in self.rescue and 0 < life <= self.a.rescue_life:
                self.rescue.add(c)
                rm.log(f"  {name(c)} low life ({life}) - 100 % until it "
                       "recovers")
            elif c in self.rescue and life >= self.a.rescue_life + 40:
                self.rescue.discard(c)
        # same chart + same rate = identical hits = a tie forever: split
        # consoles that are level and on the same rate
        seen = {}
        for c in self.cids:
            key = (round(rows[c]["pct"], 2), lv[c])
            lv[c] -= seen.get(key, 0)
            seen[key] = seen.get(key, 0) + 1
        for c in self.rescue:
            lv[c] = 100
        self.send(lv)
        order = standings(rows, live)
        if self.play_t0 and t > 4:
            if self.leader and order[0] != self.leader:
                rm.log(f"  OVERTAKE {name(order[0])} passes "
                       f"{name(self.leader)} ({rows[order[0]]['pct']:.2f} vs "
                       f"{rows[self.leader]['pct']:.2f})")
            self.leader = order[0]
        if now - self.last_board >= 5 and self.play_t0:
            self.last_board = now
            rm.log(f"t+{t:3.0f}s " + "  ".join(
                f"{i}.{name(c)} {rows[c]['pct']:6.2f} ({lv[c]}%)"
                for i, c in enumerate(order, 1)))

    def run(self):
        if self.a.mode == "rotate":
            rm.log(f"rotating AutoPlay {self.a.levels} every "
                   f"{self.a.period:g}s - Ctrl+C to stop")
        else:
            rm.log(f"compete: scenarios {', '.join(self.a.scenarios)} "
                   "(one per song) - start a song on the room leader; "
                   "Ctrl+C to stop")
        while True:
            try:
                self.tick()
            except (OSError, ValueError) as e:
                rm.warn(f"server query failed: {e}")
            time.sleep(1.0)


def down():
    rm.cmd_down()
    if os.path.exists(SERVER_PID):
        pid = open(SERVER_PID).read().strip()
        subprocess.run(["taskkill", "/T", "/F", "/PID", pid],
                       capture_output=True)
        os.remove(SERVER_PID)
        rm.log(f"killed server pid {pid}")


def main():
    rm.dpi_aware()
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="up",
                    choices=["up", "cycle", "down"])
    ap.add_argument("-n", "--players", type=int, choices=[3, 4], default=3)
    ap.add_argument("--mode", choices=["rotate", "compete"], default="rotate")
    ap.add_argument("--period", type=float, default=10.0,
                    help="rotate: seconds between changes")
    ap.add_argument("--levels", type=int, nargs="+", default=[96, 98, 100],
                    help="rotate: hit rates")
    ap.add_argument("--same", action="store_true",
                    help="rotate: all consoles get the same rate each tick")
    ap.add_argument("--scenarios", type=lambda s: s.split(","),
                    default=list(SCENARIOS),
                    help="compete: comma list, one per song, repeating "
                    f"(default {','.join(SCENARIOS)}; also: rotate)")
    ap.add_argument("--hold-secs", type=float, nargs=2, default=[6, 12],
                    help="compete: how long a new leader keeps the lead")
    ap.add_argument("--margin", type=float, default=0.25,
                    help="compete: lead (achievement %% points) that counts "
                    "as an overtake")
    ap.add_argument("--spread", type=float, default=0.6,
                    help="compete: players further behind the leader than "
                    "this get boosted to stay in the fight")
    ap.add_argument("--give-up", type=float, default=25,
                    help="compete: seconds before a stalled overtake "
                    "passes to the next player")
    ap.add_argument("--comeback-at", type=float, default=40,
                    help="compete: seconds the comeback player lags")
    ap.add_argument("--rescue-life", type=int, default=60,
                    help="compete: life at which a console gets 100 %%")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--stagger", type=float, default=8)
    ap.add_argument("--min-free-gb", type=float, default=rm.MIN_FREE_GB,
                    help="free RAM needed before each launch")
    a = ap.parse_args()
    bad = set(a.scenarios) - set(SCENARIOS) - {"rotate"}
    if bad:
        ap.error(f"unknown scenario(s): {', '.join(sorted(bad))}")
    random.seed(a.seed)
    rm.MIN_FREE_GB = a.min_free_gb
    if a.cmd == "down":
        return down()
    cids = [rm.cid_of(i) for i in range(1, a.players + 1)]
    if a.cmd == "up":
        ensure_server()
        bring_up(a.players, a.stagger)
        ensure_room(cids)
    try:
        Controller(cids, a).run()
    except KeyboardInterrupt:
        rm.log("controller stopped (instances and server left running; "
               "`python tools/mp_room.py down` closes them)")


if __name__ == "__main__":
    main()
