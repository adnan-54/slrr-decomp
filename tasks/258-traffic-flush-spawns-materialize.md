---
id: 258
status: draft
branch:
review:
depends: [257]
---
# traffic: Traffic__flushSpawns 0x417c90 + TrafficCar__materialize 0x40d4e0 pre-checks

## Goal
Drain card 257's spawn queue once per frame like type_ground vf13 0x4bac10 does after
`Traffic__step`. Evidence: `notes/42-traffic-410680-services.md` §3, §9.

## Exe evidence
### 0x417c90 Traffic__flushSpawns(ECX = T, budget, cam)
```
if (!count) return; if (DAT_005fe3bc /*src load_priority*/) budget = -1;
for e: if (0x40e6e0 valid(e)) e.dist = cam ? -(|0x40dde0(e.car, car.pos(+0x88), car.lateral(+0xa8)) - cam|^2) : 0.0
0x40c270(queue, count)                    // src slot_sort, traffic.cpp:1762
kept = 0
for i in 0..count: if (!valid(e[i])) break;
    if (!0x40d4e0(e[i].car, &budget)) { if (kept != i) e[kept] = e[i]; kept++ } else e[i] = {0,0,0}
count = kept
```
`0x40e6e0`: `e.car && e.serial == car.serial` -> true; else if `e.car` clear serial/car/flag.
Caller 0x4bac10: `0x417c90(T, 1, *0x5f1e44 > 0 ? &0x5fe110 : null)`.
### 0x40d4e0 materialize (returns 1 = drop entry, 0 = keep)
```
if (!car.live || !car.type_id || car.inst_id) return 1
parent = car.dir(+0xcc) == -1 ? lane(+0xc4).road(+0x10)+0x74 : cross(+0xc4)+0x50   // ResRefLink
if (!parent bound) return 1
... pose from 0x40da20/0x40ef70 ...
if (!0x418f60 takeParked(type, &car+0x130)) { if (budget == 0) return 0; budget--; createScripted(type, "%.3f x6,%d", "traffic_car"); getInfo(inst, 0x38, &car) }
else { getInfo 0x38; matrix "ffffff"; 0x4ec210 reparent under parent; getInfo(inst, 0x3c, seed) }
if (car.notify) car_add; return 1
```

## UNRESOLVED (blocks todo)
- writer of road `+0x74` and cross `+0x50` (the materialize parent link); 0x41a3e0
  Traffic__bindObject only binds `+0x68`/`+0x44`. Without it the drop/keep decision cannot be ported.
- camera position source for `cam` in src (`0x5fe110`, count `0x5f1e44`).
- instance creation needs Runtime access from traffic::Graph and the type_car getInfo 0x38
  take-over (notes/27 §3); `0x40da20`, `0x40ef70`, `0x567250` not traced.

## Files (when promoted)
- edit: src/game/traffic.cpp, src/game/traffic.h, src/game/runtime.cpp (the 0x4bac10 call site)
