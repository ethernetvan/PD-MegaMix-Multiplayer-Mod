# MP sync test vs a bot on the test server (17777). Usage: t_mp.py TAG [rounds]
import sys, time, subprocess, threading, re
from lead import *
TAG=sys.argv[1]; ROUNDS=int(sys.argv[2]) if len(sys.argv)>2 else 2
BOTLOG=open(os.path.join(OUT,f"{TAG}_bot.log"),"w",encoding="utf-8")
names=[]
def S(name):
    w=shot(f"{TAG}_{name}"); names.append(f"{TAG}_{name}"); return w
def st(): 
    d=dbg_last(); r=rect()
    return f"paused={d.get('paused')} st={d.get('st')} life={d.get('life')} score={d.get('score')} fg={r[5]} hung={r[6]}"
def slog(off,pat):
    return [l for l in since(SLOG,off).splitlines() if re.search(pat,l)]
def mlog(off,pat):
    return [l for l in since(MLOG,off).splitlines() if re.search(pat,l)]
def wait(pat,off,timeout,path=SLOG):
    t0=time.time()
    while time.time()-t0<timeout:
        ls=[l for l in since(path,off).splitlines() if re.search(pat,l)]
        if ls: return ls[-1]
        time.sleep(0.2)
    return None
unfocus=unfocus_game
def moving(dt=1.0):
    """True when the game picture changes over dt seconds (song running)"""
    from PIL import ImageChops, ImageGrab
    r=rect(); a=ImageGrab.grab(all_screens=True).crop(r[:4]).resize((160,90)).convert("L")
    time.sleep(dt); b=ImageGrab.grab(all_screens=True).crop(r[:4]).resize((160,90)).convert("L")
    d=ImageChops.difference(a,b); return sum(1 for p in d.getdata() if p>24)/(160*90) > 0.02
def mtime(line):  # "HH:MM:SS.mmm ..." -> seconds float
    h,m,x=line[:12].split(":"); return int(h)*3600+int(m)*60+float(x)
def tsec(line):   # "[HH:MM:SS] ..." -> seconds
    h,m,s=map(int,line[1:9].split(":")); return h*3600+m*60+s

admin({"cmd":"lobby_leave"}); time.sleep(1.5)   # a reconnect keeps the old room
code=[None]
bot=subprocess.Popen([sys.executable,"-u",r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\server\bot.py",
    "--name","SyncBot","--port","17777","--create","--behavior","follower","--load-delay","3","--song-secs","45","--vote","instant"],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding="utf-8",errors="replace")
def pump():
    for l in bot.stdout:
        BOTLOG.write(time.strftime("%H:%M:%S ")+l); BOTLOG.flush()
        m=re.match(r"LOBBY_CODE (\w+)",l)
        if m: code[0]=m.group(1)
threading.Thread(target=pump,daemon=True).start()
def B(cmd): bot.stdin.write(cmd+"\n"); bot.stdin.flush(); log("bot>",cmd)
try:
    for i in range(50):
        if code[0]: break
        time.sleep(0.2)
    log("unfocused game:",not rect()[5])
    log("room",code[0]); off0=size(SLOG); moff0=size(MLOG)
    admin({"cmd":"lobby_join","code":code[0]})
    log("pc joined:",bool(wait(r"pc00000000000001 joined",off0,8)))
    time.sleep(2); S("room")
    for rnd in range(1,ROUNDS+1):
        off=size(SLOG); moff=size(MLOG)
        bot_leads = (rnd % 2 == 1)
        log(f"=== round {rnd}: {'bot' if bot_leads else 'PC'} leads ===")
        if bot_leads:
            if rnd==1: B("pick 28"); time.sleep(4); S(f"r{rnd}_sel"); B("start")
            # later bot-led rounds: bot auto-picks on round_end
        else:
            time.sleep(3); tap("A"); time.sleep(1.5); S(f"r{rnd}_confirm"); tap("A")
        l=wait(rf"lobby {code[0]}: launch r\d+",off,40); log("launch:",l and l[11:90])
        if not l: raise SystemExit("no launch")
        l2=wait(r"loader hold engaged",moff,40,MLOG); t_hold=time.time(); log("pc loader hold:",bool(l2))
        time.sleep(1.0); w=S(f"r{rnd}_held"); log("held white",w,st(),"picture moving (expect False):",moving(0.7))
        ls=wait(rf"lobby {code[0]}: start r\d+",off,60); log("server start:",ls and ls[:60])
        t_start=time.time()
        lr=wait(r"loader hold released",moff,5,MLOG); log("pc released:",lr)
        bl=[l for l in open(os.path.join(OUT,f"{TAG}_bot.log"),encoding="utf-8").read().splitlines() if "-- start r" in l]
        log("bot got start at",bl[-1][:8] if bl else None,"| pc released at",lr and lr[:12])
        time.sleep(0.6); S(f"r{rnd}_go")
        time.sleep(2.5); w=S(f"r{rnd}_play")
        log("playing white",w,st(),"picture moving:",moving())
        if rnd<=2:
            log("game focused:",rect()[5])
            # owner (bot) pause -> resume
            o=size(SLOG); B("pause"); time.sleep(2.5); w=S(f"r{rnd}_pause_owner"); log("owner pause ->",st(),"moving:",moving(), slog(o,r"pause by")[-1:][0][11:70] if slog(o,r"pause by") else None)
            o=size(SLOG); B("resume"); time.sleep(2.5); w=S(f"r{rnd}_resume_owner"); log("owner resume ->",st())
            log("running after resume: moving:",moving())
            # member (PC) vote pause -> resume
            o=size(SLOG); admin({"cmd":"pause_req"}); time.sleep(3.0); S(f"r{rnd}_pause_vote"); log("pc pause_req ->",st(),"moving:",moving(), [x[11:80] for x in slog(o,r"vote|pause by|resume by")][-2:])
            o=size(SLOG); admin({"cmd":"resume_req"}); time.sleep(3.0); S(f"r{rnd}_resume_vote"); log("pc resume_req ->",st(),"moving:",moving(), [x[11:80] for x in slog(o,r"vote|pause by|resume by")][-2:])
        # wait for the round to finish: PC results + standings complete
        lf=wait(rf"lobby {code[0]}: r\d+ standings complete",off,240); log("standings complete:",bool(lf))
        S(f"r{rnd}_results")
        le=wait(rf"lobby {code[0]}: r\d+ end -> leader",off,150); log("round end:",le and le[11:90])
        lb=wait(r"SEL 'PcRenamed' INIT",off,60); log("pc back at song select:",bool(lb))
        time.sleep(3); w=S(f"r{rnd}_backsel"); log("back white",w,st())
    log("mp_log notable:",[l for l in mlog(moff0,r"NOT reached|stuck|FAILED|WARNING|timeout")])
finally:
    sheet(names,TAG+"_sheet",cols=4)
    try: B("leave")
    except Exception: pass
    time.sleep(1); admin({"cmd":"lobby_leave"}); bot.kill()
