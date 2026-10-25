---
id: 214
status: implemented
branch: card/214
depends: [213]
---
# Traffic: lane attach (ctLane vf3) + time-to-lane-end + lane event heap push

## Goal
Finish the spawn path of card 213: after `ctLane__insertCar` the exe calls the lane's vf3
0x40cbd0, which does the live/not-live bookkeeping, schedules a timed lane event on the
Traffic min-heap (T+0) when the new car is the first of a not-live lane, and adds the car's
length to the lane occupancy. Port vf3, `TrafficCar__timeToLaneEnd` 0x40d990,
`TrafficCar__targetSpeed` 0x40dc00 and the heap push 0x417050/0x41a950. The heap consumer
(needs the per-car step 0x4107c0) and the instance side stay out. Source: notes/29 §3.

## Exe evidence (Car fields from card 213; new Lane/Graph fields below)
New Lane fields (ctLane ctor 0x40c610 zeroes all): `live` (+0xa8, byte), `pending_events`
(+0xac), weak ref `blocker` {serial +0xb0, car +0xb4, byte +0xb8}. Existing: `kc8` (+0xc8, lane
occupancy), `k98` (+0x98 speed limit), `k34` (+0x34), `node88[2]`/`node88[3]` (+0x90/+0x94).
New Graph fields: `heap` (T+0, entries {float time, lane}), `live_cars` head (T+0x88, link car+0x44),
`live_count` (global 0x5e6514; INVENTED: per graph).
### 0x40cbd0 ctLane__vf3(lane, car)
```
if (lane.blocker.car == car && (car == null || lane.blocker.serial == car.serial)) lane.blocker = {0, 0, 0}
if (car.lane != lane) append car at the lane's tail              // insert already set it: no-op here
if (!lane.live) {
    if (car.live) {
        live_count--
        if (!car.type_id && car.instance_id && car.notify) queue(sink T+0x3130, 0x10, "car_rem %d %d", car.instance_id, car)
        car.live = 0; car.k128 = 0
    }
    if (lane.cars.front() == car) {
        t = TrafficCar__timeToLaneEnd(car)
        if (lane.pending_events == 0) { Traffic__scheduleLane(t, lane); lane.pending_events++ }
    }
} else if (car.live != 1) {
    live_count++
    if (!car.type_id && car.instance_id && car.notify) queue(sink, 0x10, "car_add %d %d", car.instance_id, car)
    car.live = 1
    if (car.live_next == 0) { car.live_next = T.live_cars; T.live_cars = car }   // head insert
}
TrafficCar__syncInstance(car, lane.road + 0x68)                   // 0x410680
if (lane.k34 == -1) car.k124 = lane.node88[2] ? lane.node88[2] : lane.node88[3]
lane.kc8 += car.k24 + car.k14 + car.k20
```
### 0x40d990 TrafficCar__timeToLaneEnd(car)
```
TrafficCar__step(car)      // 0x4107c0, first lines: if (!car.lane || T.sim_time <= car.e0) return
stop = car.next_lane(+0xbc) ? 0.0 : T.cross_stop_offset
v = TrafficCar__targetSpeed(car); if (v <= 10.0) v = 10.0
t = ((car.pos - car.k14) - stop) * (2.0 / v)
return t < 0 ? 0 : t
```
### 0x40dc00 TrafficCar__targetSpeed(car)
```
s = car.max_speed
if (car.lane && lane.k98 /*ctLane vf6 0x40dd50*/ < s) s = lane.k98
f = 0
if (!car.next_lane(+0xbc)) { if (car.k124) s = (k124_obj->vf6(-1) + s) * 0.5 }
else { f = car.max_speed; if (car.kb8 && kb8_obj->vf6(car.kc0) < f) { f = kb8_obj->vf6(car.kc0); return f <= s ? s : f } }
return f <= s ? s : f
```
At spawn `next_lane`, `kb8` and `k124` are 0 (vf3 sets k124 only after the event), so only the
first two lines run.
### 0x417050 Traffic__scheduleLane(dt, lane) -> 0x41a950 TrafficHeap__push
```
e = { T.sim_time + dt, lane }
i = count++ (capacity grows by 0x400 entries)
while (i) { p = (i - 1) / 2; if (h[p].time <= e.time) break; h[i] = h[p]; i = p }
h[i] = e
```

## Files
- edit: src/game/traffic.h — the Lane/Graph fields above, `struct HeapEntry { float time; uint32_t lane; }`
  (lane index, INVENTED form) with `std::vector<HeapEntry> heap`.
- edit: src/game/traffic.cpp — `lane_attach` (0x40cbd0) called where card 213 left its
  `shortcut:` line, `car_time_to_lane_end` (0x40d990), `car_target_speed` (0x40dc00),
  `schedule_lane` (0x417050/0x41a950). Use exact float order as above.
  - The `TrafficCar__step` call: `// shortcut: 0x4107c0 not ported; it returns at once here
    because a fresh car has e0 == sim_time (notes/29 §6)`.
  - The `k124`/`kb8` vf6 branches: `// shortcut: vf6 of next-lane/cross objects not traced;
    0 for spawned cars` (keep the branch structure, skip the calls).
  - Both `queue(sink, ...)` lines and the `TrafficCar__syncInstance` call: `// shortcut:` (no
    sink/instance in Graph yet, notes/29 §5); keep the counters and flags.
- read only: tasks/213 struct names in src/game/traffic.h.

## Done when
- each ported function has its `// 0xADDR` comment; container/index choices are `INVENTED:`.
- `make -C src clean check` green; vmboot output unchanged.
- card frontmatter: status implemented, branch name.
