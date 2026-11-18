import sys, capstone
from peread import rd
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
for a in sys.argv[1:]:
    n=0x60
    if ':' in a: a,n=a.split(':'); n=int(n,16)
    a=int(a,16); print('==',hex(a))
    for i in md.disasm(rd(a,n),a):
        print(f"  {i.address:x}: {i.mnemonic} {i.op_str}")
        if i.mnemonic in('ret','int3'): break
