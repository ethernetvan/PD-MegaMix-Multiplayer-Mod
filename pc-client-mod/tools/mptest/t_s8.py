# state-8 follower launch test (deterministic):
#   PC opens the song-start confirm panel SOLO (state 8), THEN joins a
#   bot-led room. When the leader starts, the follower launch autopilot
#   must close the panel with B, return to state 6, jump to the pick,
#   and launch. Variant 2: panel open on a DIFFERENT pv than the pick.
import sys, time, subprocess, threading, re, os
from lead import *
from t_mp_util import *

def sel_state():
    with open(SLOG, encoding="utf-8", errors="replace") as f:
        f.seek(max(0, size(SLOG) - 8000)); t = f.read()
    ls = [l for l in t.splitlines() if re.search(r"SEL '[^']+' f=\d+ state=", l)]
    if not ls: return (None, None)
    m = re.search(r"state=(\d+) pv=(\d+)", ls[-1])
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)

def wait_state6(timeout=240):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s, p = sel_state()
        if s == 6: return True
        time.sleep(3)
    return False

def scenario(tag, panel_pv):
    print(f"=== scenario {tag} (panel on pv {panel_pv}) ===", flush=True)
    admin({"cmd": "lobby_leave"}); time.sleep(1)
    # solo: jump to the pv and open the confirm panel -> state 8
    admin({"cmd": "jump", "pv": panel_pv}); time.sleep(2)
    tap("A"); time.sleep(3)
    s, pv = sel_state()
    print("panel solo: state", s, "pv", pv, flush=True)
    if s != 8:
        tap("A"); time.sleep(3)
        s, pv = sel_state()
        print("retry panel: state", s, "pv", pv, flush=True)
        if s != 8:
            print("!! cannot reach state 8, abort", flush=True); return False
    # join a bot-led room with the panel still open
    bot = subprocess.Popen([sys.executable, "-u",
        r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py",
        "--name", "LeadBot", "--port", "17777", "--create",
        "--behavior", "follower", "--load-delay", "2", "--song-secs", "300"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    code = [None]
    def pump():
        for l in bot.stdout:
            m = re.match(r"LOBBY_CODE (\w+)", l)
            if m: code[0] = m.group(1)
            print("BOT>", l.rstrip(), flush=True)
    threading.Thread(target=pump, daemon=True).start()
    def Bb(c): bot.stdin.write(c + "\n"); bot.stdin.flush()
    for i in range(60):
        if code[0]: break
        time.sleep(0.2)
    off = size(SLOG)
    admin({"cmd": "lobby_join", "code": code[0]}); time.sleep(4)
    s, pv = sel_state()
    print("after join: state", s, "pv", pv, flush=True)
    Bb("pick 28"); time.sleep(3)
    s, pv = sel_state()
    print("after pick: state", s, "pv", pv, flush=True)
    if s != 8:
        print("!! panel closed on join/pick - cannot test state-8 path",
              flush=True)
        bot.kill(); return None
    off2 = size(SLOG)
    Bb("start"); time.sleep(2)
    t0 = time.time(); seen = []
    while time.time() - t0 < 60:
        s, p = sel_state()
        if s is not None and (not seen or seen[-1][0] != s):
            seen.append((s, p, round(time.time() - t0, 1)))
        if s == 10: break
        time.sleep(0.3)
    print("states:", seen, flush=True)
    line = wait(r"loaded=1 .*pv=28\b", off2, 40)
    print("loaded pv28:", line, flush=True)
    time.sleep(8)
    d = [l for l in since(SLOG, off).splitlines() if "DROPPED" in l]
    print("DROPPED:", d, flush=True)
    sc = [l for l in since(SLOG, off2).splitlines()
          if "loaded=1" in l and "score=" in l]
    print("last dbg:", sc[-1] if sc else None, flush=True)
    shot(f"s8_{tag}")
    ok = bool(line) and not d
    # wait for the song to end and the PC back in select, then leave
    print("waiting for round end...", flush=True)
    wait_state6(280)
    admin({"cmd": "lobby_leave"}); bot.kill(); time.sleep(2)
    return ok

print("ensure song select:", sel_state(), flush=True)
r1 = scenario("same", 28)     # panel open on the SAME pv the leader picks
r2 = scenario("diff", 43)     # panel open on a DIFFERENT pv
print("RESULT same:", r1, "diff:", r2, flush=True)
