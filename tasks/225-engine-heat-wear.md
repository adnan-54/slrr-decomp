---
id: 225
status: implemented
branch: card/225
depends: [224]
---
# Vehicle: engine temperature, radiator overheat damage, engine wear (Vehicle::update §1)

## Goal
Port the engine thermal + wear block of `Vehicle__update` 0x49c8e0 (notes/34 §3, notes/09 §1
"Thermal"). Today `engine_temp` stays 20 forever, `getEngineTemp` and the HUD emergency lamp never
react, and engine wear never builds up. After this card: temperature rises with combustion heat and
cools toward ambient, >100 °C damages the radiator, engine wear grows with rpm^16 and lowers
`torque_health`/`nitro_health` (and at 1.0 halves them and multiplies engine friction by 50).

## Exe evidence (cleaned; e = engine block, offsets mapped to src names)
### helpers
```
// 0x4afb50 Vehicle__damageRoll(amount), ECX = &{wear, rate}. Deterministic, NOT random.
int wear_add(float &wear, float rate, float amount):
    if (amount <= 0) return 0
    wear = rate * amount + wear
    if (wear < 0) wear = 0                     // exe also logs "intdamage_data: part with invalid wear detected!"
    if (wear < 1) return 1
    wear = 1; return 2
// 0x4b0ff0 health(w) = w < 1 ? 1 - w*w*0.6 : 0.100000024   -> already in src: health_factor (natives.cpp)
//   and wear_factor (car.cpp); reuse one of them, do not add a third copy.
K = Config.internal_damage (exe global 0x5dacd4, static init 1.0, loaded by 0x47c240 from
    Config "internal_damage"). Read it like runtime.cpp reads Config.gravity:
    vm->get_static_field("java.util.Config", "internal_damage", ...), 1.0 when missing.
    INVENTED: read once per drive_cars call (the exe reloads it only in 0x47c240).
```
### 0x49cfd0..0x49d2a5, inside `if (|rpm| > 0.001)` after combustion_torque, engine running only
`heat = thr * 1000 * T_max` where thr is the throttle after the idle governor and the starter
(vehicle.cpp `thr`), T_max = torque_at(rpm) * torque_health (vehicle.cpp `T_max`). (Nitro adds
`2000*ΔT`; the sim has no nitro yet: leave a `shortcut:` comment.)
```
if (!(car.flags & 0x400)) {
    if (heat * dt > 0) temp = heat*dt / heat_cap + temp                       // 0x1ec0, 0x1ebc
    if (temp <= 90 || radiator_dmg >= 1) c = cool_low                         // e+0xa5c
    else { v = root body linear velocity (state +0x70)
           c = health(radiator_dmg) * ((v.x*v.x + v.y*v.y + v.z*v.z) * cool_speed + cool_air) + cool_low }  // a64, a60
    cooling_coeff = c / heat_cap                                               // 0x1ec8
    if (temp > 100) { d = temp - 100; wear_add(radiator_dmg, radiator_rate, d*d*dt*K) }   // ECX = car+0x1eac (asm 0x49d071)
    if (engine_dmg < 1) {                                                      // 0x1e9c
        fric = rpm < 0 ? friction_rev : friction_fwd                           // e+0xf8 / e+0xf4, signed rpm
        r = |rpm| / max_rpm ; r = r*r ; r = r*r ; r = r*r ; r = r*r            // (|rpm|/maxRPM)^16, four squarings
        t = |temp - 90| * 0.01
        p = ((dt + dt) / engine_rate) * r * fric * 100 * (t*t + 1)            // engine_rate 0x1ea0
        if (p > 0 && wear_add(engine_dmg, engine_rate, K * p) != 0) {          // ECX = car+0x1e9c (asm 0x49d169)
            h = health(engine_dmg)
            torque_health = torque_base * h ; nitro_health = nitro_torque_base * h   // 0x1e04 = e+0xd4*h, 0x1e08 = e+0xd8*h
            if (engine_dmg >= 1) { torque_health *= 0.5 ; nitro_health *= 0.5 ;
                                   friction_fwd *= 50 ; friction_rev *= 50
                                   // event M+0x1f4c (engine blown sound): shortcut, not posted
            }
        }
    }
}
```
### always while the engine runs (after the |rpm| block, before the gear-ratio write)
```
d = temp - ambient                                                             // 0x1ec4
temp = temp - |d| * cooling_coeff * dt * d
```
(Engine off: no thermal code at all; temperature stays.)
(The rate start values, notably engine_rate = 5e-8, come from card 224. engine_rate must never be 0
here: `(dt+dt)/0` makes p inf and the wear NaN -> 1, a blown engine.)

## Where in src
- The sim (`vehicle::Car`, vehicle.h/.cpp) owns the per-tick math: add the fields it needs (temp,
  ambient, heat_cap, cooling_coeff, cool_low/air/speed, engine_dmg/rate, radiator_dmg/rate,
  torque_base, nitro_base, nitro_health, k_dmg, a `wear_off` bool for flags & 0x400) and do the block
  inside `Car::update_engine_and_drivetrain` at the exe's place (heat/wear inside the `|rpm| > 0.001`
  block after `combustion_torque`; cooling after it, engine running only). `engine.torque_health` is
  the value the next tick's T_max uses.
- `drive_cars` (car.cpp, the INVENTED glue block) copies CarData -> sim before `sim.update` and the
  changed values back after it: engine_temp, engine_temp_ambient, heat_capacity, cooling_coeff,
  cooling_low/air/speed, engine_damage/rate, radiator_damage/rate, torque_base, nitro_torque_base,
  torque_health, nitro_health, friction_fwd/rev, flags & 0x400, K. Copy back: engine_temp,
  cooling_coeff, engine_damage, radiator_damage, torque_health, nitro_health, friction_fwd,
  friction_rev.

## Files
- edit: src/vehicle/vehicle.h, src/vehicle/vehicle.cpp, src/game/car.cpp (drive_cars glue)
- read only: src/script/natives.cpp `health_factor`, `ch_force_update` (how torque_health is built)

## Done when
- each ported block has a `// 0xADDR` comment; untraced glue is `INVENTED:`, the missing nitro heat
  term and the unposted event are `shortcut:` comments
- `make -C src clean check` green (race numbers may move; do not add hand-made expected values)
- card frontmatter: status implemented, branch name
