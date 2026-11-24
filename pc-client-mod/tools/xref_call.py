import capstone, sys
from peread import rd, base
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
targets={int(a,16) for a in sys.argv[1:]}
lo,hi=base+0x1000,base+0x1000+0xb23a00
code=rd(lo,hi-lo)
for ins in md.disasm(code,lo):
    if ins.mnemonic in ('call','jmp') and ins.operands and ins.operands[0].type==capstone.x86.X86_OP_IMM:
        if ins.operands[0].imm in targets:
            print(f"{ins.address:x}: {ins.mnemonic} {ins.operands[0].imm:x}")
