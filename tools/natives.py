#!/usr/bin/env python3
"""Extract the native-method table: every call to JVM::registerNative (0x55c690) in .text.
Usage: natives.py <exe> <out.json>"""
import struct, sys, json, re
exe, out = sys.argv[1], sys.argv[2]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
opt = struct.unpack_from('<H', d, pe+20)[0]
base = struct.unpack_from('<I', d, pe+24+28)[0]
secs = []
o = pe+24+opt
for i in range(nsec):
    vs, va, rs, ro = struct.unpack_from('<IIII', d, o+8+i*40)
    secs.append((va+base, vs, ro, rs))
def off(va):
    for a, vs, ro, rs in secs:
        if a <= va < a+max(vs, rs): return ro+va-a
def cstr(va):
    f = off(va)
    if f is None: return None
    e = d.index(b'\0', f)
    return d[f:e].decode('latin1')
START, END, REG = 0x401000, 0x591f00, 0x55c690   # whole .text: all registrars (0x4ac310, 0x4c6570, 0x4d2550, 0x4d5fb0)
f0, f1 = off(START), off(END)
res = []
i = f0
while i < f1:
    if d[i] == 0xE8:
        rel = struct.unpack_from('<i', d, i+1)[0]
        va = START + (i-f0)
        if va+5+rel == REG:
            # layout: 4 x (68 imm32), pushed right-to-left, with one `mov ecx,[vm]` (8b 0d imm32) somewhere between
            pushes = []
            k = i
            for n in range(4):
                if d[k-6:k-4] == b'\x8b\x0d':
                    k -= 6
                if d[k-5] != 0x68: pushes = []; break
                k -= 5
                pushes.append(struct.unpack_from('<I', d, k+1)[0])   # walking back: class first
            res.append((va, pushes))
            if not pushes: print('MISS', hex(va), d[i-24:i+5].hex(' '))
    i += 1
tbl = []
for va, p in res:
    if len(p) < 4: continue
    # args pushed right-to-left: first push = param_4(fn), then param_3(sig idx/ptr), param_2 method, param_1 class
    # scanning backwards yields param_1 .. wait: last push is param_1 => first found = param_1
    cls, meth, sig, fn = p[0], p[1], p[2], p[3]
    tbl.append(dict(call=hex(va), cls=cstr(cls), meth=cstr(meth), sig=cstr(sig), fn=hex(fn)))
json.dump(tbl, open(out, 'w'), indent=1)
print(len(res), len(tbl))
for t in tbl[:12]: print(t)
