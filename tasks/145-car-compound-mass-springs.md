---
id: 145
status: todo
branch: card/145
depends: [144]
---
# Car compound mass and wheel spring constants (createCar wheel loop)

## Why
The woken freeride `car_root` weighs 1 kg (createBox mass 1, `PhysBody__updateCompoundMass` 0x5032d0
not ported) and `WheelData::spring_k/bump/rebound` stay 0 (the createCar formula is not ported), so
the existing `vehicle::Car` suspension cannot carry the car. Card 146 drives `Vehicle::update` per
sub-step; this card gives it the exe's mass and spring numbers. notes/09 "Drive path".

## Exe functions (3)
- `Vehicle__createCar` 0x4992e0, wheel loop (`i < M+0x1fb4`, template `M+0x2070 + i*0xc8`):
```
if (M+0x1f34 <= 0) M+0x1f34 = 1 / invMass(part_comp)            // spring weight, before the loop
W+0x38 = base; W+0x3c = travel; W+0x40 = ratio                    // cfg "spring base travel ratio"
W+0x50 = base + travel; W+0x54 = base + 0.1; W+0x58 = -1; W+0x5c = base + travel + r
W+0x44 = |g| * M+0x1f34 / M+0x1fb4 / W+0x3c                        // |g| = |world+0x1c4| = 9.81
W+0x48 = W+0x44 * W+0x40;  W+0x4c = 0.7 * W+0x48
Suspension__configure 0x4f4280(W+0x44, W+0x48, W+0x5c, r+W+0x54, r+W+0x58, W+0x4c, r)
```
- `Part__initInstance` 0x4c1f90: part_comp gets one child per kind-1/2 bone record (the cfg `body`
  lines), then `0x5032d0(part_comp)`.
- `PhysBody__updateCompoundMass` 0x5032d0 (mass part only):
```
M = sum over children with invMass != 0 of 1/invMass        // body's own shape mass NOT counted
com = mass-weighted mean of child offsets
I = sum m_i*(|r_i-com|^2*E - (r_i-com)(r_i-com)^T) + R_i*I_i*R_i^T
M == 0 -> invMass = 0, def flag 4 (static); else invMass = 1/M, inertia = I
recurse into the parent when the body is itself a child (part_comp -> car_root)
```

## Files (2 + check)
- src/game/car.h + src/game/car.cpp:
  - `fill_car`: read the `spring` line 3rd value into a new `WheelData::damp_ratio` (W+0x40).
  - `create_car_bodies` (after car_root exists): part_comp mass = sum of the cfg `body` masses;
    car_root mass = part_comp mass + per wheel the `type` line mass (values[0], the wheelbone child)
    + 0.01 (sensor capsule, 0x4f3f10). Inertia = point-mass parallel-axis terms about the COM from
    the body-line positions (`xform.p*`) and the wheel hubs. Store the COM offset in
    `CarData::com` (car_root local) for card 146. `shortcut:` children's own shape inertia and the
    COM re-centring of child offsets/pose are not ported (upgrade with real compound children).
  - Then per wheel: `spring_k = 9.81f * M1f34 / n / travel` with `M1f34 = part_comp mass`,
    `bump = spring_k * damp_ratio`, `rebound = 0.7f * bump`. Keep `rest_len/max_len/min_len/free_len`
    as fill_car sets them (already match W+0x50/54/58/5c).
- src/tools/vmboot.cpp: print car_root mass, M1f34 and wheel 0's k/bump/rebound for the player car.

## vmboot check (real data)
For the player car payload: `1/part_comp.inv_mass` equals the sum of the `body` masses of the car's
shipped cfg (re-read the cfg through `load_native_cfg`, |diff| < 1e-3), car_root mass equals that
plus the wheel type masses + 0.01 per wheel, wheel count equals the cfg `type` line count (>= 4), and
every wheel has `spring_k > 0`, `bump > 0`, `rebound > 0`. Existing wakeup checks stay green (the
1 kg -> ~1200 kg change must still rest on the soup: no wheels act yet, so the box rests on it).

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`; report the
  printed mass/k numbers.

## Not this card
Wheel/sensor/wheelbone bodies in the world, the drive hook (146), COM re-centring, shape inertia.
