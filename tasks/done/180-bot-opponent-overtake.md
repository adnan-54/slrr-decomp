---
id: 180
status: merged
branch: card/180
depends: [178]
---
# `native bot` planner step 2: opponent overtake offset (0x486020 block 0x486766..0x486942)

## Why (notes/19 s8.1 step 2, s8.2)
`AI_spline <width> <file> <opponent car id>` stores an opponent (`+0x25e0` ResRefLink, resolved target `+0x25e8`).
In 0x486020, between step 1 (curvature offset, clamped to +-0.8) and step 3 (pull to the current lateral
position), the bot shifts its lateral line offset `off` to pass that opponent when it is close ahead on the
spline. It also raises the "overtaking" flag `+0x25f0`, which 0x483bd0 clears after steering (and which card 184's
obstacle walk reads: the opponent is skipped as an obstacle while the flag is set). src skips the block (shortcut
comment in `bot_plan`).

## Exe evidence (ECX bot; `d, lat` = own `ctPath__project` 0x414830 result of step 0; `off` = step-1 result)
```
if ((mode == 2 || mode == 3) && +0x25e8 != 0) {           // opponent resolved
  if (!Game__getInfo(opp, 2, &P)) goto done               // opp must answer getInfo 2 (car pos); only P.x, P.z used
  if (!0x414050 projectStrict(P.x, P.z, &pd, &plat)) goto done
  if (!(|plat| < w(pd))) goto done                        // w = 0x414a80 width
  r  = plat / w(pd)
  V  = getInfo(opp, 3)  body linear velocity, 3D; V /= |V| if |V| != 0
  Fo = getInfo(opp, 4)  -(R third column), 3D;     Fo /= |Fo| if |Fo| != 0
  if (Fo.V <= 0.8) goto done                              // opponent not driving forward
  if (d + 5 <= pd) goto done                              // more than 5 m behind (bots drive decreasing param)
  +0x25f0 = 1                                             // overtaking flag (byte)
  if (pd <= d - 20) goto done                             // more than 20 m ahead
  k    = +0x2594 * (1 / w(pd)) * 2.5                      // half_width margin in width units
  side = lat < plat                                       // byte
  if (d - 5 <= pd) minus = side
  else {
    Dp = ctPath__dirAt 0x412ed0(pd)                       // raw 3D tangent
    if (Dp.V > -0.8) { Sd = 0x413110 side(pd) (3D); minus = (Sd.V < 0) }
    else if (!side)  minus = (1 - k < r)                  // no room on the + side
    else             minus = !(r < k - 1)
  }
  if (!minus) { v = k * 1.3 + r; if (off <= v) off = (0.8 <= v) ? 0.8 : v }
  else        { v = r - k * 1.3; if (v < -0.8) off = -0.8; else if (v < off) off = v }
}
done:   // step 3 follows with this off
```
`Game__getInfo(opp, 3/4)` = `type_car__vf15` 0x4a15c0 cases 3/4 (same sources as the bot snapshot, notes/19 s8.1:
case 3 = root body linear velocity, case 4 = -(R col 2)), but here neither is flattened. 0x483bd0 ends with
`+0x25f0 = 0` right before 0x482910 (so the flag lives from the planner to the end of the same tick).

## Files
- edit: src/game/car.h — `BotData`: `uint8_t overtaking = 0; // +0x25f0`
- edit: src/game/car.cpp — `bot_plan`: replace the step-2 shortcut comment with the block above (opponent =
  the `g_cars` entry whose resource id == `b.opponent`, use its `root_body` pos/vel/rotation like `bot_snapshot`;
  no entry or no root_body = getInfo failure -> skip); `bot_steer`: `b.overtaking = 0;` before `bot_controls`
  (0x483bd0 `+0x25f0 = 0`). Helpers: `traffic::path_project_strict`, `path_width_at`, `path_dir_at`,
  `path_side_at` (src/game/traffic.cpp).
- read only: src/game/traffic.h

## Done when
- `// 0x486766` (step 2) comment; untraced bits `INVENTED:` (none expected beyond the id->car lookup)
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/180
