# avatar portrait test: PC + 3 bots (module 0/40/283), portraits on the
# F8 panel, race hexagons + leaderboard, results, popup_test
import sys,time,subprocess,threading,re,os,json,socket
from lead import *
from t_mp_util import *
unfocus=unfocus_game
def phase(code):
    try:
        r=admin({"cmd":"lobbies"}); d=json.loads(r)
        return d.get(code,{}).get("phase")
    except Exception:
        return None
def waitphase(code,want,timeout):
    t0=time.time()
    while time.time()-t0<timeout:
        p=phase(code)
        if p==want: return True
        time.sleep(1.5)
    return False

admin({"cmd":"lobby_leave"}); time.sleep(1); print("sel",ensure_select(),flush=True)
BOTPY=r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py"
code=[None]
lead_bot=subprocess.Popen([sys.executable,"-u",BOTPY,"--name","AvaLead","--port","17777","--create","--behavior","follower","--load-delay","2","--song-secs","300","--module","0"],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding="utf-8",errors="replace")
def pump():
    for l in lead_bot.stdout:
        m=re.match(r"LOBBY_CODE (\w+)",l)
        if m: code[0]=m.group(1)
threading.Thread(target=pump,daemon=True).start()
def B(c): lead_bot.stdin.write(c+"\n"); lead_bot.stdin.flush()
for i in range(50):
    if code[0]: break
    time.sleep(0.2)
extra=[subprocess.Popen([sys.executable,"-u",BOTPY,"--name",nm,"--port","17777","--join",code[0],"--behavior","follower","--load-delay","2","--song-secs","300","--module",md],
    stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,text=True) for nm,md in (("AvaRin","40"),("AvaTeto","283"))]
time.sleep(2.5)
admin({"cmd":"lobby_join","code":code[0]}); time.sleep(3)
print("room phase:",phase(code[0]),flush=True)
admin({"cmd":"autoplay","on":True,"pct":95}); time.sleep(1)
# F8 member panel with portraits
admin({"cmd":"overlay","open":True}); time.sleep(3)
shot("ava_panel")
admin({"cmd":"overlay","open":False}); time.sleep(1)
B("pick 28"); time.sleep(3); B("start")
print("loading:",waitphase(code[0],"PLAYING",90),flush=True)
time.sleep(8)
r=rect(); im=ImageGrab.grab(all_screens=True).crop(r[:4])
im.save(os.path.join(OUT,"ava_song0.png")); time.sleep(8)
im=ImageGrab.grab(all_screens=True).crop(r[:4])
im.save(os.path.join(OUT,"ava_song1.png"))
print("results:",waitphase(code[0],"RESULTS",300),flush=True)
time.sleep(3); shot("ava_res0"); time.sleep(5); shot("ava_res1")
print("select:",waitphase(code[0],"SELECT",120),flush=True)
time.sleep(3)
admin({"cmd":"popup_test"}); time.sleep(2.5)
shot("ava_pop0"); time.sleep(2); shot("ava_pop1")
admin({"cmd":"autoplay","on":False}); lead_bot.kill(); [b.kill() for b in extra]
time.sleep(1); admin({"cmd":"lobby_leave"})
print("done",flush=True)
