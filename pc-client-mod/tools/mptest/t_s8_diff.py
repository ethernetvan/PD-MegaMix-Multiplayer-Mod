import sys, time, subprocess, threading, re
from lead import *
from t_mp_util import *

def sel_state():
    with open(SLOG, encoding="utf-8", errors="replace") as f:
        f.seek(max(0, size(SLOG) - 4000)); t = f.read()
    ls = [l for l in t.splitlines() if re.search(r"SEL '[^']+' f=\d+ state=", l)]
    if not ls: return (None, None)
    m = re.search(r"state=(\d+) pv=(\d+)", ls[-1])
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)

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
print("after join:", sel_state(), flush=True)
Bb("pick 28"); time.sleep(3)
print("after pick:", sel_state(), flush=True)
if sel_state()[0] != 8:
    print("!! not state 8 - abort"); bot.kill(); sys.exit(1)
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
shot("s8_diff")
print("RESULT diff:", bool(line) and not d, flush=True)
# don't wait the whole song; leave the room mid-song is fine for this test
admin({"cmd": "lobby_leave"}); bot.kill()
