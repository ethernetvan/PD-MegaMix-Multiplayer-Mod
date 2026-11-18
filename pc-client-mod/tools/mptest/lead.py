# lead tooling: admin taps, pv memory reads, DISPLAY2 screenshots
import socket, json, time, os, sys, ctypes, subprocess, struct
from PIL import ImageGrab
OUT=r"C:\Users\cl672\Documents\Coding\mm-re\tmp\ab"
SLOG=r"C:\Users\cl672\Documents\Coding\mm-re\tmp\pc_server2.log"
MLOG=r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\pc-client-mod\dist\MegaMixMP\mp_log.txt"
CID="pc00000000000001"
k=ctypes.windll.kernel32; u=ctypes.windll.user32
k.OpenProcess.restype=ctypes.c_void_p
k.ReadProcessMemory.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
def pid():
    o=subprocess.run(["tasklist","/FI","IMAGENAME eq DivaMegaMix.exe","/FO","CSV","/NH"],capture_output=True,text=True).stdout
    return int(o.split(",")[1].strip('"')) if "Diva" in o else 0
_H=[None,0]
def H():
    p=pid()
    if p!=_H[1]: _H[0]=k.OpenProcess(0x410,False,p) if p else None; _H[1]=p
    return _H[0]
def rd(a,n):
    b=ctypes.create_string_buffer(n); r=ctypes.c_size_t(0)
    k.ReadProcessMemory(H(),ctypes.c_void_p(a),b,n,ctypes.byref(r)); return b.raw[:r.value]
PV=0x140000000+0x12C2330
def pv():
    b=rd(PV,2)
    return (b[0],b[1]) if len(b)==2 else (-1,-1)
def admin(p,cid=CID):
    if cid: p["cid"]=cid
    with socket.create_connection(("127.0.0.1",17790),timeout=5) as s:
        s.sendall((json.dumps(p)+"\n").encode()); return s.recv(65536).decode()
def tap(b,n=1,gap=300): admin({"cmd":"tap","btn":b,"count":n,"hold":50,"gap":gap})
def rect():
    from ctypes import wintypes as W
    res=[]; mypid=pid()
    @ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def cb(h,l):
        p=W.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(p))
        b=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,b,256)
        if p.value==mypid and b.value.endswith("Mega Mix+") and u.IsWindowVisible(h):
            r=W.RECT(); u.GetClientRect(h,ctypes.byref(r)); pt=W.POINT(0,0); u.ClientToScreen(h,ctypes.byref(pt))
            res.append((pt.x,pt.y,pt.x+r.right,pt.y+r.bottom,h,u.GetForegroundWindow()==h,u.IsHungAppWindow(h)))
        return True
    u.EnumWindows(cb,0); return res[0] if res else None
def shot(name):
    r=rect()
    im=ImageGrab.grab(all_screens=True).crop(r[:4]) if r else ImageGrab.grab(all_screens=True)
    im.save(os.path.join(OUT,name+".png"))
    px=list(im.resize((96,54)).convert("RGB").getdata())
    w=sum(1 for a,b,c in px if a>235 and b>235 and c>235)/len(px)
    return round(w,2)
def size(p): return os.path.getsize(p)
def since(p,off):
    with open(p,encoding="utf-8",errors="replace") as f: f.seek(off); return f.read()
def log(*a): print(time.strftime("%H:%M:%S"),*a,flush=True)
if __name__=="__main__":
    c=sys.argv[1]
    if c=="shot": print(sys.argv[2], "white",shot(sys.argv[2]), "pv",pv(), "rect",rect())
    elif c=="tap": tap(sys.argv[2], int(sys.argv[3]) if len(sys.argv)>3 else 1); time.sleep(float(sys.argv[4]) if len(sys.argv)>4 else 1.0); print("pv",pv())
    elif c=="pv": print(pv())
    elif c=="admin": print(admin(json.loads(sys.argv[2])))

def sheet(names, out, cols=4, w=480):
    from PIL import Image, ImageDraw
    ims=[Image.open(os.path.join(OUT,n+".png")) for n in names]
    h=int(w*ims[0].height/ims[0].width); rows=(len(ims)+cols-1)//cols
    S=Image.new("RGB",(cols*w,rows*h))
    for i,im in enumerate(ims):
        t=im.resize((w,h)); ImageDraw.Draw(t).text((6,4),names[i],fill=(255,255,0))
        S.paste(t,((i%cols)*w,(i//cols)*h))
    S.save(os.path.join(OUT,out+".png"))
def seq(steps, tag, cols=4):
    """steps: list of (btn or None, wait_s). screenshot after each."""
    names=[]
    for i,(b,w) in enumerate(steps):
        if b: tap(b)
        time.sleep(w); n=f"{tag}{i}"; shot(n); names.append(n)
    sheet(names, tag+"_sheet", cols)

def close_game():
    r=rect()
    if r: u.PostMessageW(r[4],0x10,0,0)
    for i in range(25):
        if not pid(): return True
        time.sleep(1)
    return False
def launch():
    off=size(SLOG)
    p=subprocess.Popen([sys.executable,r"C:\Users\cl672\Documents\Coding\MegaMix-Switch-Mods\pc-client-mod\tools\launch_game.py"],stdout=subprocess.DEVNULL)
    for i in range(120):
        if CID in since(SLOG,off): break
        time.sleep(1)
    time.sleep(5); return CID in since(SLOG,off)
def to_select():
    """title -> main menu -> song select, verified by SEL INIT in the server log"""
    off=size(SLOG)
    for i in range(12):
        if "SEL 'PcRenamed' INIT" in since(SLOG,off): time.sleep(2.5); return True
        tap("A"); time.sleep(2.2)
    return False
def wait_log(needle, off, timeout, path=None):
    path=path or SLOG; t0=time.time()
    while time.time()-t0<timeout:
        if needle in since(path,off): return True
        time.sleep(0.25)
    return False
def dbg_last():
    """last DBG telemetry line for the PC from the server log -> dict"""
    import re
    with open(SLOG,encoding="utf-8",errors="replace") as f:
        f.seek(max(0,size(SLOG)-6000)); t=f.read()
    ls=[l for l in t.splitlines() if "DBG 'PcRenamed'" in l]
    if not ls: return {}
    d=dict(re.findall(r"(\w+)=([-\d.]+)",ls[-1])); d["t"]=ls[-1][1:9]; return d

def unfocus_game():
    """Test aid: if the game window is foreground, minimize it through its
    own system menu (Windows then activates the previously used window) and
    restore it without activating. Sends no input and never takes the
    foreground away from another app."""
    r=rect()
    if not r or not r[5]: return True
    u.PostMessageW(r[4],0x0112,0xF020,0)      # WM_SYSCOMMAND / SC_MINIMIZE
    time.sleep(1.2); u.ShowWindow(r[4],4)     # SW_SHOWNOACTIVATE
    time.sleep(1.0); r=rect(); return bool(r) and not r[5]

def back_to_select():
    """from a solo song: open the pause menu if needed, Return to Song List"""
    off=size(SLOG)
    if pv()[0]!=1: return False
    if pv()[1]!=1: tap("Plus"); time.sleep(2.5)
    tap("Down",4); time.sleep(2.2); tap("A")
    ok=wait_log("SEL 'PcRenamed' INIT",off,25); time.sleep(3); return ok

def ensure_select(tries=12):
    """get the PC to song select from wherever it is (solo)"""
    for i in range(tries):
        off=size(SLOG)
        p=pv()
        if p[0]==1:
            back_to_select()
        else:
            tap("A"); time.sleep(2.2)
        if wait_log("SEL '",off,1.5) and "INIT" in since(SLOG,off) : time.sleep(2); return True
        lines=[l for l in since(SLOG,off).splitlines() if "SEL '" in l]
        if lines and "DEST" not in lines[-1]: return True
    return False
