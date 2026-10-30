---
id: 240
status: todo
branch:
review:
depends: []
---
# traffic: Graph::step — lane event heap loop (ctLane__timedEvent + heap pop) and the event-1 drain

Promotion note (orchestrator): the UNRESOLVED items are out-of-slice services/untraced helpers
(event-code-1 handler, 0x40e460 lane-end move, 0x5fde90 context). Implement exactly as below
and mark them `shortcut:`/`SERVICE MISSING`; do not invent behaviour.

## Goal
Finish `Traffic__step` 0x41b290 in `Graph::step` (src/game/traffic.cpp). The four active-list
walks are already ported (cards 220/233). Add what is still missing, in the exe's order:
1. the pseudo-car prologue stores at the top (T+0x3118; a no-op in src, nothing is parked),
2. the lane event min-heap loop **before** the walks: pop every entry whose `time < t`, running
   `ctLane__timedEvent` 0x40fcf0 on its lane, then `TrafficHeap__pop` 0x41a810,
3. the event-1 list drain at T+0xc8 **after** the active-lane walk: unlink each car
   (`car+0x10`), clear `car+0x10`, and when `car+0x138` (instance id) is set queue event code 1
   to the car's instance (`Game__queueEvent` 0x47d550 -> 0x47c760).
New internal helpers: `lane_timed_event` (0x40fcf0), `heap_pop` (0x41a810) and
`lane_schedule_if_idle` (0x40d380). No new `Car`/`Lane`/`Graph` fields (all exist).

Correction to the brief: 0x41b290 contains **no** T+0x3130 walk. T+0x3130 is a ResRefLink event
*sink* (notes/27 §2), not a list; the event-1 drain is the only notify-like walk. Leave the
existing `// shortcut: the notify sink (T+0x3130) ...` comments in lane_attach / car_drop_live /
car_live_step / remove_car untouched.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x41b290 Traffic__step(t) (ECX = T = this) — prologue, heap loop, drain
```
DAT_005e652c = T
T+0x84 (sim_time) = t
if (T+0x3118 != 0) { pc = T+0x3118; pc+0xe8 = pc+0xe4 = pc+0xe0 = t + 1.0f }  // 0x593598 = 1.0f
while (*(int*)(T+0x4) != 0 && *(float*)*(int*)T < t) {           // heap count, heap[0].time, strict <
    ctLane__timedEvent(ECX = heap[0].lane)                      // 0x40fcf0, entry = {float time @+0, Lane* @+4}
    TrafficHeap__pop(ECX = T)                                   // 0x41a810
}
[four walks T+0x90 / T+0x94 / T+0x88 / T+0x8c — already ported, unchanged]
while (*(int*)(T+0xc8) != T) {                                  // sentinel is T itself
    car = *(int*)(T+0xc8)
    T+0xc8 = *(int*)(car+0x10)
    *(int*)(car+0x10) = 0
    if (*(int*)(car+0x138) != 0)
        Game__queueEvent(ECX = 0x5fde90, &car+0x130, 0, 1, 0, 0)  // 0x47d550 -> 0x47c760
}
```
`0x47d550` is a 9-byte thunk (`JMP 0x47c760`), so the args are 0x47c760's: ECX = 0x5fde90
(a BSS static context object, all zero in the file; `0x47c760` reads its own state from ECX),
stack args `(ref &car+0x130, sender 0, code 1, param 0, extra 0)`. 0x47c760 binds the target via
`ResRefLink__set 0x40cac0` from `ref+0xc` (the Resource*, car+0x13c) and returns **1**
(`0x47c944 MOV EAX,1`); 0x41b290 ignores the result. ResRefLink layout: `+0 prev, +4 next,
+8 id (car+0x138), +0xc Resource* (car+0x13c)` (notes/27 §2, notes/32).
UNRESOLVED: which handler event code 1 reaches (mask bit 0) — not traced; the call itself is exact.

### 0x40fcf0 ctLane__timedEvent (ECX = lane) — 226 bytes, complete
```
lane+0xac (int) = lane+0xac - 1
if ((int)(lane+0xac) < 0) lane+0xac = 0
if (lane+0xa8 /*live*/ != 0) return                    // live lanes are stepped by 0x40f4e0
for (car = lane+0x8; car != 0; ) {                     // lane car-list head, car+0x8 = next
    next = *(int*)(car+0x8)                            // read BEFORE the step (it may relink)
    TrafficCar__step(ECX = car)                        // 0x4107c0 -> src car_step
    car = next
}
head = lane+0x8
if (head != 0 && *(int*)(head+0xbc) == 0) {            // no next lane
    f = TrafficCar__timeToLaneEnd(ECX = head)          // 0x40d990 -> src car_time_to_lane_end
    if ((double)f <= 0.01) {                           // 0x593560 = 0x3F847AE147AE147B = 0.01 (double)
        if (!0x40e460(ECX = lane, head, 0.0f, lane+0x38))
            ctLane__scheduleIfIdle(ECX = lane, 1.0f)   // 0x40d380, 0x3f800000
    } else {
        if (lane+0xac != 0) return
        Traffic__scheduleLane(ECX = DAT_005e652c, f, lane)   // 0x417050 -> src schedule_lane
        lane+0xac++
    }
}
if (lane+0xac == 0 && lane+0x8 != 0) {
    Traffic__scheduleLane(ECX = DAT_005e652c, 1.0f, lane)
    lane+0xac++
}
```
`(double)f <= 0.01` is equivalent to the float test `f <= 0.01f` for every float (0.01f <= the
double 0.01 and the next float is above it); either form is behaviour-equal.
0x40e460 is the lane-end move to the next lane (args: lane, head car, float 0.0, lane+0x38); its
body is **not traced**. src has no next-lane move, so treat it as failed: the same
`lane_schedule_if_idle(1.0f)` runs. If 0x40e460 succeeded, the exe still reaches the final
`scheduleLane(1.0f)` whenever `pending == 0`, so the scheduling difference is only when 0x40e460
itself sets pending (unobserved).

### 0x40d380 (unnamed; call it ctLane__scheduleIfIdle) (ECX = lane, float dt)
```
if (lane+0xac != 0) return
Traffic__scheduleLane(ECX = DAT_005e652c, dt, lane)    // 0x417050
lane+0xac++
```

### 0x41a810 TrafficHeap__pop (ECX = T) — heap array T+0, count T+4, entry 8 bytes
```
n = T+4 - 1;  T+4 = n
if (n != 0) {
    i = 0; c = 1
    while (c < n) {                                    // last element = h[n]
        r = 2*i + 2
        if (r < n) {
            if (h[c].time <= h[r].time) goto LEFT
            if (h[n].time <= h[r].time) break
            h[i] = h[r]; i = r
        } else {
LEFT:       if (h[n].time <= h[c].time) break
            h[i] = h[c]; i = c
        }
        c = 2*i + 1
    }
    h[i] = h[n]
}
```
The last element is only *read* (h[n]) and written at the end; children are always < n, so copying
the vector's back element into `last` before `pop_back()` is exact.

### Mapping to src (fields already exist)
| exe | src |
|---|---|
| T+0 array, T+4 count | `Graph::heap` (`std::vector<HeapEntry{float time; uint32_t lane}>`), push already `schedule_lane` 0x417050 |
| lane+0xac / +0xa8 / +0x8 / +0x38 | `Lane::pending_events` / `Lane::live` / `Lane::cars` (vector, INVENTED) / `Lane::k38` |
| car+0xbc / +0x10 / +0x138 / +0x13c | `Car::next_lane_b` / `Car::event_next` / `Car::inst_id` / `Car::k13c` (Resource* in the exe, uint32 id word here) |
| car+0xe0..0xe8 | `Car::e0/e4/e8` |
| T+0xc8 | `Graph::event1` (1-based car index, `kLiveEnd` sentinel; producer already in `car_check_obstacle` 0x40eff0) |

## Changes
- edit: src/game/traffic.cpp only.

1. In the anonymous namespace after `lane_active_step` 0x40f4e0 (before `}  // namespace`):
   - `void lane_schedule_if_idle(Graph &g, Lane &lane, size_t li, float dt)` — 0x40d380 body.
   - `void heap_pop(Graph &g)` — 0x41a810 body. `const Graph::HeapEntry last = g.heap.back();`
     then `pop_back()`; if empty return; sift down exactly as above (`<`/`<=` order as written),
     final `g.heap[i] = last`.
   - `void lane_timed_event(Graph &g, uint32_t li)` — 0x40fcf0 body. Use the same INVENTED
     index loop as `lane_active_step` (`for (k = 0; k < lane.cars.size(); k++) car_step(g,
     lane.cars[k]);`) with an `// INVENTED:` comment (the exe reads car+0x8 before each step;
     the vector can change under a step). `head` = `lane.cars.empty() ? kLiveEnd :
     lane.cars.front()`. The `head == lane.cars.front()` is the same INVENTED head that
     `lane_attach` uses (`lane.cars.front()`, 0x40cbd0). Call `car_time_to_lane_end(g, head)`
     (0x40d990, it steps the head again; the `t > car.e0` gate inside `car_step` makes that a
     no-op, as in the exe). Shortcut comment for 0x40e460 as described in Goal/evidence.
2. `Graph::step`: after `sim_time = t;` insert the heap loop
   `while (!heap.empty() && heap.front().time < t) { lane_timed_event(*this, heap.front().lane);
   heap_pop(*this); }` — strict `<`, and it must stay **before** the cross_eq walk. Narrow the
   existing shortcut comment to the pseudo-car / `DAT_005e652c` only.
3. `Graph::step`: after the active-lane walk add the event-1 drain:
   ```cpp
   while (event1 != kLiveEnd) {
       const uint32_t ci = event1 - 1;
       Car &car = cars[ci];
       event1 = car.event_next;   // T+0xc8 = car+0x10
       car.event_next = 0;
       if (car.inst_id != 0) {
           // 0x47d550 Game__queueEvent(ECX = 0x5fde90, &car+0x130, 0, 1, 0, 0)
           // SERVICE MISSING: the traffic module has no Runtime* (resolve()/queue_event
           // live in script/natives.cpp), so the code-1 event and the +0x130 ref are not
           // dispatched. The exe dispatches to the instance id car+0x138 and returns 1.
       }
   }
   ```
   `car.inst_id` *can* be non-zero (native addTrafficCar with an instance, natives.cpp:7347),
   so keep the check; the empty body is the documented missing service.
4. Do not add fields, do not touch traffic.h, do not touch the four walks, cards 237-239 code,
   or the T+0x3130 shortcut comments.

## Files
- edit: src/game/traffic.cpp (`Graph::step` 0x41b290, new `lane_timed_event` 0x40fcf0,
  `heap_pop` 0x41a810, `lane_schedule_if_idle` 0x40d380)
- read only: src/game/traffic.h (Lane/Car/Graph fields), src/script/natives.cpp:7347 (inst_id setter)

## Done when
- each ported function has a `// 0xADDR Name` comment; the next-lane move and the missing event
  dispatch are `shortcut:`/`SERVICE MISSING` comments, the car-list index loop is `INVENTED:`
- the heap loop sits before the cross_eq walk and the event-1 drain after the active-lane walk
- `make -C src check` green (the `--city` run executes the heap loop; the vmboot heap/pending
  checks must still hold); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
