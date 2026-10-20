#!/usr/bin/env python3
"""Static string xrefs: for every function in analysis/procs.tsv, list strings whose address occurs as a 32-bit
immediate in its code; derive 'Class::method' name candidates from the strings. Usage: strxref.py <exe> <out.json>"""
import struct, sys, json, re, bisect, ast, collections
exe, out = sys.argv[1], sys.argv[2]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
text = None
for i in range(n):
    o = pe + 24 + osz + i * 40
    if d[o:o + 8].rstrip(b'\0') == b'.text':
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
        text = (base + va, base + va + vsz, rptr)
procs = [(int(l.split('\t')[0], 16), l.split('\t')[1].strip()) for l in open('analysis/procs.tsv')]
starts = [a for a, _ in procs]
strs = {}
for l in open('analysis/strings.tsv'):
    a, v = l.rstrip('\n').split('\t', 1)
    strs[int(a, 16)] = ast.literal_eval(v)
needle = {struct.pack('<I', a): a for a in strs}
res = collections.defaultdict(list)
t0, t1, rp = text
blob = d[rp:rp + (t1 - t0)]
for i in range(len(blob) - 3):
    w = blob[i:i + 4]
    if w in needle:
        va = t0 + i
        k = bisect.bisect_right(starts, va) - 1
        if k >= 0: res[starts[k]].append(needle[w])
cand = {}
for f, ss in res.items():
    c = collections.Counter()
    for a in ss:
        m = re.match(r'^((?:[A-Za-z_]\w*::)+[A-Za-z_~]\w*)', strs[a])
        if m: c[m.group(1)] += 1
    if c: cand[f] = c.most_common(3)
json.dump({'funcs': {hex(f): [hex(a) for a in v] for f, v in res.items()},
           'candidates': {hex(f): v for f, v in cand.items()}}, open(out, 'w'))
print('functions with string refs', len(res), 'with Class::method candidates', len(cand))
for f in sorted(cand): print(hex(f), cand[f])
