---
id: 045
status: implemented
branch: card/045
depends: [008]
---
# vehicle: engine block dyno pointer (e+0xc30) + torque curves from DynoData (part of 0x4aebf0)

## Goal
`CarData` already holds the two engine torque curves the Chassis natives read
(`torque_rpm/torque_nm`, `nitro_rpm/nitro_nm`, used by `Chassis.getTorque`).
Nothing fills them yet. Add the engine block's dyno pointer (`e+0xc30`) and
`e+0x14` (rpm of max torque), and port the curve-building block of
`Vehicle__refreshParameters` (0x4aebf0) as one function. Card 046 calls it from
`Chassis.forceUpdate`.

## Exe evidence (cleaned pseudocode, renamed fields)
`e` = car engine block, `d` = `e+0xc30` = `EngineModel*` (card 005 struct:
`step` +0x1d0, `count` +0x1d4, `rpm_max_torque` +0x1c0, `torque_table[0]` +0x1dc,
`torque_table[1]` +0x1e0; an empty vector = the exe's null pointer).
`e+0/+4` = (count, pairs) of the normal curve = our `torque_rpm/torque_nm`;
`e+8/+0xc` = nitro curve = our `nitro_rpm/nitro_nm`.

### 0x4aebf0 Vehicle__refreshParameters — curve block (runs after script `updatevariables`)
    d = e->dyno;                                   // e+0xc30
    if (d == null) {
        e->rpm_max_torque = 0;                     // e+0x14
        normal = nitro = [(0, 0)];                 // count 1, one zero pair
    } else {
        step = d->step; n = d->count;
        if (n != e->count) { reallocate both curves to n pairs; e->count = e->nitro_count = n; }
        e->rpm_max_torque = d->rpm_max_torque;   // e+0x14 = d+0x1c0 (float)
        if (d->torque_table[0] is null) {
            normal = nitro = [(0, 0)];             // count 1; e+0x14 keeps d's value
        } else {
            for (i = 0; i < n; i++) {
                rpm = (float)i * (float)step;
                normal[i] = (rpm, d->torque_table[0][i]);
                nitro[i]  = (rpm, d->torque_table[1] non-null ? d->torque_table[1][i] : d->torque_table[0][i]);
            }
        }
    }
Note: when `n` changed but `torque_table[0]` is null the exe still ends at count 1.
`n ≤ 0` with a non-null table leaves `n` pairs (= none); keep that (vector size `max(n,0)`).

### Who sets e+0xc30 (for reference; card 046 resolves it)
`Vehicle__onPartAttached 0x4942b0`: slot 401 empty → the car script's `dynodata` field;
slot 401 holds the attached part → that part's `dynodata`; stored as the DynoData
native handle (our `game::dyno_data(Object*)` from card 008).

## Changes
1. `src/game/car.h`, `struct CarData`, in the "Engine block e" group:
   ```cpp
   EngineModel *dyno = nullptr;   // e+0xc30 DynoData native (owned by the DynoData object, card 008)
   float rpm_max_torque = 0.0f;   // e+0x14 (from dyno +0x1c0)
   ```
   and after `CarData *car_data(...)`:
   ```cpp
   // Curve block of Vehicle__refreshParameters 0x4aebf0 (dyno -> e+0..0xc, e+0x14).
   void refresh_engine_curve(CarData &c);
   ```
   (`struct EngineModel;` is already forward-declared by card 008.)
2. `src/game/car.cpp` (`vehicle/dyno.h` is already included by card 008): implement
   `refresh_engine_curve` exactly as above with `// 0x4aebf0 Vehicle__refreshParameters (curve block)`.
   The four vectors always end with equal sizes per curve (`torque_rpm.size() == torque_nm.size()`).
   Use `(float)i * (float)c.dyno->step` for the rpm.

## Files
- edit: src/game/car.h, src/game/car.cpp
- read only: src/vehicle/dyno.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
