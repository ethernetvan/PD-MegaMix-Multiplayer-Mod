import sys,time,re,os
from lead import *
TAG="e"
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

