---
id: 177
status: merged
branch: card/177
depends: [176]
---
# `native bot` planner 0x486020 part 2: speed limit (step 5) + 0x483bd0 steering without obstacles

## Why (notes/19 s8.1)
After card 176 the bot has `T`/`D`. This card ports the speed limit and the steering angle that 0x483bd0 hands to
0x482910(S, steer, speed). The controls themselves (axis writes) are card 178; here the pair is stored only.

## Exe
```
0x486020 step 5 (after the target search; d, lat from step 0; vmax = vmax0 = 1000):
a0 = (mode == 3) ? 5 : 15; dist = a0
reach = S.speed^2 / (2 * +0x2580) * 1.5; if (mode == 3) reach *= 2; reach += a0      // +0x2580 = 7.2 (0x485e10)
q = d - a0; t = dirAt(q); prev = normalize_xz(t) (skip <= 1e-5); prevY = t.y
if (dist < reach) while ((q -= 2) >= 0) {
    t = dirAt(q)
    dec = +0x2580 - t.y * 9.81; dec = clamp(dec, 0, +0x2580)
    f1 = seg_f1(q)                                      // 0x414980, .spl default 1.0
    txz = normalize_xz(t)
    kk = 1                                              // traffic: (Traffic+0x312c / w(q)) / 3 + 2/3 (skipped)
    g = (S.w0.f2 + S.w1.f2 + S.w2.f2 + S.w3.f2) * 0.25  // S+0x4c,+0x64,+0x7c,+0x94
    if (+0x25d8 < g) +0x25d8 = g
    mu = +0x2584 * +0x25d8 * (sqrt(1 - t.y^2))^2
    ca = clamp(txz.x * prev.x + txz.z * prev.z, -1, 1); a = acos(ca)
    if (a > 0) vmax = min(vmax, sqrt(mu * 9.81 * 2 / a / kk * f1 * f1 + 2 * dec * dist))
    if (mode == 3) { lim = seg_lerp(q) * 0.2777778          // 0x413f30, km/h -> m/s (10000 default)
                     vmax = min(vmax, sqrt(2 * dec * dist + (+0x258c * lim * 2.5)^2)) }
    dY = asin(t.y) - asin(prevY)                            // 0x570ac1 asinf
    if (+0x2724 < 0 && dY > 0.001)
        vmax = min(vmax, sqrt(cos(dY) * S.wheelbase * 4.905 / sin(dY) * f1 * f1 + 2 * dec * dist))
    prevY = t.y; dist += 2; if (dist >= reach) break; prev = txz }
if (S+0xc0) { r = |lat| / w(d); a = max(1.8 - r, 1.01) * S.speed
              if (max(a, vmax * 0.5) <= vmax) vmax = max(a, vmax * 0.5)
              S+0xc4 = min(r, 1) }
0x483bd0(S, T.x, T.z, -D.x, -D.z, vmax, vmax)

0x483bd0 (S, tx, tz, dx, dz, v, v2), no-obstacle path:
F = (S.x + S.fwd.x, S.z + S.fwd.z); V = (tx - F.x, tz - F.z)
0x40ab10(solver, F, V, +0x2594, 0): clears the interval lists; len = |V|; dir = V / len (len > 0)
[bot_trigger create when +0x2700 == 0, obstacle walk +0x26b4 -> card 179]
0x40a8a0(&V, ...): no intervals -> chosen angle 0 -> V = len * dir (unchanged)
+0x269c, +0x26a0 = V; c = normalize(V) (0 when |V| <= 1e-5)
ang = 0x40a7d0(c, (S.fwd.x, S.fwd.z)): g = f.x c.x + c.z f.z; s = c.x f.z - f.x c.z
      s <= 0 ? (g > 1 ? 0 : g < -1 ? -pi : -acos g) : (g > 1 ? 0 : g < -1 ? pi : acos g)
+0x25f0 = 0; 0x482910(S, steer = -ang, speed = max(0, v2))
```

## What to change (<= ~120 lines; files: src/game/car.h, src/game/car.cpp)
Extend `bot_plan` with step 5 and a `bot_steer` (0x483bd0 no-obstacle path) that stores `steer`, `speed`,
`+0x269c/+0x26a0` and `+0x25d8` in BotData. `shortcut:` comments for the traffic term, bot_trigger and the obstacle
walk (card 179). No axis writes.

## vmboot checks (--selfcheck and --race, drive-start block, per mode-3 bot, after >= 1 tick)
1. **Hard check**: `0 < speed <= 1000`, `|steer| <= pi`, all finite; `+0x25d8 > 0` (the grid cars have grip).
2. Print per bot: vmax, steer (deg), reach, +0x25d8. (No recomputed-formula checks; the driving check is card 178.)

## Done when
`make -C src -j4 check` green; vmboot `--selfcheck` and `--race` (ulimit -v 1500000; timeout 300) PASSED with 1.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`). `INVENTED:` with the exe address for any gap.
No unit tests, no map names in engine code.

## Not this card
0x482910 and its callees (178), opponent block, bot_trigger, obstacles, mode 8 (179).

## Result
Implemented 0x486020 step 5 (braking-distance look-ahead: curvature, mode-3 km/h limit, crest term, max grip +0x25d8,
off-centre +0xc0 speed factor) and `bot_steer` (0x483bd0 no-obstacle path: V, angle to S.fwd, steer/speed pair).
Stored in `BotData`: `plan_vmax`, `plan_reach`, `plan_steer`, `plan_speed`, `plan_aim[2]` (+0x269c/+0x26a0) and
`max_grip` (+0x25d8). vmboot checks vmax/steer/reach/+0x25d8 per bot. `make -C src clean check` green;
selfcheck 164 checks, race 154, 0 failures.
Shortcuts (cards 179/178): traffic-density `kk` = 1, bot_trigger + obstacle walk, no axis writes. The steering
`dx/dz = -D` argument is passed by 0x486020 but unused on the no-obstacle path.
