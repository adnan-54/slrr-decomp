---
id: 227
status: implemented
branch: card/227
depends: [226]
---
# Vehicle: end-of-tick telemetry, general wear, Part__syncWear, mileage (Vehicle::update §8)

## Goal
Port the tail of `Vehicle__update` 0x49c8e0 (0x4a089a..0x4a0bf3) and `Part__syncWear` 0x497520
(was FUN_00497520).
Today `Chassis.getMileage` never moves, the car's own wear never grows, and the car-side wear values
(engine/clutch/... from cards 225/226) are never written back into the installed parts, so a
`forceUpdate` (0x4a8060 reads P+0xb8) forgets them. Evidence: notes/34 §7.

## Exe evidence (cleaned)
K = Config.internal_damage (same read as card 225; reuse its helper). clamp01 as in card 226.
### 0x4a089a..0x4a0bf3, last block of Vehicle::update (runs when dt > 1e-5)
```
v = root body linear velocity (car_root state vel)
speed = sqrt(v.x*v.x + v.y*v.y + v.z*v.z) * 0.2 + speed * 0.8              // car+0x1fd8
(car+0x1fc0..0x1fd4 acceleration/angular telemetry: not part of this card)
d = speed * dt
odo_engine += |omega_e| * dt          // car+0x1e90, omega_e = car+0x1e7c (src rpm_raw)
odo_shaft  += |shaft_speed| * dt      // car+0x1e94
odo_dist   += d                       // car+0x1e98
if (!(flags & 0x400) && (speed > 0.01 || |omega_e| > 0.01 || |shaft_speed| > 0.01)) {
    x = d * K
    if (x > 0) car_part.wear = clamp01(car_part.max_wear_inv * x + car_part.wear)   // car+0xb8 / +0xbc:
                                         // the car is a part too: its own PartData (part_data(rt, car resource))
    for each installed part (exe: car+0xdc list; src: c->part_list, INVENTED like card 222):
        Part__syncWear(part, car, dt)
    mileage += speed * dt                // car+0x2250 (recomputes speed*dt; same value)
}
```
### 0x497520 Part__syncWear(obj, P, M, car, dt)
```
if (P.wear >= 1) return
k = P.dmg_mode & 0xffff ; w = (int)(P.dmg_mode >> 16)
switch (k) {
  case 2:   wear_add(P, |omega_e| * K * dt)
  case 3:   wear_add(P, speed * K * dt)                       // car+0x1fd8, no abs
  case 4:   wear_add(P, |shaft_speed| * K * dt)               // car+0x1e78
  case 0xe: wear_add(P, |combustion_torque * omega_e| * K * dt)   // car+0x1e8c * car+0x1e7c
  case 5:   if (w < wheel count) P.wear = wheels[w].tyre_wear    // W+0x15c
  case 6:   if (w < wheel count) P.wear = wheels[w].brake_wear   // W+0x164
  case 7:   if (w < wheel count) P.wear = wheels[w].susp_damage  // W+0x16c
  case 8:   P.wear = engine_damage    case 9:  P.wear = trans_wear
  case 10:  P.wear = clutch_wear      case 12: P.wear = radiator_damage
  default:  nothing
}
if (P.wear < 0) P.wear = 0                     // exe logs "Part <name> has invalid wear!"
if (P.wear >= 1 && part has a script instance) call script method "updateDamage" (no args)  // Part.java:706
// wear_add(P, a) = 0x4afb50 on {P.wear, P.max_wear_inv}: if a <= 0 nothing; P.wear += P.max_wear_inv*a;
//   < 0 -> 0; >= 1 -> 1. Card 225 has the helper; make it reachable from car.cpp (vehicle.h) or
//   write the same 6 lines next to Part__syncWear, not a third variant with different rounding.
```
`w < wheel count` is a signed compare (dmg_mode >> 16 is 0..0xffff; 0xffff never passes).

## Where in src
- car.h `CarData`: add `speed_smoothed` (+0x1fd8), `odo_engine` (+0x1e90), `odo_shaft` (+0x1e94),
  `odo_dist` (+0x1e98), `combustion_torque` (+0x1e8c), all 0 at create (0x4992e0 zeroes them).
- car.cpp `drive_cars`: after `sim.update(dt)` and the existing copy-back (rpm_raw, shaft_speed, ...),
  copy `c->combustion_torque = sim.combustion_torque`, then run the block above. Runtime: use the
  file's `g_rt` (as `part_data` does); script instance: `rt.script_instance(P)` + `rt.call_method(obj,
  "updateDamage", {}, &rv, &err)` like the `install` call in `part_attach`.
- New static function `part_sync_wear(Runtime&, CarData&, Runtime::Resource*, float dt, float K)`
  with the `// 0x497520 Part__syncWear` comment.

## Files
- edit: src/game/car.h, src/game/car.cpp
- read only: src/vehicle/vehicle.h (sim outputs, wear_add helper from card 225)

## Done when
- `// 0xADDR` comments; part_list stand-in and the per-call K read are `INVENTED:`
- `make -C src clean check` green (no hand-made expected values; `Chassis.getMileage` now grows during
  the vmboot race, which is fine to print but not to assert against a made-up number)
- card frontmatter: status implemented, branch name
