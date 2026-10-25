---
id: 186
status: todo
branch:
depends: [185]
---
# `native bot` 0x483bd0 tail: match exe rounding, drop the INVENTED no-interval guard

## Why (notes/19 s8.3)
`bot_steer` keeps V unchanged when the solver has no intervals (INVENTED guard from card 181/185). The exe does
NOT do that: 0x40a8a0 always returns `len * (V * (1/len))` (ulp noise included), and 0x483bd0 then normalises
with a reciprocal multiply (src divides) and a double threshold. Port the exe arithmetic exactly.

## Exe evidence (all SSE single precision unless stated)
### 0x40a8a0 BotAvoid::resolve tail (asm 0x40aa50..0x40ab06) - src `BotAvoid::resolve` already matches
```
ang = |hi| > |lo| ? lo : hi        // no intervals: lo = hi = 0 -> ang = 0
c = cosf(ang); s = sinf(ang)       // 0 -> exactly 1, 0
x' = c*dx - dz*s; z' = s*dx + dz*c // dir stored back
out = (len * x', z' * len)         // len = +0x2c from 0x40ab10, dir = V * (1.0f/len)
```
So with no intervals the exe's V is `len*(V*(1/len))`, not V.

### 0x483bd0 after the resolve call (asm 0x4848b0..0x48496f)
```
0x40a8a0(&out, ...)                       // out = (x, z)
S+0x269c = out.x; S+0x26a0 = out.z        // b.plan_aim
len = sqrtss(out.z*out.z + out.x*out.x)
if ((double)len > 1e-5)                   // CVTPS2PD + COMISD vs double 0x593588 = 1e-5
    k = 1.0f / len; c = (out.x * k, out.z * k)
else c = (0, 0)
ang = 0x40a7d0(c, S.fwd.xz)               // p = (fwd.x, fwd.z)
S+0x25f0 = 0
0x482910(S, -ang, 0 > v2 ? 0 : v2)
```
### 0x40a7d0 angle(e = c, p = fwd.xz) (asm, float result via ST0)
```
g  = p.x*e.x + e.z*p.z;  cr = e.x*p.z - p.x*e.z
if (cr > 0): g > 1 -> +0.0f;  g < -1 -> +pi (0x40490fdb);  else acosf(g)
else:        g > 1 -> -0.0f;  g < -1 -> -pi;               else -acosf(g)
```
(src `bot_steer` returns +0.0f for `cr <= 0 && g > 1`; the exe returns -0.0f, i.e. steer = +0.0f.
`bot_avoid.cpp` `angle2` already has the -0.0f.)

## Current src (src/game/car.cpp, `bot_steer`, ~line 2682)
```cpp
    float rv[2];
    av.resolve(rv);  // 0x40a8a0
    // INVENTED: with no intervals resolve() is the identity rotation and len*dir only adds float
    // ulp noise to V (acosf below amplifies it into a different race); keep V exactly then.
    if (!av.intervals.empty()) { vx = rv[0]; vz = rv[1]; }
    b.plan_aim[0] = vx;  // +0x269c
    b.plan_aim[1] = vz;  // +0x26a0
    float len = sqrtf(vx * vx + vz * vz);
    float cx = 0.0f, cz = 0.0f;  // normalize(V), zero when |V| <= 1e-5
    if (len > 1e-5f) {
        cx = vx / len;
        cz = vz / len;
    }
    ...
    if (cr <= 0.0f)
        ang = g > 1.0f ? 0.0f : g < -1.0f ? -3.14159265f : -acosf(g);
```

## Change
1. Always `vx = rv[0]; vz = rv[1];` (delete the guard and its INVENTED comment).
2. `float len = sqrtf(vz * vz + vx * vx); if ((double)len > 1e-5) { float k = 1.0f / len; cx = vx * k; cz = vz * k; }`
   with a `// 0x483bd0: reciprocal multiply, double 1e-5 (0x593588)` comment.
3. `cr <= 0 && g > 1` -> `-0.0f`.
Nothing else. Do not touch `BotAvoid` (it already matches).

## Files
- edit: src/game/car.cpp (`bot_steer` only)

## Done when
- `make -C src clean check` green (clean build, not incremental).
- If the `--race` checks in vmboot fail after the change, do NOT re-add the guard: set status `blocked`, and
  report which check failed with its printed numbers (the exe has this noise; a failure means another src
  divergence, to be traced separately).
- no new tests or hand-made expected values; card frontmatter status/branch filled.
