import struct, re
from peread import rd, secs, base

pat = re.compile(rb'\.(\?AV|\?AU)[ -~]{4,120}?@@\x00')
for nm,va,vs,ra,rs in secs:
    if vs > 0x4000000: continue
    try: data = rd(base+va, max(vs,rs))
    except Exception: continue
    if not data: continue
    for m in pat.finditer(data):
        s = m.group(0)[:-1].decode(errors='replace')
        if 'ause' in s:
            print(nm, hex(base+va+m.start()), s)
