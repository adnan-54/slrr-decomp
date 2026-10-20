#!/usr/bin/env python3
"""Append per-class native listings (classes not yet in the notes) from analysis/natives.tsv. Usage: native_notes.py <notes.md> <class-prefix>..."""
import sys, collections
notes, prefixes = sys.argv[1], tuple(sys.argv[2:])
by = collections.defaultdict(list)
for l in list(open('analysis/natives.tsv'))[1:]:
    fn, c, m, sg = l.rstrip('\n').split('\t')
    if c.startswith(prefixes): by[c].append((m, sg, fn))
with open(notes, 'a') as f:
    f.write("\n## Additional registrars\n"
            "`JVM::registerNative` (0x55c690) has four callers: 0x4d5fb0 (core `java.*`, above), 0x4ac310 (vehicle), 0x4c6570 and 0x4d2550. "
            "`tools/natives.py` scans all of .text: **422 natives** in total (`analysis/natives.tsv`).\n")
    for k in sorted(by):
        f.write(f"\n### {k} ({len(by[k])})\n")
        for m, sg, fn in by[k]: f.write(f"- `{m}{sg}` -> {fn}\n")
