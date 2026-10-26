---
id: 219
status: implemented
branch: card/219
depends: [218]
---
# Traffic: ctLane__setLive, road keep-alive (road vf1 -> lane vf7), cross activation

## Goal
Port the lane live-state switch and its callers that a road object uses to keep its lanes live
(notes/32 §5). A live lane makes its cars live (live-car list) and holds a refcount on the road's two
crosses; switching it off drops the cars and schedules the first car's lane-end event. Nothing calls
the keep-alive yet (the physics driver is not carded, notes/32 §4); card 220 adds the expiry.
No map-specific code: everything works on the loaded Graph.

## Exe evidence (asm-checked; T = Traffic object, node pointers are ids in src)
### 0x40f6d0 ctLane__setLive(lane, on)
```
if (lane.live == on) return                          // +0xa8
g_live_lanes += on ? 1 : -1                          // global 0x5e6500
lane.live = on
road = lane.road                                     // +0x10
if (on) {
    for c in (road.node[1], road.node[2]):           // road+8, road+0xc: crosses (eq or light)
        if (c.k38 == 0) { cross_vf10(c); c.k38 = 1 }  // +0x38 signed refcount
        else if (c.k38 < 0) c.k38 = 1
        else c.k38 += 1
    for car in lane.cars (first to last):
        if (car.live != 1) {                         // +0xdc
            g_live_count++                           // 0x5e6514 = Graph::live_count
            if (car.type_id == 0 && car.inst_id && car.notify) queue "car_add %d %d" to T+0x3130
            car.live = 1
            if (car.live_next == 0) { car.live_next = T.live_cars; T.live_cars = car }   // head insert
            // exe then tests (car.live == 0 && car.k128) -> dead code, car.live is 1 here
        }
    if (lane.active_next == 0) { lane.active_next = T.active_lanes; T.active_lanes = lane }  // +0xa4, T+0x8c
} else {
    for c in (road.node[1], road.node[2]): c.k38 -= 1; if (c.k38 == 0) c.k38 = -1
    for car in lane.cars: TrafficCar__dropLive(car)  // 0x40fa50
    if (lane has cars) {
        t = TrafficCar__timeToLaneEnd(first car)     // 0x40d990 = src car_time_to_lane_end
        if (lane.pending_events == 0) { Traffic__scheduleLane(t, lane); lane.pending_events++ }  // +0xac
    }
}
for car in lane.cars: TrafficCar__syncInstance 0x410680(car, road+0x68)
```
### 0x40fa50 TrafficCar__dropLive(car)
```
if (car.next_lane_b == 0 && car.live) {              // +0xbc, +0xdc
    g_live_count--
    if (car.type_id == 0 && car.inst_id && car.notify) queue "car_rem %d %d" to T+0x3130
    car.live = 0
    if (car.k128) car.k128 = 0
}
```
### 0x40eed0 ctLane__vf7 (keep alive)
```
ctLane__setLive(lane, 1); lane.kc0 = T.sim_time      // +0xc0 = T+0x84
```
### 0x40f8e0 ctRoad__vf1
```
for (l = road.node[0]; l; l = l.node14) l->vf7()     // road+4 first lane, lane+0x14 next
```
### 0x415f20 ctCrossEq__vf10 / 0x415f40 ctCrossLight__vf10 (activate)
```
eq:    if (c+0x54 == 0) { c+0x54 = T+0x90; T+0x90 = c }
light: if (c+0x60 == 0) { c+0x60 = T+0x94; T+0x94 = c }
```
All list ends are the sentinel T (0x416000 ctor sets T+0x88/+0x8c/+0x90/+0x94 = T); src already
encodes that end as `kLiveEnd` for the live-car list (card 216) — use the same encoding.

## Files
- edit: src/game/traffic.h
  - `Lane`: add `uint32_t active_next = 0;  // +0xa4 (0 = not listed, kLiveEnd = list end)`.
  - `Cross`: rename/retype `k38` to `int32_t live_refs = 0; // +0x38 live-lane refcount, -1 = going inactive`
    (its only use is `c.k38 = 0;` in cross_init, traffic.cpp ~699; `Lane::k38` and `Car::k38`
    are different fields, leave them) and add
    `uint32_t active_next = 0; // +0x54 (eq) / +0x60 (light)`.
  - `Graph`: add `uint32_t active_lanes = kLiveEnd; // +0x8c`, `active_eq = kLiveEnd; // +0x90`,
    `active_light = kLiveEnd; // +0x94` (1-based indices into lanes / cross_eq / cross_light, INVENTED
    encoding like live_cars), `int32_t live_lanes = 0; // global 0x5e6500, INVENTED: per graph`.
  - public: `void set_lane_live(size_t lane, bool on);  // 0x40f6d0` and
    `void keep_road_live(size_t road);  // 0x40f8e0 road vf1 -> 0x40eed0 lane vf7`.
- edit: src/game/traffic.cpp
  - `cross_by_node(g, id, &is_light)` helper: id 1..eq -> cross_eq[id-1], eq+1..eq+light ->
    cross_light; anything else null (INVENTED bounds check, the exe dereferences a pointer).
    Also a road lookup for `lane.road` (ids: eq + light + 1 .. + roads), INVENTED bounds check.
  - `cross_activate` (0x415f20/0x415f40), `car_drop_live` (0x40fa50), `Graph::set_lane_live`,
    `Graph::keep_road_live` (walk road.node[0] then node14 with the same lane-count guard as
    `road_bind_object`). car_add / car_rem notify and syncInstance: `// shortcut:` like
    `lane_attach` (the T+0x3130 sink and instance side are not modelled).
  - A null cross/road id (bad data) skips that cross: mark `// INVENTED:`.
- read only: src/game/traffic.cpp `lane_attach`, `car_time_to_lane_end`, `schedule_lane`,
  `road_bind_object` (copy their style and list encoding).

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`/`shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
