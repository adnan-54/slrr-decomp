#!/usr/bin/env python3
"""Extract the native-method table registered by FUN_004d5fb0 (calls to JVM::registerNative 0x55c690).
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
START, END, REG = 0x4d5fb0, 0x4d8484+0x200, 0x55c690
f0, f1 = off(START), off(END)
res = []
i = f0
while i < f1:
    if d[i] == 0xE8:
        rel = struct.unpack_from('<i', d, i+1)[0]
        va = START + (i-f0)
        if va+5+rel == REG:
            # collect preceding push imm32 (0x68)
            pushes = []
            j = i-1
            # walk back scanning 5-byte pushes, skipping other short insns
            k = i
            while len(pushes) < 4 and k > f0 and i-k < 60:
                k -= 1
                if d[k] == 0x68 and k+5 <= i:
                    pushes.append(struct.unpack_from('<I', d, k+1)[0])
                    k -= 0  # continue scanning
            # pushes are collected last-to-first: fn? order (cdecl-ish thiscall: last arg pushed first)
            res.append((va, pushes))
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
