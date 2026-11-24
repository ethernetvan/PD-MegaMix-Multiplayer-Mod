# long-running bar capture: PC autoplay 95 vs bots; captures ~60-110 s in
# where the PC passes SlowBot/MidBot so trailing bots draw no segment.
import sys, time, subprocess, threading, re, os
from lead import *
from t_mp_util import *
admin({"cmd": "lobby_leave"}); time.sleep(1); print("sel", ensure_select(), flush=True)
code = [None]
bot = subprocess.Popen([sys.executable, "-u",
    r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py",
    "--name", "BarBot", "--port", "17777", "--create", "--behavior",
    "follower", "--load-delay", "2", "--song-secs", "300"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    text=True, encoding="utf-8", errors="replace")
def pump():
    for l in bot.stdout:
        m = re.match(r"LOBBY_CODE (\w+)", l)
        if m: code[0] = m.group(1)
threading.Thread(target=pump, daemon=True).start()
def B(c): bot.stdin.write(c + "\n"); bot.stdin.flush()
for i in range(50):
    if code[0]: break
    time.sleep(0.2)
BOTPY = r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py"
extra = [subprocess.Popen([sys.executable, "-u", BOTPY, "--name", nm,
    "--port", "17777", "--join", code[0], "--behavior", "follower",
    "--load-delay", "2", "--song-secs", "300", "--speed", sp],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL, text=True)
    for nm, sp in (("SlowBot", "0.45"), ("MidBot", "0.75"),
                   ("FastBot", "1.3"), ("MaxBot", "12"))]
time.sleep(2)
off = size(SLOG)
admin({"cmd": "lobby_join", "code": code[0]}); time.sleep(3)
admin({"cmd": "autoplay", "on": True, "pct": 95}); time.sleep(1)
B("pick 28"); time.sleep(3); B("start")
wait(r"lobby \w+: start r\d+", off, 40)
print("started, waiting for PC to pass the slow bots...", flush=True)
from PIL import ImageGrab
for nm, secs in (("late60", 45), ("late75", 15), ("late90", 15), ("late105", 15)):
    time.sleep(secs)
    r = rect()
    im = ImageGrab.grab(all_screens=True).crop(r[:4])
    im.save(os.path.join(OUT, f"bar_{nm}.png"))
    W, H = im.size
    im.crop((0, int(H * 0.85), W, int(H * 0.97))).save(
        os.path.join(OUT, f"bar_{nm}_strip.png"))
    print("saved", nm, flush=True)
admin({"cmd": "autoplay", "on": False}); bot.kill()
[b.kill() for b in extra]; time.sleep(1)
admin({"cmd": "lobby_leave"})
print("done", flush=True)
