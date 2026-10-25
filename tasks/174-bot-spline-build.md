---
id: 174
status: todo
branch: card/174
depends: [173]
---
# `native bot` spline: build the AI_spline rows into a `traffic::Path` (0x4122b0/0x414fe0/0x4124d0) + bot queries

## Why (notes/19 s8.1)
Card 173 stores the `.spl` rows only. The bot's spline object (+0x2604) is the traffic `ctPath` class that src already
has as `traffic::Path`/`Segment` (src/game/traffic.*, .trc loader, `path_pos_at` 0x413360, `path_dir_at` 0x412ed0,
`segment_side` 0x4131a0, `path_project` 0x414830). Missing: the Hermite build from (pos, dir) pairs, the 0x54-byte
segment's per-row fields, and four queries the planner (cards 176/177) uses.
Script does: `AI_spline <width> <file> <opp>` (Bot.java:491). Exe provides: the spline build and queries.

## Exe (ECX = path or segment)
```
0x4122b0 Path::addSegment(p0, d0, p1, d1, w, f1, i1, f2, f3, i2):
  n++, realloc segs (0x54 each); seg = segs[n-1]; 0x414fe0(seg, p0, d0, p1, d1)
  seg+0x3c = w; +0x40 = f1; +0x44 = i1; +0x48 = f2; +0x4c = f3; +0x50 = i2
  path+0x38 (length) += seg.len                        // float sum (Ghidra shows an int cast: same bits)
  if first: start = p0; start_dir = normalize(seg.m0)
  end = p1; end_dir = normalize(seg.m1)                // every segment (NOT -m0 like the .trc loader)
0x414fe0 Segment::buildHermite(p0, d0, p1, d1):
  p0, p1 stored; curved(+0x38) = 1
  c = normalize(p1 - p0); m0 = normalize(d0); m1 = normalize(d1)      // normalize: skip when length == 0
  L = |p0 - p1|; f0 = sqrt(|m0| / L); f1 = sqrt(|m1| / L)             // |m| == 1 after normalize
  s0 = (m0.c + 1) * 0.5 <= 0 ? 0 : sqrt(...); s1 likewise with m1
  len(+0x30) = L / ((s1 * f1 + 1 - f1) * (s0 * f0 + 1 - f0)); m0 *= len; m1 *= len
  old = len
  do { 0x4124d0(seg); k = len / old; m0 *= k; m1 *= k; ch = (len - old) / old; old = len } while (|ch| > 0.01)
  +0x34 = len; +0x40 = 1.0; +0x44 = 1; +0x48 = 10000; +0x4c = 10000; +0x50 = 0
0x4124d0 Segment::arcLength:
  len = 1.0; prev = segment_pos(0)  (0x4134e0; with len 1: t = d)
  sum1 = sum over i = 1..50  of |P(i*0.01) - P((i-1)*0.01)|   (P = Hermite p0,p1,m0,m1 at t; straight: p0 + m0 t)
  sum2 = sum over i = 51..100 likewise (separate float accumulator, continues from P(0.50))
  len = sum2 + sum1
0x414050 Path::projectStrict(x, z, &d, &lat): like path_project 0x414830 but a segment whose segment_project
  (0x414480 logic inlined) fails is skipped (no endpoint fallback); first strict |lat| minimum wins;
  d = local d + sum of earlier seg.len; returns found.
0x4133f0 Path::posOffsetAt(out, d, off): wrap/walk as path_pos_at; out = segment_pos(local) + segment_side(local) * off
Getters, same wrap/walk (negative d: d -= int(d/len)*len, d += len; walk while d >= seg.len up to n-1):
  0x414a80 width = seg+0x3c; 0x414980 seg+0x40; 0x414a00 seg+0x44; 0x413fd0 seg+0x50;
  0x413f30 t = local/seg.len, (1-t)*seg+0x4c + t*seg+0x48; 0x413110 side = segment_side(local)
0x482630 (card 173) calls 0x4122b0(prev pos, prev dir, pos, dir, w, f1, i1, f2, f3, i2) for every row after the first;
  per-row w < 0 -> base = +0x2594 + cmd width; ctor 0x47ed10: **+0x2594 = 1.4** (card 173 used 0, INVENTED).
```

## What to change (<= ~150 lines; files: src/game/traffic.h, src/game/traffic.cpp, src/game/car.cpp; car.h one field)
1. traffic.h `Segment`: add `width` (+0x3c), `i1` (+0x44), `f2` (+0x48), `f3` (+0x4c), `i2` (+0x50); the existing
   `k40` is +0x40 (f1). Declare `segment_build_hermite` (0x414fe0 incl. 0x4124d0), `path_add_segment` (0x4122b0),
   `path_project_strict` (0x414050), `path_pos_offset_at` (0x4133f0), `path_side_at` (0x413110) and the getters
   (one helper `path_seg_at(p, d, &local)` doing the shared wrap/walk, then `width/f1/i1/i2/lerp` one-liners).
   Reuse `segment_pos`/`segment_side`/`segment_project`; keep the .trc loader untouched.
2. car.cpp AI_spline (0x482630): build `BotData::spline` (new `traffic::Path` field in car.h) with
   `path_add_segment` per row after the first; base width uses 1.4 for +0x2594 (drop the INVENTED 0).
   Keep `spline_points` (vmboot reads them).

## vmboot checks (--selfcheck and --race, drive-start block after beginRace, per bot i < racers)
1. **Hard check**: `spline.segs.size() == spline_points.size() - 1` (37) and `spline.length` >= the chord sum of
   the file points (Hermite arc >= chord); print length and chord sum (route_1 scratch value: 2431.41 / 2427.42).
2. **Hard check** (build vs the already ported 0x414830): for every file point k = 1..36,
   `path_project(p_k.x, p_k.z)` gives `|lat| < 0.05` and `|d - sum(seg[0..k-1].len)| < 0.05`.
3. **Hard check**: `path_project_strict` of the `cpTrigger[0]` trigger position (the script's checkpoint built from
   Norisring track_data.java:42; vmboot's `check_triggers` block ~vmboot.cpp:1271 already resolves these triggers;
   notes/19 s8: cp0 ~ (48.8, -542.4)) on bot 0's route_1 lands in segment 27 or 28 and `|lat| < width`.
4. Print per bot: segments, length, width(0), i2(0), lerp(0) (10000).

## Done when
`make -C src -j4 check` green; `ulimit -v 1500000; timeout 300 src/build/vmboot --selfcheck` and `--race` PASSED
with 1-3. Result: printed lengths, any changed check.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, Result names the first blocker). Mark gap logic
`INVENTED:` with the exe address. No unit tests, no map names in engine code (only vmboot). No planner/tick.

## Not this card
Bot tick 0x47f980 (175), planner 0x486020 (176/177), controls 0x482910 (178).

## Result
