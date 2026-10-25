---
id: 176
status: implemented
branch: card/176
depends: [175]
---
# `native bot` planner 0x486020 part 1: lateral offset + target point (steps 1, 3, 4)

## Why (notes/19 s8.1)
Card 175 stops at the 0x486020 call. This card ports the part of 0x486020 that picks the target point `T` and the
spline direction `D` there; the speed limit (step 5) and the steering call 0x483bd0 are card 177. Bots drive the
spline in **decreasing** param (every look-ahead is `d - x`).

## Exe (0x486020, ECX = bot, args S, look, vmax0 = 1000; spline queries from card 174, "dirAt" = path_dir_at 0x412ed0,
"w(q)" = width 0x414a80, "pof(q, o)" = path_pos_offset_at 0x4133f0)
```
x, z = S.pos.x, S.pos.z; 0x414830(x, z, &d, &lat)                    // path_project
if (i1(d) == 0) S.nitro(+0xb8) = -1;  S+0xc0 = (i2(d) != 0)           // 0x414a00, 0x413fd0
// traffic bins block (+0x2724 < 0 && DAT_005e652c Traffic): skipped, src race has no traffic manager -> bins 0
// step 1: lateral offset from the curvature ahead
lim = +0x2584 * 6; q = min(spline.length, d - 20); t0 = normalize_xz(dirAt(q)) (skip when |.| <= 1e-5); q -= 2
c = 1; acc = 0; off = 0
if (+0x2724 < 0 && q > 0) for (i = 0; i < 100; i++) {
    t = normalize_xz(dirAt(q)); g = t.z * t0.z + t.x * t0.x
    a = g > 1 ? 0 : g < -1 ? pi : acos(g); if (t.x * t0.z - t.z * t0.x <= 0) a = -a
    v = clamp(a * -15 * lim, -lim, lim)
    acc += c; off = (c / acc) * v + (1 - c / acc) * off
    q -= 2; c *= 0.96; if (q <= 0) break; t0 = t }
off = clamp(off, -0.8, 0.8)
// step 2 (opponent, mode 2/3 with opponent +0x25e8): 0x486766..0x486942 -> card 179 (shortcut)
// step 3: pull toward the car's current lateral position
if (mode == 3 || +0x2724 > 0) { r = lat / w(d); thr, k = (S.nitro > 0) ? (0.9, 0.25) : (0.2, 0.1)
    if (|off - r| < thr) { b = max(0, (|off - r| - thr + k) / k); off = clamp((1 - b) * r + b * off, -0.9, 0.9) } }
// step 4: target search from d - 10 back to L = +0x268c
f = dirAt(L) (x, z raw, not normalised); lo = -1; hi = 1; back = false; q = d - 10
do { q -= 5; if (q <= L) q = L
     h = w(q) - 1.4; if (h <= 1) h = 1                                  // 1.4 = +0x2594
     A = pof(q, h); B = pof(q, -h); C = pof(q, (h - 1.4) * off)
     s(P): u = (P.x - x, P.z - z), normalised (0,0 when |u| <= 1e-5); g = u.z f.z + u.x f.x
           s = 0.5 * ((u.x f.z - u.z f.x <= 0) ? -1 - g : g + 1)
     if (s(B) < s(A)) break
     if (s(C) < lo || hi < s(C)) { q += 5.5; back = true }
     else { lo = max(s(A), lo); hi = min(s(B), hi); if (back) break }
} while (L < q)
q2 = d - (d - q) * 0.8; if (q2 <= +0x2608) q2 = +0x2608               // reset: -1
D = dirAt(q2); T = pof(q2, (w(q2) - 1.4) * off)
u = normalize3(T - S.pos); step = 28; n = 100000
do { P = T + u * step
     if (path_project_strict(P.x, P.z, &pd, &pl) && |pl| < w(pd) && pd > +0x2608) { T = P; if (--n == 0) { n = 2; step *= 0.333333 } }
     else { n = 2; step *= 0.333333 }
} while (step >= 1)
// -> step 5 + 0x483bd0(S, T.x, T.z, -D.x, -D.z, vmax, vmax)  (card 177)
```

## What to change (<= ~150 lines; files: src/game/car.h, src/game/car.cpp)
Replace the card-175 0x486020 stub with `bot_plan(BotData&, BotSnapshot&, float look, float vmax0)` doing the code
above; store `d`, `lat`, `off`, `T`, `D` in BotData (diagnostics for vmboot and input for card 177). Keep exe
order and float types (no doubles). Steps 2 and the traffic block: `shortcut:` comments naming card 179.

## vmboot checks (--selfcheck and --race, drive-start block, per mode-3 bot, after >= 1 tick)
1. **Hard check**: `path_project_strict(T.x, T.z)` succeeds with `|lat_T| < w(d_T)` and `d_T` lies **behind** the car's
   `d` in spline order: `0 < wrap(d - d_T) < length / 2` (wrap into [0, length)); bots aim backwards along the
   spline (the straight-line extension loop can carry T far down a straight, so no tighter bound).
2. **Hard check**: `|off| <= 0.9`, `D` is a unit-length 3D vector (|D| within 1e-3 of 1), all values finite.
3. Print per bot: d, lat, off, T, d_T.

## Done when
`make -C src -j4 check` green; vmboot `--selfcheck` and `--race` (ulimit -v 1500000; timeout 300) PASSED with 1-2.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`). `INVENTED:` with the exe address for any gap.
No unit tests, no map names in engine code. Do not write axes.

## Not this card
Step 5 speed limit + 0x483bd0 (177), controls (178), opponent block / bot_trigger (179).

## Result
Implemented `bot_plan` (0x486020 steps 1, 3, 4) in `src/game/car.cpp`, called from the mode-3
look-ahead. Stores `plan_d/plan_lat/plan_off/plan_T/plan_D` in `BotData`. vmboot selfcheck and
race pass with the new T/off/D checks (selfcheck 163 checks, race 153, 0 failures).
