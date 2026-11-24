import sys, time
from lead import *
TAG=sys.argv[1]
if not pid(): log("launch connected:",launch())
admin({"cmd":"lobby_leave"})
log("song select:",to_select())
tap("A"); time.sleep(1.5); off=size(SLOG); tap("A")
log("loaded:",wait_log("loaded=1",off,40))
names=[]
def step(name,btn=None,n=1,wait=2.5):
    if btn: tap(btn,n)
    time.sleep(wait); w=shot(f"{TAG}_{name}"); names.append(f"{TAG}_{name}"); d=dbg_last()
    log(name,"white",w,"paused",d.get("paused"),"st",d.get("st"),"score",d.get("score"),"fg",rect()[5],"hung",rect()[6])
step("play5",None,1,5); step("play10",None,1,5)
step("pause1","Plus"); step("resume1","Plus"); step("play_after",None,1,4)
step("pause2","Plus"); step("nav","Down",4,2.5)
off=size(SLOG); tap("A"); t0=time.time()
for t in (2,6,12,20):
    time.sleep(max(0,t-(time.time()-t0)))
    w=shot(f"{TAG}_exit{t}"); names.append(f"{TAG}_exit{t}")
    log(f"exit+{t}s white",w,"SEL back:", "SEL 'PcRenamed' INIT" in since(SLOG,off),"hung",rect()[6])
sheet(names,TAG+"_sheet",cols=4)
