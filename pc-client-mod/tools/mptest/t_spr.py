import sys,time
from lead import *
from PIL import ImageGrab, Image
if not pid(): print("connected",launch(),flush=True)
admin({"cmd":"lobby_leave"}); print("sel",ensure_select(),flush=True)
time.sleep(1); tap("A"); time.sleep(1.5); tap("A")
for i in range(60):
    if pv()[0]==1: break
    time.sleep(0.5)
time.sleep(14)
frames=[]
for p in [int(x) for x in sys.argv[1:]]:
    admin({"cmd":"sprtest","prio":p}); time.sleep(0.6)
    r=rect(); im=ImageGrab.grab(all_screens=True).crop(r[:4]); W,H=im.size
    frames.append(im.crop((0,int(H*0.88),int(W*0.45),int(H*0.96))))
admin({"cmd":"sprtest","prio":-1})
w,h=frames[0].size; S=Image.new("RGB",(w,h*len(frames)))
for i,f in enumerate(frames): S.paste(f,(0,i*h))
S.save(os.path.join(OUT,"spr_prio.png")); print("done",flush=True)
