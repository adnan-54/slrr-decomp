---
id: 196
status: implemented
branch: card/196
depends: []
---
# car HUD: osd_* type cfg gauges + Chassis.getOSD/setOSD/getAngvel natives

## Goal
Port the car type record's 17 HUD gauge records (M+0x1a44 + i*0x48) with their ctor defaults
and the 14 `osd_*` type-cfg keywords, and implement the stub natives `Chassis.getOSD`,
`Chassis.setOSD` (Track.java:1951-1989 debug OSD) and `Chassis.getAngvel`. No rendering here
(card 197 computes the per-frame values). Full RE: notes/21-hud-osd.md.

## Exe evidence
### Gauge record G (0x48 bytes) and member ctor 0x4909b0
```
struct OsdGauge {            // offsets inside G
  float pos[3];              // +0x00
  float ori[3];              // +0x0c (Ypr)
  int32 kind;                // +0x18  0 none, 1 pin, 2 text, 5 lamp
  int32 ref_id;              // +0x1c ResourceRef (0x10); its id at +0x24 is the "set" test
  int32 color;               // +0x2c
  char  fmt[9];              // +0x30 (8 chars + NUL)
  float offset, scale, min, max;   // +0x38 +0x3c +0x40 +0x44
};
0x4909b0 for every G: ref 0, kind 0, fmt "", offset 0, scale 1.0, min 0, max 0 (pos/ori not written: 0)
```
### Defaults from type vf1 0x49ab20 (0x49ab4a..0x49b27e), index i = (G - 0x1a44) / 0x48
```
i  G       name        kind pos                    ori   off/scale/min/max            ref(rpk,id)       color      fmt
0  0x1a44  cockpit_rpm  1   -0.4 0.1 -0.7          0     2.2 -0.0005 -2.2 2.2         -
1  0x1a8c  cockpit_spd  1   -0.3 0.1 -0.7          0     2.2 -0.02 -2.2 2.2           -
2  0x1ad4  rpmpin       1   1.093 -0.866 1.1001    0     2.41 -0.000535 -2.414 2.41   cars.rpk 0xd227
3  0x1b1c  spdpin       1   1.657 -0.44 1.1001     0     2.3574 -0.01305 -2.353 2.3574 cars.rpk 0xd226
4  0x1b64  nospin       2   0.73 -0.85 -0.0001     0     2.5 -0.02 -2.2 2.2           -       (kind 2 is what 0x49ad90 writes)
5  0x1bac  damage       0   0.926 0.855 -0.0001    0     0 1 0 0                      cars.rpk 0x57
6  0x1bf4  gauge        0   1.41 -0.53 1.1         0     0 1 0 0                      cars.rpk 0xd225
7  0x1c3c  rpm          2   0.72 0.755 0           0     0 1 0 99999                  -                 0xff000000 "%4.0f"
8  0x1c84  speed        2   0.76 0.57 0.0001       0     0 1 0 999                    cars.rpk 0x58     0xff000000 "%03.0f"
9  0x1ccc  gear         2   0.512 0.889 0.0001     0     0 1 0 0                      cars.rpk 0x58     0xff000000 "%c"
10 0x1d14  nos          2   0.73 -0.85 -0.0001     0     0 8 0 8                      cars.rpk 0x5c     0xff000000 "%01.0f"
11 0x1d5c  turbo        2   0.73 0.9 -0.0001       0     0 8 0 8                      cars.rpk 0x5c     0xff000000 "%01.0f"
12 0x1da4  mileage      2   0.73 0.73 -0.0001      0     0 1 0 999999                 cars.rpk 0x58     0xff000000 "%07.0f"
13 0x1dec  abs          5   3.24 -2.4 -3.58        0     0 1 0 0                      -                 0xffff0000
14 0x1e34  asr          5   3.24 -2.4 -3.58        0     0 1 0 0                      -                 0xffff0000
15 0x1e7c  hbrake       5   3.24 -2.4 -3.58        0     0 1 0 0                      -                 0xffff0000
16 0x1ec4  emergency    5   3.24 -2.4 -3.58        0     0 1 0 0                      -                 0xffff0000
gear chars M+0x1a38 (char[9]) = "N123456R"   (0x49abd2 writes "0123456R", 0x49ae5a overwrites)
```
Default refs come from `0x5189e0(rpk name, id, 0xe, 0)`.

### Keyword handlers (table 0x5da7b8), all `if (M+0x120 == 0)`; scanner 0x569380 on the values
```
osd_gauge   0x48e770  "%d %f %f %f"                          id, pos              G6
osd_rpmpin  0x48e7f0  "%d %f %f %f %f %f %f %f"              id, pos, off,scale,min,max  G2
osd_spdpin  0x48e890  same                                                         G3
osd_nospin  0x48e930  same                                                         G4
osd_damage  0x48e9d0  "%d %f %f %f"                          id, pos              G5
osd_damage_slot 0x48ea50  "%c" into a stack temp: no effect (accept, ignore)
osd_gear    0x48eaa0  "%d %f %f %f %d %8s"  id, pos, color, gear chars -> M+0x1a38;  ref only if count > 1   G9
osd_speed   0x48eb30  "%d %f %f %f %d %8s"  id, pos, color, fmt                   G8
osd_rpm     0x48ebe0  same                                                         G7
osd_nos     0x48ec90  "%d %f %f %f %d %8s %f %f %f %f"  id, pos, color, fmt, off,scale,min,max  G10
osd_turbo   0x48ed40  same                                                         G11
osd_mileage 0x48edf0  same                                                         G12
osd_abs     0x48eea0  id, pos, color (5 targets)                                   G13
osd_asr     0x48ef20  same G14;  osd_hbrake 0x48efa0 G15;  osd_emergency 0x48f020 G16
after the scan (count > 0, gear count > 1): G.ref = 0x51e860(id) via 0x51fa40(res, 0xe, 0)
```
Missing trailing values keep the previous value (scanf stops).

### 0x4b1fd0 Chassis.getOSD(int id) -> float, M = car type record, car = car instance
```
switch id: 0 M+0x1b54  1 M+0x1b58  2 M+0x1b5c  3 M+0x1b60        // G3 spdpin offset/scale/min/max
  4: |root active-state vel| * (1 / measure_per_hour[0x5fe378]) * M+0x1b58 + M+0x1b54
  5 M+0x1b0c  6 M+0x1b10  7 M+0x1b14  8 M+0x1b18                  // G2 rpmpin offset/scale/min/max
  9: |car+0x1e80 * 9.549296| * M+0x1b10 + M+0x1b0c
other id / no car -> 0
```
### 0x4b4850 Chassis.setOSD(int id, float v): ids 0-3, 5-8 write the same fields; 4, 9, others ignored.
### 0x4b24f0 Chassis.getAngvel() -> Vector3
```
b = car+0x13ec; while (b+0x58 & 4) b = b+0x20      // root
return active-state angular velocity of b (state + 0xd4); no car -> new Vector3 (0,0,0)
```

## src mapping
- `M` fields live on `game::CarData` (as `sh_lo` etc. from the type cfg in `fill_car`).
- `|root active-state vel|` = `c->root_body->vel` length (car_root); 0 when root_body is null.
- `measure_per_hour` = `sched(vm).measure_per_hour` (natives.cpp:162, System.setMeasure 0x4e57b0).
- angular velocity: `RigidBody::angular_velocity()` (physics.h:167) of `c->root_body`.

## Steps
1. car.h: add `struct OsdGauge` (fields above, `int32_t ref_id = 0; const char *ref_pkg =
   nullptr; int32_t ref_local = 0;` for the default (rpk, id) pair) and in CarData
   `OsdGauge osd[17]; char gear_chars[9] = "N123456R";`; a function `void osd_defaults(CarData&)`
   in car.cpp fills the table above, called from the CarData ctor (or wherever the other
   0x49ab20 defaults are set).
2. car.cpp `fill_car`: the 14 keywords. Parse the id and colour with `strtoul(s, 0, 0)` cast to
   int32 (`// INVENTED: 0x569380 integer conversion not traced; strtoul keeps 0xFF000000`).
   `%8s`: copy at most 8 chars. A cfg id sets `ref_id` and clears `ref_pkg`.
3. natives.cpp: `ch_get_osd`, `ch_set_osd`, `ch_get_angvel` using `car_of(a)` like
   `ch_get_shifting` (natives.cpp:3567); replace the three `nullptr, true` rows.

## Files
- edit: src/game/car.h, src/game/car.cpp (fill_car ~line 187), src/script/natives.cpp
- read only: src/physics/physics.h (RigidBody::angular_velocity)

## Done when
- each function has its `// 0xADDR` comment; INVENTED on the integer scan
- `make -C src clean check` green; natives line reports 3 fewer explicit stubs
- no new unit tests; card frontmatter: status implemented, branch name
