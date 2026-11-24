import sys,time,subprocess,threading,re
from lead import *
from t_mp_util import *
admin({"cmd":"lobby_leave"}); time.sleep(1); print("sel",ensure_select())
code=[None]
bot=subprocess.Popen([sys.executable,"-u",r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py","--name","VisBot","--port","17777","--create","--behavior","follower","--load-delay","2","--song-secs","25"],
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
admin({"cmd":"overlay","open":True}); time.sleep(1.5); shot("v_panel"); admin({"cmd":"overlay","open":False}); time.sleep(1)
B("pick 28"); time.sleep(3); B("start")
wait(rf"lobby {code[0]}: start r\d+",off,40); off2=size(SLOG); time.sleep(8); shot("v_song")
l=wait(r"RESULT .* ev=init",size(SLOG) if False else off2,200); log("pc results:",bool(l)); time.sleep(4); shot("v_results"); time.sleep(4); shot("v_results2")
bot.kill()
