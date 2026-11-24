import struct,sys
P=r"C:\Program Files (x86)\Steam\steamapps\common\Hatsune Miku Project DIVA Mega Mix Plus\DivaMegaMix.exe"
f=open(P,'rb'); d=f.read(4096)
pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<Q',d,pe+24+24)[0]
secs=[]
o=pe+24+opt
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',d,o+40*i); secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs))
def rd(addr,n):
    rva=addr-base
    for nm,va,vs,ra,rs in secs:
        if va<=rva<va+max(vs,rs):
            f.seek(ra+rva-va); return f.read(n)
if __name__=='__main__':
    print(hex(base)); [print(s[0],hex(s[1]),hex(s[2])) for s in secs]
    for a in sys.argv[1:]:
        a=int(a,16); print(hex(a), rd(a,16).hex(' '))
