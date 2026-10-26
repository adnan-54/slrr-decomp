---
id: 220
status: todo
branch:
depends: [219]
---
# Traffic: Graph::step active-lane walk + ctLane__activeStep expiry

## Goal
Replace part of the `Graph::step` shortcut (traffic.cpp, `void Graph::step`) with the exe's walk of
the active-lane list (T+0x8c, filled by card 219's `set_lane_live`). A live lane that no road
keep-alive refreshed for more than 0.5 s of traffic time is switched off and unlinked. The per-car
movement inside a live lane (TrafficCar__step 0x4107c0, hand-over 0x40e460) stays a shortcut, as do
the other three walks and the event heap (their step functions are not ported). Generic, no map data.

## Exe evidence
### 0x41b290 Traffic__step(T, t), notes/31 §5 (verified)
```
T.sim_time = t                                                   // +0x84 (already in src)
while (heap not empty && heap[0].time < t) { ctLane__timedEvent; TrafficHeap__pop }   // not ported
for (list, link, step) in [(T+0x90 active_eq,    +0x54, 0x415c40),     // not ported
                           (T+0x94 active_light, +0x60, 0x415d60),     // not ported
                           (T+0x88 live_cars,    +0x44, 0x40f350),     // not ported
                           (T+0x8c active_lanes, +0xa4, ctLane__activeStep 0x40f4e0)]:
    p = &list
    while (*p != T /*end*/) {
        if (step(*p)) p = &(*p).link
        else { n = (*p).link; (*p).link = 0; *p = n }            // unlink, zero the link
    }
notify queue T+0xc8 ...                                          // not ported
```
### 0x40f4e0 ctLane__activeStep(lane) -> 1 = stay listed
```
if (!lane.live) return 0                                         // +0xa8
if (T.sim_time - lane.kc0 <= 0.5) {                              // +0x84, +0xc0 (set by lane vf7)
    for car in lane.cars (next fetched before the body):
        if (car.next_lane_b == 0) {                              // +0xbc
            TrafficCar__step 0x4107c0(car)
            v = max(TrafficCar__targetSpeed 0x40dc00(car), 10)  // src car_target_speed
            t = max(((car.pos - car.k14) - T.cross_stop_offset) * (2 / v), 0)   // +0x88, +0x14, T+0x3140
            0x40e460(lane, car, t, lane.k38)                     // hand-over to the next lane
        }
    return 1
}
ctLane__setLive(lane, 0)                                         // card 219 set_lane_live(lane, false)
return 0
```

## Files
- edit: src/game/traffic.cpp
  - `lane_active_step(Graph &g, size_t li)` (0x40f4e0): the expiry exactly as above; the per-car
    body is one `// shortcut: TrafficCar__step 0x4107c0 and hand-over 0x40e460 not ported` (do not
    compute the unused t).
  - `Graph::step`: keep `sim_time = t`; then walk `active_lanes` with the unlink rule above
    (1-based lane index, `kLiveEnd` = end, link = `Lane::active_next`). Keep one shortcut comment for
    the heap pop, the eq/light/live-car walks and the notify queue (cite 0x41b290). Walk order stays
    eq, light, cars, lanes: put the lane walk last.
- read only: src/game/traffic.h (`Lane::active_next`, `Graph::active_lanes`, `set_lane_live` from card 219)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`/`shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
