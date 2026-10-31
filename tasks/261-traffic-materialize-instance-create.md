---
id: 261
status: draft
branch:
review:
depends: [258]
---
# traffic: materialize creator — createScripted "traffic_car" + type_car getInfo 0x38/0x3c

## Goal
Replace card 258's creator stub in runtime.cpp with the exe's instance side of
TrafficCar__materialize 0x40d4e0: create the scripted car instance under the road/cross object,
hand it to the traffic car (type_car getInfo 0x38 take-over), and send `car_add`.
Notes: `notes/42-traffic-410680-services.md` §11, `notes/27-traffic-car-pool.md` §3.

## Exe evidence
```
obj = 0x523c80 GameObject__createScripted(&parent /*road +0x68 / cross +0x44 bound object*/,
          &car+0x144 /*type link*/, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d", "traffic_car")
0x40cac0 ResRefLink__set(&car+0x130, obj)       // inst_id = obj+0x80
getInfo(ECX = 0x5fde90, &car+0x130, 0x38, &car)  // 0x40d8fd: type_car vf15 0x4a15c0 case 0x38
if (car.notify) Game__queueEvent(T+0x3130, 0, 0x10, "car_add %d %d" (inst_id, car), 0)
```
type_car vf15 0x4a15c0 case 0x38 (notes/27 §3): take over = 0x4ae5a0, zero wheel states,
unregister callbacks 8/9 (kinematic, driven by the traffic step), T+0x2054 = car.

## Why draft (blocks todo)
- Parent: src `Road::object` / `Cross::object` are map-object **record ids** (runtime.cpp
  `map_node_touched` -> `bind_object(m.params, m.record)`); src creates no road/cross instance
  (type_road vf3 0x4d42a0 only binds), so there is no Resource to parent under. Needs an
  INVENTED choice (e.g. resolve the record, or the ground instance) — orchestrator decision.
- type_car getInfo 0x38 take-over is not ported (notes/27 §3): without it a materialized car is
  a free physics car the traffic step never drives (notes/27: "porting it alone would freeze a
  car that nothing moves" — the converse holds for porting the create alone).
- The T+0x3130 car_add sink (City) is not modelled anywhere in src.

## Files (when promoted)
- edit: src/game/runtime.cpp (creator lambda at the 0x4bac10 call site), src/game/car.cpp (getInfo 0x38 take-over)
