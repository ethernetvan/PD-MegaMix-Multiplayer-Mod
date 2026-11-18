import re
from peread import rd, secs, base
pat = re.compile(rb'\.\?AV[ -~]{4,120}?@@')
tot = 0
for nm,va,vs,ra,rs in secs:
    if vs > 0x4000000: continue
    data = rd(base+va, max(vs,rs))
    if not data: continue
    hits = list(pat.finditer(data))
    print(nm, len(hits))
    for m in hits[:5]:
        s = m.group(0).decode(errors='replace')
        if 'ause' in s: print('   *', hex(base+va+m.start()), s)
        tot += 1
