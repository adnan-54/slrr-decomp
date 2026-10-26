---
id: 226
status: merged
branch: card/226
depends: [225]
---
# Vehicle: transmission/clutch wear, brake heat/wear/failure, bearing friction into the sim

## Goal
Port the wear writes of `Vehicle__update` 0x49c8e0 §3 (clutch coupling) and §4 (brakes), notes/34 §5-§6.
Also fix a gap found while tracing: the wheel bearing friction W+0xf0 (`WheelRef.setBearing`, stored in
src as `WheelData::damage_visual`, which is a misnomer) is never copied into `vehicle::Wheel::bearing`,
so the sim brakes as if bearing were 0. After this card brakes heat up and wear (lower effective
brake/handbrake, a fully worn brake adds 100 bearing drag and keeps 20 %), and the transmission/clutch
wear the HUD letters T/L/O read starts moving.

## Exe evidence (cleaned)
K = Config.internal_damage (card 225 already brings it into the sim as `k_dmg`); `wear_off` = car
flags & 0x400 (card 225). health(w) = w < 1 ? 1 - w*w*0.6 : 0.100000024 (src `wear_factor`).
`clamp01(x)`: if x < 0 -> 0 (exe logs "intdamage_data: part with invalid wear detected!"); if x >= 1 -> 1.

### §3, 0x49e1bf..0x49e2f6: only in the coupled branch (engine running and gear ratio != 0),
right after `clutch_slip` (car+0x1e88) is written. vehicle.cpp names: k, wd_new, we_new, clutch_slip.
```
if (!wear_off) {
    x = |(wd_new - we_new) * k * 0.1| * K
    if (x > 0) trans_wear = clamp01(trans_rate * x + trans_wear)        // 0x1ea4 / 0x1ea8
    y = |clutch_slip| * 0.01 ; y = y*y ; y = y*y * dt * 0.0001 * K      // (|slip|*0.01)^4 * dt * 1e-4 * K
    if (y > 0) clutch_wear = clamp01(clutch_rate * y + clutch_wear)     // 0x1eb4 / 0x1eb8
}
```
### §4, 0x49e5xx..0x49e8de: per wheel with |ω| > 0.001 (existing brake loop in vehicle.cpp)
```
T_b = (extra_brake + brake) * brake_torque_factor * brake_eff
      + handbrake_torque_factor * hbrake * handbrake_eff + bearing          // ported
if (T_b > 0) {
    ... ω update (ported) ...                                                // ω' = w.spin afterwards
    if (wear_off) continue                    // goto next wheel: no heat, no wear, NO cooling
    q = |ω'| * (T_b - bearing) * 10 * 0.001 * dt * 50
    if (q > 0) {
        brake_temp = q / brake_heat_cap + brake_temp                         // W+0x188, W+0x184
        if (brake_wear < 1 && q * K > 0) {                                   // W+0x164
            brake_wear = clamp01(brake_rate * q * K + brake_wear)            // W+0x168
            h = health(brake_wear)
            brake_eff = brake * h ; handbrake_eff = handbrake * h            // W+0xd8 = W+0xd0*h, W+0xdc = W+0xd4*h
            if (brake_wear >= 1) { bearing += 100 ; brake_eff *= 0.2 ; handbrake_eff *= 0.2
                                   // event M+0x1f6c (brake failure sound): shortcut, not posted
            }
        }
    }
}
if (!wear_off) { d = brake_temp - brake_ambient ; brake_temp -= |d| * brake_cool * dt * d }   // W+0x18c, W+0x190
```
(The exe loop also needs the wheel's suspension object S; every src sim wheel has one.)
### 0x4992e0 Vehicle__createCar per-wheel start values
`W+0x184` brake heat cap = 0x47435000 (50000.0f), `W+0x188` brake temp = 20.0f, `W+0x18c` ambient =
20.0f, `W+0x190` brake cooling = 0x3a83126f (0.001f). Keep these in `vehicle::Wheel` (the sim wheel
persists across ticks; the glue does not overwrite them).

## Where in src
- vehicle.h `Wheel`: add brake_wear, brake_rate, brake_temp, brake_ambient, brake_heat_cap, brake_cool
  (start values above, wear/rate 0). `Car`: trans_wear/rate, clutch_wear/rate (card 225 added k_dmg and
  wear_off).
- vehicle.cpp: the two blocks above at the exe positions in `update_engine_and_drivetrain`. Note
  `Wheel::set_brake/set_handbrake` set base and eff together; the glue below must set them separately.
- car.cpp `drive_cars` glue, before `sim.update`: `sw.bearing = w.damage_visual` (W+0xf0);
  `sw.brake = w.brake; sw.brake_eff = w.brake_eff; sw.handbrake = w.hbrake; sw.handbrake_eff = w.hbrake_eff`
  (instead of set_brake/set_handbrake); `sw.brake_wear/brake_rate = w.brake_wear/brake_rate`;
  car-level trans/clutch wear + rates. After: copy back `w.brake_wear`, `w.brake_eff`, `w.hbrake_eff`,
  `w.damage_visual = sw.bearing`, `trans_wear`, `clutch_wear`.

## Files
- edit: src/vehicle/vehicle.h, src/vehicle/vehicle.cpp, src/game/car.cpp
- read only: src/game/car.h (WheelData brake/hbrake/brake_eff/hbrake_eff/brake_wear/brake_rate/damage_visual)

## Done when
- `// 0xADDR` comments on the ported blocks; glue lines `INVENTED:`; unposted event `shortcut:`
- do not rename `WheelData::damage_visual` (not part of this card); comment at the glue line that it is W+0xf0 bearing
- `make -C src clean check` green (race numbers will move: bearing friction now brakes the wheels;
  no hand-made expected values)
- card frontmatter: status implemented, branch name
