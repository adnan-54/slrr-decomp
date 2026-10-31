---
id: 244
status: implemented
branch: card/244
depends: [242]
---
# traffic: cross vf3 0x40cb60 + segment posOffset 0x413660 + syncInstance 0x410680

## Goal
Complete the cross hand-over side that card 242 leaves as `// SERVICE MISSING:`:
0x413660 `ctSegment__posOffset` (the `dir >= 0` cross geometry of `car_ref_pos_offset`),
0x40cb60 `ctCross__vf3` (append the car to the cross's car list +8/+0xc, re-reference it
onto its exit lane, then sync its instance) and 0x410680 `TrafficCar__syncInstance`
(instance / spawn-queue / park bookkeeping; the instance side has no src service, so it
stays a branch skeleton with `// shortcut:` marks).

Promotion note (orchestrator): card 242 merged, so a lane-end move can now route a car onto
a cross path (`kc0 >= 0` tagged ref) — and that car has **no world position** until
`0x413660` lands here. Implement all three functions in this card; the syncInstance side
stays a documented skeleton (its callees `0x4ec210`/`0x417080`/`0x488080`/`0x41a500` and
the counter globals `0x5e64f4/f8/fc` are untraced). Replace the stale `0x40cb60`/`0x4883e0`/
`0x410680` `SERVICE MISSING` comments card 242 left in `car_step` step 6, and refresh the
`car_ref_pos_offset` / `car_path_dir` / `car_path_length` comments that still say no car
can be on a cross.

## Exe evidence (cleaned pseudocode, renumbered fields are our names)
Helpers 0x4134e0 `segment_pos` and 0x4131a0 `segment_side` are already ported in
`src/game/traffic.cpp`; reuse them.

### 0x413660 ctSegment__posOffset (ECX = Segment; args `float out[3], float d, float off`)
```
0x4134e0(this = Segment, d, out);        // src segment_pos
0x4131a0(this = Segment, d, side);       // src segment_side
out[0] = side[0] * off + out[0];
out[1] = side[1] * off + out[1];
out[2] = side[2] * off + out[2];
```
Returns `out`; nothing else is written. (The `dir` of the cross ref comes from the
sibling 0x412f60 `segment_dir`, already ported.)

### 0x40cb60 ctCross__vf3 (ECX = Cross; args `Car *car, uint32_t path`) — 98 bytes, RET 8
Intrusive tail append into the cross car list (asm-verified):
```
car+0xc = cross;                      // owner
car+0x4 = cross+0xc;                  // prev = old tail
car+0x8 = 0;                          // next
if (cross+0xc == 0) cross+0x8 = car;  // empty: head = car
else old_tail->next (car+0x8) = car;  // else old tail's next
cross+0xc = car;                      // new tail
b    = cross->paths[path].b;          // byte at cross+0x1c + path*0x60 + 1 (src CrossPath::b)
lane = cross->links[b].node;          // dword at [cross+0x14 + b*8] (src Cross::links[b].node)
0x40f950(ECX = car, owner = lane, path = lane+0x3c, dir = -1);   // src car_set_ref
0x410680(ECX = car, arg = cross+0x44);                           // syncInstance, src Cross::object
```
`cross+0x1c` is the CrossPath array (stride 0x60), `cross+0x14` the CrossLink array
(stride 8, `node` = a Lane* in the exe, a node id in src). `0x40f950` is card 241's
`car_set_ref`; with `car->live` it only sets the next ref, otherwise it unlinks the car
from its owner (here: the cross) and moves it onto the lane.

### 0x410680 TrafficCar__syncInstance (ECX = Car; arg `ResRefLink *link`) — 279 bytes, RET 4
The exe returns **void** in every branch. `bound` = `link+8 != 0` (= our object id != 0),
`inst` = car+0x138, `type` = car+0x14c, `live` = byte car+0xdc, `notify` = byte car+0xdd.
```
inst = car->inst_id;  type = car->type_id;  bound = link->object != 0;
if (type == 0) {                                  // 0x41069c
    if (!inst) return;
    if (!car->live || !bound) link_reparent(&car->link(+0x130), T+0x311c);   // 0x4ec210
    else                      link_reparent(&car->link(+0x130), link);       // 0x4ec210
    return;
}
if (!car->live || !bound) {                       // 0x4106d0 -> 0x410713
    if (!inst) return;
    if (car->notify) queue(T+0x3130, 0, 0x10, sprintf("car_rem %d %d", inst, car)); // 0x5935fc
    car->e8 = T->sim_time;                        // +0xe8 = T+0x84
    Traffic__parkInstance(ECX = T, type, &car->link(+0x130));   // 0x417080
    link_unlink(ECX = &car->link(+0x130));                      // 0x488080
    0x5e64f4 -= 1;  0x5e64fc += 1;                // globals
    return;
}
if (!inst) {                                      // 0x4106dd
    0x5e64f4 += 1;  0x5e64f8 += 1;                // globals
    Traffic__queueSpawn(ECX = T, car);            // 0x41a500
    return;
}
link_reparent(&car->link(+0x130), link);          // 0x410700, 0x4ec210
```
Callees, none of which exists in src (all stay `// shortcut:`):
- 0x4ec210: re-parents the +0x130 instance link (ECX = &car+0x130, arg = target ResRefLink).
- 0x417080 `Traffic__parkInstance(type_id, &car+0x130)` (notes/29 §5).
- 0x488080: unlinks the +0x130 link node from its bucket list (`{+0x130,+0x134,+0x138,+0x13c}`, owner +0x13c, bucket head owner+0x78).
- 0x41a500 `Traffic__queueSpawn(car)` (notes/29 §5); notify sink T+0x3130; counter globals 0x5e64f4/f8/fc meanings UNRESOLVED.

Call sites of 0x410680 in src (all with `road+0x68` / `cross+0x44` = our object id):
`lane_attach` 0x40cbd0 (existing shortcut at traffic.cpp:1104), `set_lane_live` 0x40f6d0
(existing shortcut at traffic.cpp:2522, `for car in lane.cars` per card 219), new
`cross_attach` 0x40cb60. 0x4107a0/`natives.cpp` refreshInstance shortcuts stay (0x4107a0
not traced here).

## Changes
1. `src/game/traffic.h`
   - `Cross`: add `std::vector<uint32_t> cars;` — comment: `// +0x8/+0xc intrusive car list (tail append, 0x40cb60); INVENTED: car indices, tail = back()`.
   - `Car`: add `uint32_t cross_node = 0;` next to `lane` — comment: `// +0x0c owner when it is a cross (exe stores one owner pointer); INVENTED: cross node id, 0 = owner is Car::lane`.
   - declare `void segment_pos_offset(const Segment &s, float d, float off, float out[3]);  // 0x413660` next to `segment_side`.
2. `src/game/traffic.cpp`
   - `segment_pos_offset` after `segment_side`: `segment_pos(s,d,out);` then `segment_side(s,d,side);` then `out[i] = side[i]*off + out[i];` (keep this order).
   - `car_ref_pos_offset` (0x40dde0) `dir >= 0` branch: `segment_pos_offset(*reinterpret_cast<const Segment *>(path), d, off, out); return;` — `path` is `&c.paths[i].seg` for a cross ref (card 242 tag).
   - `car_ref_dir` 0x40dd60 `dir >= 0`: `segment_dir(*reinterpret_cast<const Segment *>(path), d, out); return;`
   - `car_ref_length` 0x40e810 `dir >= 0`: `return reinterpret_cast<const Segment *>(path)->len;` (Segment +0x30).
   - `lane_unlink` 0x40ca40: before the lane branch, `if (car.cross_node != 0) { Cross *c = cross_by_node(g, car.cross_node, nullptr); if (c) erase ci from c->cars; } else if (car.lane < g.lanes.size()) { ...existing... }`; clear `car.cross_node = 0` together with prev/next/lane.
   - new `car_sync_instance(Graph &g, uint32_t ci, uint32_t link_object)` = 0x410680, defined before `lane_attach`; exact branch order above, `bound = link_object != 0`, `car.e8 = g.sim_time` in the park branch, every service call a `// shortcut:`.
   - `lane_attach`: replace the shortcut at 0x410680's place with `Road *road = road_by_node(g, lane.road); car_sync_instance(g, ci, road ? road->object : 0);` (same position, before the `lane.k34` line).
   - `Graph::set_lane_live`: replace the final shortcut with `for (uint32_t ci : lane.cars) car_sync_instance(*this, ci, road ? road->object : 0);` (`road` already computed).
   - `car_set_ref` 0x40f950: in the `!car.live` branch the old-owner detach must not treat a cross as a lane: guard it with `car.dir < 0` and add `// shortcut: cross vf4 0x4883e0 not traced` for `dir >= 0`.
   - new `cross_attach(Graph &g, uint32_t cross_node, uint32_t ci, size_t idx)` = 0x40cb60, after `car_set_ref`: resolve `Cross *c = cross_by_node(g, cross_node, nullptr)` (return if null, INVENTED), `c->cars.push_back(ci); car.cross_node = cross_node;`, guards for `idx < c->paths.size()` and `c->paths[idx].b < c->links.size()` (INVENTED; the exe indexes unchecked), `Lane *lane = lane_at(g, c->links[c->paths[idx].b].node)`, `car_set_ref(g, ci, (size_t)(lane - g.lanes.data()), &lane->path, -1)` when lane, then `car_sync_instance(g, ci, c->object)`.
   - `car_step` step 6 (0x411503, card 242): for `dir >= 0` use the Segment length `pos += reinterpret_cast<const Segment *>(car.path)->len` (instead of card 242's `0`), and replace card 242's cross-vf3 `// SERVICE MISSING:` with `cross_attach(g, car.path_lane, ci, (size_t)car.dir);` in the `else` of the lane-append branch.
   - `cross_bind_object` 0x40ebb0: extract `road_bind_object`'s per-car body unchanged into a file-local helper `car_bind_object(Graph &g, uint32_t ci, uint32_t object)` and call it for `cross.cars` (first to last) and from the road lane walk. The body's services are already shortcuts, so nothing new is modelled; this only makes the new list match the exe's consumer.

## Files
- edit: src/game/traffic.h (Cross::cars, Car::cross_node, segment_pos_offset)
- edit: src/game/traffic.cpp (segment_pos_offset; car_ref_pos_offset/dir/length dir>=0; lane_unlink; car_sync_instance + lane_attach/set_lane_live wiring; car_set_ref guard; cross_attach + step 6; cross_bind_object)

## Done when
- each ported function has a `// 0xADDR Name` comment; the untraced/missing pieces (0x4ec210, 0x417080, 0x488080, 0x41a500, T+0x3130 sink, counters 0x5e64f4/f8/fc, cross vf4 0x4883e0) are marked `INVENTED:`/`shortcut:`
- 0x410680 keeps the exe branch order and returns nothing; no observable race/city change (`race 659.414 s`, `--city 63` unchanged)
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch `card/244`

## UNRESOLVED
- Globals 0x5e64f4 / 0x5e64f8 / 0x5e64fc: incremented/decremented in 0x410680 only; no reader traced, meaning unknown (not modelled).
- 0x4ec210 / 0x417080 / 0x488080 / 0x41a500 semantics beyond the call shape above are not ported (instance side absent from src, notes/29 §5).
- `cross->links[b].node` is a resolved Lane* in the exe and a node id in src; a non-lane id is an INVENTED guard (the exe would call into it).
