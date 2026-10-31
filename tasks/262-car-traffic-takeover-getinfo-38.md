---
id: 262
status: draft
branch:
review:
depends: []
---
# car: type_car getInfo 0x38 take-over/release (0x4a15c0 case 0x38)

## Goal
Port type_car vf15 0x4a15c0 case 0x38 (`getInfo(inst, 0x38, &tc)`): hand a car instance to a
traffic car (kinematic, driven by the traffic step) or release it back to physics. Callers in
the exe: addTrafficCar 0x4dd090, materialize 0x40d4e0 (0x40d798/0x40d8fd), halt recycle 0x40fae0
(0x40fc79), park 0x417080 / reset 0x417350 (owner = pseudo-car T+0x3118), remTrafficCar 0x4de520
(release, &zero). Notes: `notes/27-traffic-car-pool.md` §3, `notes/42` §5, §6, §14.

## Known (notes/27 §3, summary only)
case 0x38 (p = &tc): `M+0x2054` = current traffic car. Same -> return. tc = 0 (release): queue
`obs_add <M.id>`, rebind, `M+0x58 = old tc+0x154`, re-register callbacks 8 and 9. current 0 ->
tc != 0 (take over): 0x4ae5a0, zero wheel states, unregister callbacks 8/9, restore links,
`M+0x2054 = tc`. Returns `M+0x2054`.

## UNRESOLVED (blocks todo; trace before carding)
- case 0x38 body not asm-read: exact order, "rebind"/"restore links" targets, wheel-state fields.
- 0x4ae5a0 untraced.
- Which code writes the kinematic instance pose from the traffic car each frame (the reader of
  `M+0x2054`); without it a taken-over car is frozen.
- src mapping of `M+0x2054` (CarData has no field; car.cpp "traffic AI pointer not modelled").

## Files (when promoted)
- edit: src/game/car.cpp, src/game/car.h, src/script/natives.cpp (addTrafficCar/remTrafficCar shortcuts)
