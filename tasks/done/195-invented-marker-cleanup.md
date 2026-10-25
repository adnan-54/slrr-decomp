---
id: 195
status: merged
branch: card/195
depends: []
---
# cleanup: INVENTED markers (cards 188/190) and 0x567630 singular case

## Goal
Three comment/behaviour nits left from cards 188 and 190. No new logic beyond removing one
invented fallback.

## Exe evidence
### 0x461dd0 camera setup, sin/cos order (asm 0x461f32..0x461f80) — verified, src already matches
```
x  = type+0x10 (fov) * [0x5993cc]          // 0x3c0ef921 = 0.0087266 = pi/360
c  = 0x570e7e(x)                            // cosf (0x570e7e -> 0x57e3f0 fcos, math.h:125)
s  = 0x570fcb(x)                            // sinf
k  = sqrtss(sqrtss(c / s))                  // DIVSS XMM1(c), XMM0(s): cos / sin
zf = type+0x18 * k
```
src/game/runtime.cpp:1428 `sqrtf(sqrtf(cosf(rad) / sinf(rad)))` is correct: only add the comment.

### 0x567630 general 4x4 inverse (row-major), no singular test
```
det = dot(row0 of m, cofactors 0x5677d0(0,0..3))
adj = 0x567400(m)                          // adjugate into this
r   = rcpps(det) (4 lanes)                 // hardware reciprocal estimate
for each of the 16 entries: out = ((r + r) - r*r*det) * adj   // one Newton step
```
No `det == 0` branch: a singular matrix yields inf/NaN entries, never identity.

## Changes
1. src/game/runtime.cpp `mat4_inverse` (~line 1321): delete the `identity when singular` fallback
   (the `if (det == 0) return identity` branch, whatever its exact form) and its comment; keep
   `k = 1.0f / det`. Comment: `// 0x567630: no singular test (det 0 -> inf/NaN).` Add
   `// INVENTED: 1/det instead of rcpps + one Newton step (rcpps is CPU-dependent).`
   Leave `render/device.cpp mat4_inverse_affine` alone (not 0x567630).
2. src/game/runtime.cpp:1428: append to the 0x461dd0 comment `cos/sin order verified (asm
   0x461f4c cosf, 0x461f5b sinf, DIVSS cos/sin)`.
3. src/tools/vmboot.cpp:2477-2481: the `steerhelp 0` command is a harness fixture (a player
   never sends it); prefix the comment with `INVENTED harness fixture:`.

## Files
- edit: src/game/runtime.cpp, src/tools/vmboot.cpp

## Done when
- the three edits above; `make -C src clean check` green (vmboot numbers unchanged: the pick
  matrices are never singular)
- card frontmatter: status implemented, branch name
