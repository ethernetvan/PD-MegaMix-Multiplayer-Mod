import sys,time,subprocess,threading,re
from lead import *
from t_mp_util import *
admin({"cmd":"lobby_leave"}); time.sleep(1); print("sel",ensure_select(),flush=True)
code=[None]
bot=subprocess.Popen([sys.executable,"-u",r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py","--name","BarBot","--port","17777","--create","--behavior","follower","--load-delay","2","--song-secs","40"],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding="utf-8",errors="replace")
def pump():
    for l in bot.stdout:
        m=re.match(r"LOBBY_CODE (\w+)",l)
        if m: code[0]=m.group(1)
threading.Thread(target=pump,daemon=True).start()
def B(c): bot.stdin.write(c+"\n"); bot.stdin.flush()
for i in range(50):
    if code[0]: break
    time.sleep(0.2)
off=size(SLOG); admin({"cmd":"lobby_join","code":code[0]}); time.sleep(3)
admin({"cmd":"autoplay","on":True,"pct":95}); time.sleep(1)
B("pick 28"); time.sleep(3); B("start")
wait(rf"lobby {code[0]}: start r\d+",off,40); time.sleep(3)
from PIL import ImageGrab, Image
frames=[]
for i in range(12):
    r=rect(); im=ImageGrab.grab(all_screens=True).crop(r[:4]); W,H=im.size
    frames.append(im.crop((0,int(H*0.85),W,int(H*0.97))))
    if i in (3,8): im.save(os.path.join(OUT,f"bar_full{i}.png"))
    time.sleep(2.5)
w,h=frames[0].size
S_=Image.new("RGB",(w,h*len(frames)))
for i,f in enumerate(frames): S_.paste(f,(0,i*h))
S_.save(os.path.join(OUT,"bar_burst.png"))
admin({"cmd":"autoplay","on":False}); bot.kill(); time.sleep(1); admin({"cmd":"lobby_leave"})
print("done",flush=True)
