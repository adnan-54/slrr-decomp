---
id: 216
status: todo
branch:
depends: [215]
---
# GroundRef: remTrafficCar / notifyTrafficCar / setTrafficCarBehaviour + TrafficCar__remove

## Goal
Turn the three stubs `GroundRef.remTrafficCar` (0x4de520), `notifyTrafficCar` (0x4de5f0) and
`setTrafficCarBehaviour` (0x4e6280) into the exe behaviour on the src car pool (cards 213-215), and
port `TrafficCar__remove` 0x40f290 with its lane unlink 0x40ca40 and lane detach `ctLane__vf4`
0x40e710. Also fix the live-list end marker (the exe list ends at a sentinel, not 0). vmboot
`--city` reaches 45 notifyTrafficCar(id, 1) calls (Valocity.java:164); remTrafficCar is only
called when leaving Valocity (Valocity.java:266), which `--city` does not do. notes/30 §1.

## Exe evidence (src names: car = `traffic::Car`, handle = index + 1, see card 213)
### 0x4de520 GroundRef.remTrafficCar(int car)
```
if (car) {
    if (car.inst_id /*+0x138*/) getInfo(instance link +0x130, 0x38, &zero)  // type_car take-over release: NOT ported (take-over itself is not ported, notes/27 §3)
    TrafficCar__remove(car)
}
```
### 0x4de5f0 GroundRef.notifyTrafficCar(int car, int flag) -> int
```
if (!car) return 0
if (flag >= 0) car.notify /*+0xdd*/ = flag != 0
return car.notify
```
### 0x4e6280 GroundRef.setTrafficCarBehaviour(int car, int mode)
```
if (car && car.inst_id) car.behaviour /*+0x160*/ = mode
```
### 0x40f290 TrafficCar__remove(car) (asm-checked)
```
if (car is not on a lane /*car+0xc == 0*/) return       // nothing, slot not freed
unlink car from its lane list                            // 0x40ca40: car+4/+8/+0xc = 0
if (car.path_lane /*+0xc4*/) path_lane.vf4(car, car.dir) // ctLane__vf4 below (a cross in the exe when on a cross path; src cars are always on lanes)
car.next_lane_a = car.next_lane_b = 0                    // +0xb8, +0xbc
car.path_lane = 0; car.path = 0; car.kd0 = car.kd4 = 0   // +0xc4, +0xc8, +0xd0, +0xd4
car.lane = 0 (off lane)                                  // +0xc
if (car.live_next /*+0x44*/ != 0) {                      // in the live list
    if (T.live_cars == car) T.live_cars = car.live_next
    else { p = T.live_cars; while (p.live_next != car) p = p.live_next; p.live_next = car.live_next }
    car.live_next = 0
}
TrafficCar__release(car)                                 // 0x40c730 = src car_release
push car on the pool free list                           // src: free_cars.push_back(ci)
```
### 0x40e710 ctLane__vf4(lane, car, dir) — detach (dir unused)
```
lane.kc8 -= car.k24 + car.k14 + car.k20; if (lane.kc8 < 0) lane.kc8 = 0
f = first car of lane (after the unlink)
if (f) {
    TrafficCar__step(f)                                  // 0x4107c0: NOT ported (same shortcut as car_time_to_lane_end)
    t = timeToLaneEnd(f)                                 // the 0x40d990 math inlined: stop, max(targetSpeed,10), max(0, ...)
    if (lane.pending_events == 0) { schedule_lane(t, lane); lane.pending_events++ }
}
if (lane.k34 == -1) car.k124 = 0
```
### Live-list end marker (Traffic__ctor 0x416000: `T+0x88 = T`; Traffic__step 0x41b290 walks `while (node != T)`)
In the exe the live list (`T+0x88`, link `car+0x44`) ends at a sentinel (T), so every listed car
has `live_next != 0` and `live_next == 0` means "not listed". src `lane_attach` (0x40cbd0) head-inserts
with `car.live_next = g.live_cars` where an empty list is 0, so the last car looks unlisted.
Fix: an end marker constant (e.g. `kLiveEnd = 0xffffffff`, INVENTED encoding of the sentinel T):
`Graph::live_cars` starts at `kLiveEnd`, a listed car's `live_next` is `kLiveEnd` or `index + 1`,
0 stays "not listed". The attach test `if (car.live_next == 0)` is then exact.

## Files
- edit: src/game/traffic.h — `Graph::live_cars` initial value (end marker); declare
  `void remove_car(uint32_t handle);` (0x40f290) next to `spawn_on_road`.
- edit: src/game/traffic.cpp — `remove_car` (with the unlink and `lane_detach` 0x40e710 as
  file-local helpers next to `lane_attach`; reuse `car_time_to_lane_end`, `schedule_lane`,
  `car_release`); the end marker in `lane_attach`.
- edit: src/script/natives.cpp — `gr_rem_traffic_car`, `gr_notify_traffic_car`,
  `gr_set_traffic_car_behaviour` (lines ~7359-7376, ~7479): graph via
  `game_rt()->ground_traffic(value_handle(a[0]))` like `gr_add_traffic_car`; replace the
  SERVICE MISSING comments.

## Notes for the implementer
- "on a lane" (`car+0xc != 0`): in src `car.lane` is an index where 0 is valid. Use
  `car.serial != 0` (pool slot in use; every src car is inserted on a lane by spawn_on_road) and
  mark it `INVENTED:`. Handle 0, handle > cars.size(), or a freed slot (serial 0): do nothing /
  return 0 (`INVENTED:` bounds check; the exe dereferences the pointer).
- Lane list in src is `Lane::cars` (vector of car indices): unlink = erase `ci` from
  `lanes[car.lane].cars`.
- Each NOT-ported line is one `// shortcut:` comment.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced/encoding choices are `INVENTED:`.
- `make -C src clean check` green; `--selfcheck`/`--race` output unchanged; `--city` checks pass
  (run vmboot with `ulimit -v` + `timeout`, see CHECKPOINT environment notes).
- no new vmboot check: `--city` never calls remTrafficCar, and addTrafficCar already sets
  `notify` from the car cfg, so a notify check could not catch a misread.
- card frontmatter: status implemented, branch name.
