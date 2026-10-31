---
id: 258
status: merged
branch: card/258
review: claude
depends: [257, 260]
---
# traffic: Traffic__flushSpawns 0x417c90 + TrafficCar__materialize 0x40d4e0 (decision, pose, params)

## Goal
Drain card 257's spawn queue once per frame like type_ground vf13 0x4bac10 does right after
`Traffic__step`: score, sort, try each entry with materialize, keep the budget-blocked ones.
materialize ports the drop/keep decision, the parent choice, the instance pose and the
createScripted params string; the instance itself is made by a caller-supplied creator
(INVENTED bridge; the Runtime side is card 261, so runtime.cpp passes a creator that returns 0).
Notes: `notes/42-traffic-410680-services.md` §3, §9, §11, §13.

## Exe evidence (car fields as in src/game/traffic.h `struct Car`; queue = `Graph::spawn_queue`)
### 0x417c90 Traffic__flushSpawns(ECX = T, int budget, const float *cam)
```
if (count == 0) return;
if (DAT_005fe3bc /*load_priority*/) budget = -1;
for (i = 0; i < count; i++)                                  // pass 1: score
    if (0x40e6e0 valid(&q[i]))
        q[i].dist = cam ? -((p.x-cam[0])^2 + (p.y-cam[1])^2 + (p.z-cam[2])^2) : 0.0f
          where p = 0x40dde0(car.path ref, car.pos(+0x88), car.lateral(+0xa8))
                  // src: car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p)
0x40c270(q, count)                                           // src slot_sort(spawn_queue.data(), n)
kept = 0;
for (i = 0; i < count; i++) {                                // pass 2
    if (!valid(&q[i])) break;                                // stale tail is dropped (count = kept)
    if (!0x40d4e0 materialize(q[i].car, &budget)) { if (kept != i) q[kept] = q[i]; kept++; }
    else { q[i].serial = 0; q[i].car = 0; q[i].flag = 0; }  // dist (+0xc) left as is
}
count = kept;                                                // src: spawn_queue.resize(kept)
```
`0x40e6e0 WeakCarRef__valid(e)`: `e.car && e.serial == cars[e.car-1].serial` -> true; else if
`e.car` set `serial = car = 0, flag = 0`; return false. (Same test as the leader slots,
traffic.cpp `slot.serial != g.cars[slot.car - 1].serial`.)

### 0x40d4e0 TrafficCar__materialize(ECX = car, int *budget) -> 1 = drop entry, 0 = keep (asm-checked)
```
if (!car.live || !car.type_id || car.inst_id) return 1;     // +0xdc, +0x14c, +0x138
parent = dir(+0xcc) == -1 ? road(lane.road).object         // [[car+0xc4]+0x10]+0x74 = road +0x68 link obj
                          : cross.object;                   // [car+0xc4]+0x50 = cross +0x44 link obj
if (parent not bound) return 1;                             // link+8 == 0; src: object == 0
p = { car.front_x(+0x98), car.world[1](+0x90), car.front_z(+0x9c) };
if (!car_ref_height(car.path, car.dir, p.x, p.z, &p.y))     // 0x40da20, card 260
    car_ref_height(car.prev_path, car.prev_dir, p.x, p.z, &p.y);   // +0xd0 ref, result ignored
d = p - car.world;                                          // +0x8c/+0x90/+0x94
f = d; n = |f|; if (n != 0.0f) f *= 1.0f / n;               // 0x40ef70
s = (f.z, 0.0f, -f.x); n = sqrtf(s.x*s.x + s.z*s.z);
if (n != 0.0f) { inv = 1.0f / n; s.x *= inv; s.y = 0.0f * inv; s.z *= inv; }
u = { f.y*s.z - s.y*f.z, s.x*f.z - s.z*f.x, s.y*f.x - f.y*s.x };
Mat34 M: column 0 = s, column 1 = u, column 2 = f;          // 0x567250: m[i]=v[0], m[4+i]=v[1], m[8+i]=v[2]
h = car.k18 * 0.5f;                                         // +0x18, 0x593590 = 0x3f000000
pos = car.world + f * h;
game::mat_to_ypr(M, ypr);                                   // 0x569970, game/car.h
if (0x418f60 takeParked(type_id, &car+0x130)) { ... }       // park map = card 259
else {
    if (*budget == 0) return 0;                             // keep
    (*budget)--;
    params = sprintf("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d", pos.x, pos.y, pos.z, ypr[0], ypr[1], ypr[2], (int32_t)car.seed);
    inst = 0x523c80 createScripted(parent, type link +0x144, params, "traffic_car");
    car.inst_id = inst;                                     // 0x40cac0 ResRefLink__set(&car+0x130, obj)
    getInfo(&car+0x130, 0x38, &car);                        // type_car take-over, card 261
}
if (car.notify) queue car_add (T+0x3130 sink);              // not modelled in src
return 1;
```
Caller 0x4bac10: `0x417c90(T, 1, *0x5f1e44 > 0 ? &0x5fe110 : null)`. `0x5fe110` is a main-loop
copy of the first camera snapshot (notes/42 §13); src has no camera snapshots (boot.cpp:222
uses the `<= 0` branch), so pass `cam = nullptr`.

## Implementation notes
- `void Graph::flush_spawns(int32_t budget, const float *cam, bool load_priority, const Creator &create)`
  with `using Creator = std::function<uint32_t(const Car &car, uint32_t parent, const std::string &params)>`
  (INVENTED bridge; returns the new instance id, 0 = none). materialize is a file-local helper.
- Parent lookup: `dir == -1`: `Road *r = road_by_node(g, g.lanes[car.path_lane].road)`,
  parent = `r ? r->object : 0` (same lookup as the 0x410680 call at traffic.cpp `car_sync_instance(g, ci, road ? road->object : 0)`).
  `dir != -1`: `cross_by_node(g, car.cross_node, nullptr)->object`.
  INVENTED: `dir != -1` with `car.cross_node == 0` (lane owner, see traffic.cpp comment at
  `car_live_step`) or a missing node -> parent 0 -> return 1; the exe would read lane `+0x50`
  (Path end[0] float bits) as an object pointer.
- takeParked: `// shortcut: no park map (Traffic__takeParked 0x418f60, card 259); always the create branch.`
- creator returns 0 -> `car.inst_id = 0`, still return 1 (the exe never checks createScripted).
- getInfo 0x38 / car_add: `shortcut:` comments (card 261 / sink not modelled).
- runtime.cpp, right after `inst->traffic->step(process_world().time);` (the 0x4bac10 comment):
  `inst->traffic->flush_spawns(1, nullptr, vm() && natives::load_priority(*vm()) != 0, creator)` with
  a creator lambda that returns 0 and a `// shortcut: instance creation is card 261` comment.
  (`natives::load_priority` comes from card 260.)

## Files
- edit: src/game/traffic.cpp (flush_spawns, materialize), src/game/traffic.h (Graph::flush_spawns, Creator),
  src/game/runtime.cpp (0x4bac10 call site)
- read only: src/game/car.h (mat_to_ypr), src/engine/math.h (Mat34)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:` / `shortcut:`
- `make -C src check` green (watch the `--city` check: the queue now drains); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
