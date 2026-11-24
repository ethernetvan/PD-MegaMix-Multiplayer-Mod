# edge cases: leave while group-paused; leave while held at the start gate
import sys, time, subprocess, threading, re
from lead import *
from t_mp_util import *
def run(load_delay, tag):
    code=[None]
    bot=subprocess.Popen([sys.executable,"-u",r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py",
        "--name","EdgeBot","--port","17777","--create","--behavior","follower","--load-delay",str(load_delay),"--song-secs","40"],
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
    off=size(SLOG); moff=size(MLOG)
    admin({"cmd":"lobby_join","code":code[0]}); wait(r"pc00000000000001 joined",off,8); time.sleep(2)
    B("pick 28"); time.sleep(3); B("start")
    wait(r"launch r\d+",off,30); wait(r"loader hold engaged",moff,40,MLOG)
    return bot,B,off,moff
log("game unfocused:",unfocus_game())
# 1) leave while held at the gate (bot never reports loaded in time)
bot,B,off,moff=run(60,"held")
time.sleep(2); log("held: moving",moving(0.7))
admin({"cmd":"lobby_leave"}); time.sleep(3)
log("left while held -> released:",wait(r"loader hold released",moff,3,MLOG), st())
shot("e_left_held"); bot.kill()
# back to song select through the game's own pause menu
log("solo return to list:",back_to_select())
# 2) leave while group-paused
bot,B,off,moff=run(2,"paused")
wait(r"LOBBY .*start r\d+",off,40); time.sleep(5)
B("pause"); time.sleep(3); log("group paused:",st(),"moving",moving())
admin({"cmd":"lobby_leave"}); time.sleep(4)
log("left while paused ->",st(),"picture moving:",moving(), [l[13:] for l in mlog(moff,r"sync-pause|resume issued")][-2:])
shot("e_left_paused"); bot.kill()
log("solo return to list:",back_to_select(),"white",shot("e_end"))
