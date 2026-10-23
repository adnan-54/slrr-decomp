---
id: 089
status: todo
branch: card/089
depends: [086]
---
# game/traffic: Hermite segment evaluation + path walk (pos / dir / side)

## Goal
Port the segment and path evaluators the road queries use: segment position (0x4134e0),
direction (0x412f60), side normal (0x4131a0), and the path walkers posAt (0x413360) /
dirAt (0x412ed0). Pure math on the card-086 data; no natives yet (card 090 uses them).
Trace: notes/19 §2 "Segment / path geometry".

## Exe evidence (Claude-traced from the decompile; ECX = segment or path)
Segment fields (card 086): `p0 +0, p1 +0xc, m0 +0x18, m1 +0x24, len +0x30, curved +0x38`.
### 0x4134e0 ctSegment__pos(out, d)
    if (curved) {
        t = d / len; t2 = t*t; t3 = t2*t;
        h00 = (t3 + t3) - t2*3 + 1;  h01 = t2*3 - (t3 + t3);
        h10 = (t3 - (t2 + t2)) + t;  h11 = t3 - t2;
        out = m1*h11 + m0*h10 + p1*h01 + p0*h00;     // per component, this summation order
    } else
        out = (m0.x*d + p0.x, m0.y*d + p0.y, m0.z*d + p0.z);
### 0x412f60 ctSegment__dir(out, d)
    if (curved) {
        t = d / len; t2 = t*t;
        a = (t2 - t)*6;  b = (t - t2)*6;  c = (t2*3 - t*4) + 1;  e = t2*3 - (t + t);
        v = m1*e + m0*c + p1*b + p0*a;                 // per component
        l = sqrt(v.y² + v.x² + v.z²); if (l != 0) { inv = 1/l; v *= inv; }
        out = v;
    } else out = m0;
### 0x4131a0 ctSegment__side(out, d)
    if (curved) {
        t, a, b, c, e as in 0x412f60;
        dx =   m1.x*e + c*m0.x + p1.x*b + p0.x*a;
        nz = -(m1.z*e + m0.z*c + p1.z*b + p0.z*a);
        out = (nz, 0, dx);
        l = sqrt(dx*dx + nz*nz); if (l != 0) { inv = 1/l; out = (inv*nz, inv*0.0f, inv*dx); }
    } else out = m1;                                   // straight segments store the side normal in m1
### 0x413360 ctPath__posAt(out, d) / 0x412ed0 ctPath__dirAt(out, d)
    if (d < 0) { L = total(+0x38); if (L != 0) d = d - (float)(int)(d / L) * L; d = L + d; }
    i = 0;
    while (i < n - 1) { if (d < seg[i].len) break; d = d - seg[i].len; i++; }
    posAt: ctSegment__pos(&seg[i], out, d);   dirAt: ctSegment__dir(&seg[i], out, d);
(`(int)` truncates toward zero. With n == 0 the exe reads seg[0] out of bounds —
INVENTED: return zeros.)

## Changes
1. `src/game/traffic.h`: free functions in `namespace traffic` (or `Segment`/`Path`
   methods, same names):
   ```cpp
   void segment_pos(const Segment &s, float d, float out[3]);   // 0x4134e0
   void segment_dir(const Segment &s, float d, float out[3]);   // 0x412f60
   void segment_side(const Segment &s, float d, float out[3]);  // 0x4131a0
   void path_pos_at(const Path &p, float d, float out[3]);      // 0x413360
   void path_dir_at(const Path &p, float d, float out[3]);      // 0x412ed0
   ```
2. `src/game/traffic.cpp`: implement in float, keeping the operation order above (the
   exe uses SSE single precision); one shared `path_seg(p, &d)` helper for the walk is
   fine (`0x413360`/`0x412ed0` are copies of the same loop).

## Files
- edit: `src/game/traffic.h`, `src/game/traffic.cpp`

## Done when
- each function carries its `// 0xADDR Name` comment; the n == 0 guard is `INVENTED:`
- `make -C src clean check` green; no unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
