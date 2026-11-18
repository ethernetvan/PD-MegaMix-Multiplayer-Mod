import capstone, sys, struct
from peread import rd, base
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
targets={int(a,16) for a in sys.argv[1:]}
lo,hi=base+0x1000,base+0x1000+0xb23a00
code=rd(lo,hi-lo)
for ins in md.disasm(code,lo):
    for op in ins.operands:
        if op.type==capstone.x86.X86_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP:
            t=ins.address+ins.size+op.mem.disp
            if t in targets:
                print(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}  -> {t:x}")
