---
id: 262
status: todo
branch:
review:
depends: []
---
# car: type_car getInfo 0x38 take-over/release (0x4a15c0 case 0x38)

## Goal
Port type_car vf15 0x4a15c0 case 0x38 (`GII_CAR_TRAFFICPTR`): hand a car instance to a traffic
car (asleep, out of the control/drive loops, moved only by the traffic pose push, card 264) or
release it back to physics. Wire the two script callers addTrafficCar 0x4dd090 and
remTrafficCar 0x4de520. Notes: `notes/42-traffic-410680-services.md` §16, notes/27 §3.

## Exe evidence (asm 0x4a215c..0x4a2512, notes/42 §16)
ECX = M (type model, wheel count `M+0x1fb4`), arg1 = C (the car = src `CarData`), p = &tc.
Car fields: `C+0x2054` traffic car ptr; `C+0x2058` ResRefLink {id +0x2060, obj +0x2064};
`C+0x58` = 7th params int (`Part__initInstance` 0x4c1f90 `Str__scan(params,"%f,%f,%f,%f,%f,%f,%d",
.., P+0x58)`; src parses it as `tail` in car.cpp ~1521 and drops it). tc fields: `+0xb0` speed
(src `Car::speed`), `+0x144` type link {id +0x14c = `Car::type_id`, obj +0x150}, `+0x154` `Car::seed`.
```
if (!p) return C+0x2054;
old = C+0x2054; if (old == *p) return old;
if (*p == 0) {                                                   // release
    0x4e8860(ECX = C, 0x100): C node(+0xc)+0x84 &= ~0x100        // src Resource::flags (RI+0x84)
    q = {0}; 0x4ea900(ECX = C, &q): if (C+8 && C+0xc) q.set(node+0xfc)   // parent object
    if (q.id) { sprintf(buf, "obs_add %d" (0x59d194), C+8 /*instance id*/);
                0x47d550 Game__queueEvent(ECX = 0x5fde90, &q, 0, 0x10, buf, 0) }
    0x40cac0(ECX = &C+0x2058, old+0x150)      // C+0x2058 := old tc type object
    C+0x58 = old+0x154                        // seed
    C+0x1e7c = (C+0x2050)->+0xf0              // engine omega = idle omega
    0x47d670 Game__registerCallback(C, 8); (C, 9)
    C+0x2054 = 0; return 0
}
if (old == 0) {                                                  // take over
    0x4ae5a0 PhysBody__sleepRoot(ECX = C+0x13ec)   // if root awake: PhysBody__sleep 0x506d90(root)
    B = C+0x13ec (part_comp = src chassis_body); v = -tc+0xb0
    vel = R(B active state) * (0, 0, v)            // vel_i = R[i][2] * v (R[i][0..1] * 0.0 added)
    R = root(B) (0x480700): both states: vel(+0x70) = vel, angvel(+0xd4) = (0,0,0)
    0x4ebdd0(ECX = C, 0x100): node+0x84 |= 0x100
    C+0x1e00 = 0 (horn); C+0x1e7c = (C+0x2050)->+0xf0
    for i < M+0x1fb4: W = C+0x1414 + i*0x2e4: if (W+0x19c) stop/unlink handle W+0x194 (0x4ec720),
                      W+0x1a4 = 0; if (W+0x1bc) stop/unlink handle W+0x1b4 (0x4ec790), W+0x1c4 = 0
    0x47e390 Game__unregisterCallback(C, 8); (C, 9)
    t = *p; if (C+0x2060) { t.typeLink(+0x144).set(C+0x2064); t+0x154 = C+0x58 }
    C+0x2054 = t; return t
}
if (C+0x2060) { (*p).typeLink.set(C+0x2064); (*p)+0x154 = C+0x58 }   // swap old != 0, new != 0
C+0x2054 = *p; return *p
```
Callers:
- 0x4dd090 addTrafficCar: `tc+0x130.set(inst)` then `getInfo(inst, 0x38, &tc)`, then notify,
  rand fields and `tc+0x154 = 0` (already ported, natives.cpp `gr_add_traffic_car`).
- 0x4de520 remTrafficCar: `if (car+0x138) getInfo(&car+0x130, 0x38, &zero); TrafficCar__remove(car)`.
- Readers that test `C+0x2054` (notes/09 §354, car.cpp comments): Vehicle__update 0x49c8e0 and
  updateControls 0x498300 return early; type_car__vf13 HUD 0x494910 skips; 0x49beb0 dtor posts
  `"car_rem %d %d"` with the traffic car as second `%d`.

## Design
- car.h `CarData`: `uint32_t traffic = 0;` (+0x2054; INVENTED: the Graph car handle = pool index+1,
  as addTrafficCar returns, instead of the exe pointer), `traffic::Graph *traffic_graph = nullptr;`
  (INVENTED: the handle's owner), `uint32_t traffic_type = 0;` (+0x2058 link id), `int32_t k58 = 0;`
  (+0x58 params int). Store `tail` into `k58` at the create parse (car.cpp ~1521).
- car.cpp/.h: `uint32_t car_traffic_ptr(Runtime &rt, Runtime::Resource *r, traffic::Graph *g,
  uint32_t handle)` = the pseudocode (`handle == 0` = release). tc = `g->cars[handle-1]`, old tc =
  `c->traffic_graph->cars[c->traffic-1]` (INVENTED bounds guards). Mapping:
  - sleep: `if (c->root_body && !c->root_body->asleep) process_world().sleep(c->root_body)` (as
    car.cpp "suspend", 0x4a3fdf).
  - vel: `quat_to_mat(&m, &c->chassis_body->rot)` (engine/math.h), `root->vel = Vec3{m.m[2], m.m[6], m.m[10]} * v` (column 2),
    `root->angular_momentum = 0`. shortcut: src bodies keep one state, the exe writes both.
  - node flag: `r->flags |= 0x100` / `&= ~0x100`.
  - horn `c->horn_id = 0`; omega `c->rpm_raw = c->idle_omega` and `c->sim->engine.omega` the same
    (src keeps +0x1e7c in the sim, see car.cpp "reset").
  - obs_add: `if ((r->id >> 16) != 0xffff && r->parent)` pattern of car.cpp ~1780 (0x49beb0):
    `rt.queue_event(r->parent, nullptr, 0x10, Value::make_str("obs_add <id>"))`.
  - type/seed swap with `traffic_type` / `k58` against `tc.type_id` / `tc.seed`.
  - callbacks 8/9: src runs control/drive over `g_cars`, not the mode-8/9 lists; add
    `if (c->traffic) continue;` at the top of `control_cars` (replace the `// shortcut: the traffic AI
    pointer` line) and of `drive_cars` (before the asleep branch), citing 0x47e390/0x47d670.
    shortcut: re-registration appends at the list tail (order change) - not modelled.
  - wheel handles W+0x194/+0x1b4: `// shortcut:` src has no wheel effect handles.
  - hud_cars (~3955): skip a car with `traffic != 0` and update the comment; 0x49beb0 car_rem
    (~1783): second `%d` = `c->traffic`.
- natives.cpp: `gr_add_traffic_car` replaces the 0x38 shortcut with
  `game::car_traffic_ptr(*rt, inst, t, handle)` (before `car.notify`, exe order);
  `gr_rem_traffic_car`: before `remove_car`, if the car's `inst_id` resolves, call
  `car_traffic_ptr(*rt, rt->resolve(inst_id), t, 0)` (INVENTED: handle bounds check as now).

## Files
- edit: src/game/car.cpp, src/game/car.h, src/script/natives.cpp

## Done when
- `// 0x4a15c0 case 0x38` / `// 0x4ae5a0` / caller address comments; handle-for-pointer and graph
  pointer are `INVENTED:`; the not-modelled pieces are `// shortcut:`
- `make -C src check` green (incl. `--city`); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
