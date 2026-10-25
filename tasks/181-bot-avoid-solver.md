---
id: 181
status: merged
branch: card/181
depends: [178]
---
# `native bot` obstacle solver (global `DAT_005fe1f4`, 0x38 bytes): 0x40ab10 / 0x409fa0 / 0x40a6f0 / 0x40a8a0 + heap 0x46b5e0 / 0x40abe0

## Why (notes/19 s8.2)
0x483bd0 steers through a shared angle-interval solver: reset with the car front `F` and the aim vector `V`
(0x40ab10), one `add` per obstacle circle (0x409fa0, card 184 feeds it), then `resolve` (0x40a8a0) rotates `V` by
the smallest angle that leaves every blocked interval, nearest interval first. src's `bot_steer` uses `V`
unchanged (correct only with no obstacles). This card ports the solver as a standalone module and calls
reset + resolve from `bot_steer`; with no `add` calls the result is bit-identical to today (`V` = len * dir).

## Exe evidence
Layout (ints/floats, index = dword): `[0]` circles* (0x20 each) `[1]` nCircles `[2]` cap; `[3]` intervals* (0x20
each) `[4]` nIntervals `[5]` cap; `[6],[7]` F (x,z); `[8],[9]` dir (unit); `[10]` margin; `[0xb]` len; `[0xc]` range.
Circle: `+0 link` (next member index or -1), `+4 x, +8 z, +0xc vx, +0x10 vz, +0x14 r, +0x18 dist, +0x1c ang`.
Interval: `+0 head` (first member circle), `+4 a, +8 b` (radians), `+0xc gap` (min free distance), `+0x10 cx,
+0x14 cz, +0x18 R` (enclosing circle), `+0x1c maxr`. Arrays grow by 0x400 entries (realloc); src: std::vector.
`angle(e, p)` = 0x40a7d0 (ECX = e, both 2D): `g = p.x e.x + p.z e.z; cr = e.x p.z - p.x e.z;`
`cr <= 0 ? (g > 1 ? -0 : g < -1 ? -pi : -acos g) : (g > 1 ? 0 : g < -1 ? pi : acos g)`; `pi` = 3.1415927f,
`2pi` = 6.2831855f (0x5935a8). `asin` = 0x570ac1. All math float.
```
0x40ab10 reset(Fx, Fz, Vx, Vz, margin, minRange):            // called as (F, V, +0x2594, 0)
  free/clear both arrays (counts 0); F = (Fx,Fz); dir = (Vx,Vz); [10] = margin
  len = sqrt(Vz*Vz + Vx*Vx); [0xb] = len; range = len; if (len < minRange) range = minRange
  if (len > 0) dir *= 1/len

0x409fa0 add(x, z, vx, vz, r):
  c = new circle (index ci = nCircles++); c.x,z,vx,vz = args
  c.r = (margin + r) * 1.05
  c.dist = sqrt((F.x-x)^2 + (F.z-z)^2)
  gap = c.dist - c.r
  u = (x - F.x, z - F.z); l = |u|; u = (double)l > 1e-5 (0x593588) ? u / l : (0,0)
  c.ang = angle(dir, u)
  if (!(gap > 0)) { c.r = c.dist * 0.95; gap = c.dist - c.r; if (!(gap > 0)) { nCircles--; return } }
  if (gap > range) { nCircles--; return }
  h = asin(c.r / c.dist); a = c.ang - h; b = c.ang + h
  merged = -1
  for (k = 0; k < nIntervals; k++) {
    I = intervals[k]
    q = I.maxr * 3 + c.r * 3 + I.R - I.maxr
    if (!((I.cx - x)^2 + (I.cz - z)^2 < q*q)) continue           // (compare: q*q > dist^2)
    for (j = I.head; ; ) {
      m = circles[j]
      s  = m.r + c.r
      Px = m.x * (c.r / s) + x * (m.r / s) - F.x;  Pz = m.z * (c.r / s) + z * (m.r / s) - F.z
      L  = sqrt(Px^2 + Pz^2); if (L > 0) { Px /= L; Pz /= L }
      Dx = x - m.x; Dz = z - m.z; d = sqrt(Dx^2 + Dz^2)
      if (2*L > 3*s && d > s) { t = 1 - ((Dz*Pz + Dx*Px) / d)^2
                                thr = (t > 0) ? s * (3 / (2*sqrt(t) + 1)) : 3*s }
      else thr = s
      if (thr >= d) goto overlap
      j = m.link; if (j == -1) break
    }
    continue
   overlap:
    if (merged == -1) {
      aa = a; bb = b; while (aa < 0) { aa += 2pi; bb += 2pi }
      unite(I, aa, bb)
      if (gap < I.gap) I.gap = gap
      enclose(I, (x,z), c.r)                                   // 0x40a6f0
      c.link = m.link; m.link = ci                             // insert c after m
      merged = k
    } else {
      M = intervals[merged]
      aa = a; bb = b; while (aa < 0) { aa += 2pi; bb += 2pi }; unite(M, aa, bb)
      aa = I.a; bb = I.b; while (aa < 0) { aa += 2pi; bb += 2pi }; unite(M, aa, bb)
      if (gap < M.gap) M.gap = gap
      if (I.gap < M.gap) M.gap = I.gap
      enclose(M, (I.cx, I.cz), I.R)
      t = ci; while (circles[t].link != -1) t = circles[t].link
      circles[t].link = I.head                                 // append I's members
      nIntervals--; if (k < nIntervals) { intervals[k] = intervals[nIntervals]; k-- }
    }
  }
  if (merged != -1) return
  I = new interval (nIntervals++): head = ci
  while (a < 0) { a += 2pi; b += 2pi }
  I.a = a; I.b = b; I.gap = gap; I.cx = x; I.cz = z; I.R = c.r; I.maxr = c.r; c.link = -1

unite(I, aa, bb):     // inlined twice/three times in 0x409fa0, all float
  x0 = I.a
  if (x0 > aa && bb >= x0) I.a = aa
  else if (x0 + 2pi > aa && bb >= x0 + 2pi) { I.b += 2pi; I.a = aa }
  y = I.b
  if (bb > y && y >= aa) I.b = bb
  else if (bb + 2pi > y && y >= aa + 2pi) I.b = bb + 2pi

0x40a6f0 enclose(I, p, r):
  d = sqrt((I.cx - p.x)^2 + (I.cz - p.z)^2)
  if (d >= 0.001) { e = (d + r) - I.R
                    if (e > 0) { e *= 0.5; t = e / d; I.R += e
                                 I.cx = p.x * t + I.cx * (1 - t); I.cz = I.cz * (1 - t) + p.z * t } }
  else if (r > I.R) I.R = r
  if (r > I.maxr) I.maxr = r

0x40a8a0 resolve(&out)  (2 more stack args, +0x269c/+0x26a0, are never read; RET 0xc):
  heap.clear()                                 // global binary min-heap DAT_005e64e4 of (index, key) pairs
  for (i = 0; i < nIntervals; i++) heap_push(i, intervals[i].gap)          // 0x46b5e0
  lo = 0; loLim = -pi (0x5935b0); hi = 0; hiLim = pi (0x5935a4)
  while (heap not empty) {
    i = heap[0].index; heap_pop()                                         // 0x40abe0
    a = intervals[i].a; b = intervals[i].b
    while (a < -2pi) { a += 2pi; b += 2pi }
    while (a > 0)    { a -= 2pi; b -= 2pi }
    if (lo > a && b >= lo) { if (a > loLim || lo == 0) lo = a; else if (lo > loLim) lo = loLim }
    if (lo > b) loLim = max(b, loLim)
    while (b > 2pi) { b -= 2pi; a -= 2pi }
    while (b < 0)   { b += 2pi; a += 2pi }
    if (b > hi && hi >= a) { if (hiLim > b || hi == 0) hi = b; else if (hiLim > hi) hi = hiLim }
    if (a > hi) hiLim = min(a, hiLim)
  }
  ang = (|hi| > |lo|) ? lo : hi
  c = cos(ang); s = sin(ang); dx = dir.x; dz = dir.z
  dir.x = c*dx - s*dz;  dir.z = s*dx + c*dz
  out = (len * dir.x, len * dir.z)

0x46b5e0 heap_push(e):  n = size++; while (n != 0) { p = (n-1)/2; if (heap[p].key <= e.key) break
                                                      heap[n] = heap[p]; n = p }  heap[n] = e
0x40abe0 heap_pop():    size--; if (size == 0) return; i = 0; c = 1
  while (c < size) { r = 2*i + 2
    if (r < size && !(heap[c].key <= heap[r].key)) {      // right child smaller
        if (heap[size].key <= heap[r].key) break; heap[i] = heap[r]; i = r }
    else { if (heap[size].key <= heap[c].key) break; heap[i] = heap[c]; i = c }
    c = 2*i + 1 }
  heap[i] = heap[size]
```
Note on 0x40abe0: the exe compares `heap[c].key <= heap[r].key` and goes to the left-child branch in that case;
keep the exact comparisons (tie order decides which interval is processed first).

Use in 0x483bd0 (src `bot_steer`): `F = S.pos.xz + S.fwd.xz`, `V = T.xz - F` (already there) ->
`reset(F.x, F.z, V.x, V.z, b.half_width /*+0x2594*/, 0)` -> (bot_trigger create + obstacle walk: cards 182/184) ->
`resolve(&V)` -> `+0x269c/+0x26a0 = V` -> rest unchanged.

## Files
- new: src/game/bot_avoid.h, src/game/bot_avoid.cpp — `struct BotAvoid { ... reset(); add(); resolve(); }`
  with the circle/interval records above (int link/head, floats), the heap as a member vector (the exe's is a
  global; one solver is shared by all bots: one static instance, like `DAT_005fe1f4`)
- edit: src/Makefile — add `game/bot_avoid.cpp` to LIB_SRC (and src/Makefile.win if it lists sources)
- edit: src/game/car.cpp — `bot_steer`: reset before, resolve after (where the 0x483bd0 shortcut comment is)

## Done when
- each function has its `// 0xADDR` comment; nothing INVENTED except container types
- `make -C src clean check` green and vmboot --race output unchanged (no obstacles yet); no new unit tests
- card frontmatter: status implemented, branch card/181
