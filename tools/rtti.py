#!/usr/bin/env python3
"""Recover MSVC RTTI (32-bit PE) -> class names, vftables, bases. Usage: rtti.py <exe> <out.json>"""
import struct, sys, json, re

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + osz + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode()
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((name, base + va, max(vsz, 0), rptr, rsz))
    return d, base, secs

def demangle(raw):
    m = re.match(r'\.\?A[VU](.*)@@$', raw)
    if not m: return raw
    s = m.group(1)
    t = re.match(r'\?\$(\w+)@(.*)@@@$', s) or re.match(r'\?\$(\w+)@(.*)@$', s)
    if t:
        args = re.findall(r'[VU](\w+)@@', t.group(2) + '@@')
        return f"{t.group(1)}<{', '.join(args)}>"
    parts = s.split('@')
    return '::'.join(reversed([p for p in parts if p]))

def main(exe, out):
    d, base, secs = load(exe)
    def off(va):
        for n, sva, vsz, rptr, rsz in secs:
            if sva <= va < sva + max(vsz, rsz): return rptr + va - sva
        return None
    def u32(va):
        o = off(va)
        return None if o is None or o + 4 > len(d) else struct.unpack_from('<I', d, o)[0]
    def insec(va, name):
        return any(n == name and sva <= va < sva + vsz for n, sva, vsz, _, _ in secs)
    # 1. type descriptors: strings ".?AV"/".?AU" at va, TD = va-8
    tds = {}
    for m in re.finditer(rb'\.\?A[VU][\x21-\x7e]+?@@\x00', d):
        o = m.start()
        va = None
        for n, sva, vsz, rptr, rsz in secs:
            if rptr <= o < rptr + rsz: va = sva + o - rptr
        if va is None: continue
        raw = m.group(0)[:-1].decode()
        tds[va - 8] = raw
    # 2. scan all dwords in data sections for COLs: [sig=0, off, cdoff, pTD, pCHD, (pSelf in x64 only)]
    cols = {}
    for n, sva, vsz, rptr, rsz in secs:
        if n not in ('.rdata', '.data', '_RDATA'): continue
        for o in range(rptr, rptr + min(vsz, rsz) - 20, 4):
            sig, od, cd, ptd, pch = struct.unpack_from('<IIIII', d, o)
            if sig == 0 and ptd in tds and insec(pch, '.rdata') | insec(pch, '.data') | insec(pch, '_RDATA') and od < 0x10000 and cd < 0x10000:
                cols[sva + o - rptr] = (od, cd, ptd, pch)
    # 3. vftables: dword == COL va ; vftable starts at next dword
    vft = {}
    for n, sva, vsz, rptr, rsz in secs:
        if n not in ('.rdata', '.data', '_RDATA'): continue
        for o in range(rptr, rptr + min(vsz, rsz) - 4, 4):
            v = struct.unpack_from('<I', d, o)[0]
            if v in cols:
                vva = sva + o - rptr + 4
                fns = []
                k = vva
                while True:
                    f = u32(k)
                    if f is None or not insec(f, '.text'): break
                    if k != vva and k in {c + 0 for c in ()}: break
                    fns.append(f); k += 4
                    if u32(k) in cols: break   # next vftable's COL pointer
                vft.setdefault(v, []).append((vva, fns))
    # 4. class hierarchy
    res = {}
    for col, (od, cd, ptd, pch) in cols.items():
        nb = u32(pch + 8); arr = u32(pch + 12)
        bases = []
        for i in range(nb):
            bcd = u32(arr + 4 * i)
            btd = u32(bcd)
            bases.append(demangle(tds.get(btd, '?')))
        name = demangle(tds[ptd])
        for vva, fns in vft.get(col, []):
            res.setdefault(name, []).append({
                'raw': tds[ptd], 'col': hex(col), 'vftable': hex(vva), 'this_offset': od,
                'bases': bases[1:], 'nvfuncs': len(fns), 'vfuncs': [hex(f) for f in fns]})
    json.dump({'image_base': hex(base), 'type_descriptors': len(tds), 'cols': len(cols), 'classes': res},
              open(out, 'w'), indent=1)
    print('typedescs', len(tds), 'COLs', len(cols), 'classes with vftable', len(res),
          'vftables', sum(len(v) for v in res.values()))

main(sys.argv[1], sys.argv[2])
