---
id: 218
status: implemented
branch: card/218
depends: [217]
---
# Traffic: Traffic__bindObject + ctRoad/ctCross bindObject (road object links), corpus-checked

## Goal
A map-placed `_type_road` object (gametype 0x15) binds itself to one road or cross of the traffic
graph through its params `"<kind>;<graph>,<index>"` (notes/30 §2, notes/31). Port the bind as
`traffic::Graph::bind_object` with the road/cross object link it stores. resman calls it for every
road object of city.rpk (card 217's walker) and checks that every road and cross of city.trc gets
bound exactly once. **No runtime caller in this card**: the exe binds when the object's map node
becomes resident (physics/render driven, notes/31 §2). That driver is not carded, because it needs the
object box (notes/31 §6.4). Do not add one.

## Exe evidence (asm-checked ECX/link offsets; src names: Graph `g`, node ids as in traffic.h)
### 0x41a3e0 Traffic__bindObject(params, obj) -> ref (ECX = Traffic)
```
if (sscanf(params, "%d;%d,%d", &kind, &graph, &idx) != 3 || graph != T+0xc /*g.version*/) return 0
kind 1: road = roads[idx];       ctRoad__bindObject(road, obj);  return road
kind 2: c = cross_eq[idx];       ctCross__bindObject(c, obj);    return c ? c + 0x10 : 0   // the cross's 2nd vtable
kind 3: c = cross_light[idx];    ctCross__bindObject(c, obj);    return c ? c + 0x10 : 0
else return 0
```
No bounds check on idx in the exe (pool = chunked array).
src: return the node id (road: `E + L + idx + 1`; eq cross: `idx + 1`; light cross: `E + idx + 1`,
E/L = cross_eq/cross_light sizes), 0 = null. `INVENTED:` return 0 for idx outside the pool.
The params parse already exists: `ped::parse_road_params` (src/game/pedestrians.h).

### 0x40ed30 ctRoad__bindObject(obj) (ECX = road)
```
road+0x68 /*ResRefLink*/ .set(obj+0xc /*obj's instance*/)       // asm: lea ecx,[esi+0x68]
for (lane = road+4 /*road.node[0]*/; lane; lane = lane+0x14 /*node14*/)
  for (car = lane+8 /*head*/; car; car = car+8 /*next*/) {
    inst = car+0x138 /*inst_id*/
    if (car+0x14c /*type_id*/ == 0) {
        if (inst) reparent car instance under (car.live && obj linked ? obj : T+0x311c CarContainer)   // 0x4ec210
    } else if (!car.live || obj+8 == 0 /*obj not linked*/) {
        if (inst) {
            if (car.notify /*+0xdd*/) queueEvent(T+0x3130, "car_rem %d %d", inst, car)
            car+0xe8 = T.sim_time; Traffic__parkInstance(type, &car+0x130); unlink car+0x130; car.inst_id = 0
            counters 0x5e64f4--, 0x5e64fc++
        }
    } else if (inst) reparent under obj                           // 0x4ec210
    else { counters 0x5e64f4++, 0x5e64f8++; Traffic__queueSpawn(car) }   // 0x41a500
  }
```
### 0x40ebb0 ctCross__bindObject(obj) (ECX = cross)
```
cross+0x44 /*ResRefLink*/ .set(obj+0xc)                         // asm: lea ecx,[esi+0x44]
for (car = cross+8; car; car = car+8)   same per-car body as above
```
src has no cars on crosses (cars are always on lanes, card 216), so the cross loop has no body in src.

### What src can do in the per-car body today
- `type_id` is never set in src (no typed traffic cars), so only the first branch runs.
- The instance reparent 0x4ec210 is not ported (natives.cpp addTrafficCar has the same
  `shortcut: 0x4ec210 reparent is not ported` comment). Keep the loop and the branch structure, and put
  the reparent, parkInstance, queueSpawn, car_rem event and counters behind the same
  `shortcut:` comments (they are not modelled in src). Do not invent replacements.

## Files
- edit: src/game/traffic.h: `Road::object` (+0x68) and `Cross::object` (+0x44), `uint32_t`, comment
  `INVENTED: id of the bound object's instance, 0 = none (ResRefLink in the exe)`. Declare
  `uint32_t bind_object(const std::string &params, uint32_t object);` in Graph (+ `#include <string>` is there).
- edit: src/game/traffic.cpp: `Graph::bind_object` (0x41a3e0) with the road/cross helpers
  (0x40ed30, 0x40ebb0) as static functions next to the lane helpers.
- edit: src/tools/resman.cpp: in the city.rpk road check (card 217's loop over road objects), call
  `graph.bind_object(params, rec.id)` (`rec.id` stands in for the instance id). Checks:
  every call returns non-zero; afterwards every `graph.roads[i].object`, `cross_eq[i].object`,
  `cross_light[i].object` is non-zero (each pool slot bound once: 484/53/218).
- read only: src/game/pedestrians.h (`parse_road_params`)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`, unported calls `shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
