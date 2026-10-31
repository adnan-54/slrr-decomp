---
id: 259
status: implemented
branch: card/259
review:
depends: [258]
---
# traffic: CarContainer T+0x311c + instance reparent hook (0x416000, 0x410680, 0x40ed30/0x40ebb0, 0x40fae0)

## Goal
Give traffic::Graph a way to reparent real instances, and use it at the four exe call sites of
0x4ec210 for type-0 (script) cars: under the bound road/cross object when the car is live and the
owner link is bound, else under the CarContainer. Create the CarContainer like the Traffic ctor.
Also port the instance transfer of 0x40fae0's type-0 branch (the replacement car takes the old
car's instance). Park (0x417080), the type_car getInfo 0x38 take-over and car_add/car_rem stay
shortcuts (cards 263, 262; the T+0x3130 sink is not modelled anywhere).
Notes: `notes/42-traffic-410680-services.md` §1, §5, §7, §8, §15.

## Design (INVENTED bridge, notes/42 §15)
- `traffic.h` `struct Graph`: add
  `uint32_t car_container = 0;   // +0x311c CarContainer instance id (0x416000)` and
  `std::function<void(uint32_t inst, uint32_t parent)> reparent;  // INVENTED bridge for 0x4ec210`.
- `runtime.cpp` `load_ground`, right after `g->init(...)` (the exe creates it in the ctor 0x416000,
  before the load; src has no Graph before the load, so INVENTED placement):
  ```
  // 0x416000: createScripted(game root 0x5fde50, null type, "" (0x59380d), "CarContainer")
  Resource *cc = rt.instantiate(nullptr, rt.game_root(), "CarContainer", "", true, nullptr);
  if (cc) { cc->flags |= 0x10; cc->world_root = cc; }   // +0x84 |= 0x10; 0x51f7f0: obj+0xf0 link = obj
  g->car_container = cc ? cc->id : 0;
  g->reparent = [&rt, r, cc](uint32_t inst, uint32_t parent) {
      Resource *i = rt.resolve(inst);
      // INVENTED: road/cross objects are map record ids in src (no map node instance,
      // 0x5195a0 not ported); the ground instance r stands in for them.
      Resource *p = cc && parent == cc->id ? cc : parent ? r : nullptr;
      if (i && p) rt.reparent(i, p);           // 0x4ec210 GameRef.setParent body
  };
  ```
  (`r` = the ground Resource that owns the Graph; check `Runtime::reparent` returns early when the
  parent is unchanged, like 0x4ec210's "already under that parent" return.)
- `traffic.cpp`: a tiny helper `static void link_reparent(Graph &g, uint32_t inst, uint32_t parent)`
  that calls `g.reparent` when set (`if (inst && g.reparent) g.reparent(inst, parent)`).

## Exe evidence
### 0x416000 Traffic__ctor tail (asm 0x416223..0x416270)
```
obj = 0x523c80 GameObject__createScripted(&0x5fde50, &null_link, "" /*0x59380d = NUL byte*/, "CarContainer")
0x40cac0 ResRefLink__set(ECX = &T+0x311c, obj)
if (obj) { obj+0x84 |= 0x10; 0x51f7f0(ECX = obj, &T+0x311c) }   // 0x51f7f0: 0x40cac0(ECX = obj+0xf0, link+0xc)
0x417350 Traffic__reset(ECX = T)                                  // not this card
```
`obj+0xf0` link object word `+0xfc` = world-tree root (src `world_root`, getWTRoot 0x4e3c00);
GameRef.setFlags 0x4db900 does the same pair for bit 0x10.

### 0x410680 TrafficCar__syncInstance (src `car_sync_instance`, traffic.cpp ~1215)
Both reparent calls have ECX = `&car+0x130` (instance link), arg = parent link:
```
if (car.type_id == 0) {
    if (!inst) return;
    if (!car.live || !bound) { 0x4ec210(&car+0x130, &T+0x311c); return }   // 0x4106b8 -> link_reparent(g, inst, g.car_container)
    0x4ec210(&car+0x130, link); return                                      // 0x410701 -> link_reparent(g, inst, link_object)
}
...
reparent_link: 0x4ec210(&car+0x130, link)       // 0x410707, typed car with inst, live, bound -> link_reparent(g, inst, link_object)
```
Replace the three `// shortcut: 0x4ec210 link_reparent ... not ported.` comments in
`car_sync_instance` with the calls.

### 0x40ed30 / 0x40ebb0 inlined per-car body (src `car_bind_object`, traffic.cpp ~2526)
Same branches (notes/27 §4):
```
if (tc+0x14c == 0) { if (id) bind(tc+0xdc && sink+8 ? sink : T+0x311c) }
...
} else bind(sink)          // typed, live, bound, has inst
```
-> type 0: `link_reparent(g, inst, car.live && object ? object : g.car_container)`;
typed live bound with inst: `link_reparent(g, inst, object)`. Replace the two shortcut comments.

### 0x40fae0 halt recycle, type-0 branch (src `recycle_halted_car`, traffic.cpp ~3335), asm-checked
```
new = 0x416800 spawnCarNear(ECX = T, cross pos, 100.0f (0x42c80000), car+0x14, +0x18, +0x20, +0x24)
0x40cac0 ResRefLink__set(ECX = &new+0x130, old+0x13c)     // 0x40fbc5: new.inst_id = old.inst_id
inline 0x488080 on old+0x130; old+0x138 = 0                // 0x40fbca..0x40fc1f
new+0x2c/+0x30/+0xa8/+0x154/+0xdd = old's                  // already ported
getInfo(ECX = 0x5fde90, &new+0x130, 0x38, &new)            // 0x40fc79: shortcut, card 262
0x410680(ECX = new, new.lane(+0xc4).road(+0x10)+0x68)      // 0x40fc8e
if (new+0xdd && new+0xdc) queue(T+0x3130, 0, 0x10, "car_add %d %d" (new+0x138, cross), 0)  // shortcut (sink)
TrafficCar__remove(old)
```
In src: inside `if (nc)`, `n.inst_id = car.inst_id;` then (after the copies) call
`car_sync_instance(g, nc - 1, <object of n's lane's road>)` — use the same road-object lookup
`car_sync_instance` callers use (grep `car_sync_instance(` in traffic.cpp; e.g. `refresh`/0x4107a0
style `road_by_node(g, g.lanes[n.path_lane].road)->object`, with an INVENTED null guard if the
lane/road is missing). Keep the existing INVENTED failed-spawn behaviour; the typed branch is
unchanged (park = card 263).

## Files
- edit: src/game/traffic.h (Graph fields), src/game/traffic.cpp (helper + 3 functions),
  src/game/runtime.cpp (load_ground)

## Done when
- each ported call has a `// 0xADDR` comment; the parent stand-in and the bridge are `INVENTED:`
- remaining shortcuts name their card (park 263, getInfo 0x38 take-over 262, T+0x3130 sink)
- later instance ids shift by one (the CarContainer takes one; the exe creates it too)
- `make -C src check` green (incl. `--city`); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
