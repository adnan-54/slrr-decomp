---
id: 233
status: todo
branch:
depends: [232]
---
# Traffic: TrafficCar__liveStep 0x40f350 + Graph::step live-car walk

## Goal
Port the third `Graph::step` walk (notes/37 §1, §3): the live-car list (`Graph::live_cars`, T+0x88,
link `Car::live_next`, 1-based car index, `kLiveEnd` = end). Each live car claims the cross paths of
its next/current ref and drops out of the live state when it has no next ref and its current lane is
no longer live; a car that is not live is unlinked. The per-car motion inside
(TrafficCar__pushInstance 0x4118f0 -> advance 0x4102d0 -> TrafficCar__step 0x4107c0) stays a
shortcut, so cars still do not move. Dead code in today's vmboot (no lane goes live); **race/city
numbers may move once lanes go live: report any check delta.** Generic, no map data.

## Exe evidence (notes/37 §3, asm-checked)
Car refs, 3 dwords each {owner, path, idx}: next = +0xb8/+0xbc/+0xc0, current = +0xc4/+0xc8/+0xcc.
idx == -1: owner is a lane; idx >= 0: owner is a cross and idx its path index.
src: next = `next_lane_a` (+0xb8) / `next_lane_b` (+0xbc) / `kc0` (+0xc0, really an int index, unused
in src); current = `path_lane` (+0xc4, lane index) / `path` (+0xc8) / `dir` (+0xcc).
### 0x40f350 TrafficCar__liveStep(car) -> bool
```
if (!car.live /*+0xdc*/) { TrafficCar__pushInstance(car); return false }
if (next.path && next.idx >= 0) {                       // next.owner is a cross
    if (car.pos /*+0x88*/ <= car.speed*car.speed / (car.accel + car.accel) + car.k14)
        ctCross__claimPath(next.owner, next.idx)
    else if (ctCross__pathBlocked(next.owner, next.idx)) next.path = 0      // +0xbc
}
if (cur.path && cur.idx >= 0) ctCross__claimPath(cur.owner, cur.idx)
if (!next.path && !cur.owner->vf1() && car.live) {
    // lane vf1 0x40e390 = lane.live (+0xa8); cross vf1 0x40e380 = live_refs != 0
    live_count--                                         // global 0x5e6514
    if (!car.type_id && car.inst_id && car.notify)       // +0x14c, +0x138, +0xdd
        queue(T+0x3130, 0x10, "car_rem %d %d", car.inst_id, car_pointer)
    car.live = 0
    if (car.k128) car.k128 = 0
}
TrafficCar__pushInstance(car)                            // 0x4118f0
return true
```
### 0x41b290 walk (after the light walk of card 232, before the lane walk)
```
p = &T+0x88; while (*p != T) { if (liveStep(*p)) p = &(*p)->next_0x44; else { n = (*p)->next; (*p)->next = 0; *p = n; } }
```

## Files
- edit: src/game/traffic.cpp
  - new static `car_live_step(Graph&, Car&)` 0x40f350. src never puts a car on a cross and never sets
    a next ref, so: implement the lane-owner case (`dir < 0`: vf1 = `lanes[path_lane].live`, with the
    existing INVENTED bounds style: an out-of-range `path_lane` counts as not live) and guard the two
    cross-claim branches with `// shortcut: cross owners (idx >= 0) are not representable in src until
    TrafficCar__step 0x4107c0 hands cars to crosses` (no code for them). `car_rem`: reuse the existing
    `// shortcut: the notify sink (T+0x3130) is not modelled` comment style. Both
    TrafficCar__pushInstance calls: one `// shortcut: 0x4118f0 -> 0x4102d0 -> 0x4107c0 not ported (notes/37 §4)`.
  - `Graph::step`: add the live-car walk between the light walk (card 232) and the lane walk, same
    unlink rule (`live_next = 0` on unlink). Update the shortcut comment accordingly.
- read only: src/game/traffic.h (`Car`, `Graph::live_cars`, `live_count`), notes/37 §3.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`/`shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- report: `vmboot --race` time and `vmboot --city` check count before/after (capped: `ulimit -v 1500000; timeout 300`)
- card frontmatter: status implemented, branch name
