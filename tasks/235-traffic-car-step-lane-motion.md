---
id: 235
status: todo
branch:
review:
depends: []
---
# traffic: TrafficCar__step lane-motion slice (free distance, speed integration, ref hand-over)

## Goal
Port the pure lane-motion slice of `TrafficCar__step` 0x4107c0 (ECX = car) into
`traffic::Graph`: the entry gate, step 1 free distance (with `TrafficCar__targetSpeed`
0x40dc00), step 5 speed integration, and step 6 ref hand-over (0x40ca40 unlinkLane,
ctLane vf3 0x40cbd0 / vf4 0x40e710). Wire the two src functions that already call it
(`car_time_to_lane_end` = 0x40d990, `lane_active_step` = 0x40f4e0).

Steps 2 (leader slots, 0x40d2c0/0x40daa0), 3 (obstacle scan), 4 (lane-change decision,
0x40fde0) and 7/8 (projection/registration) are **later cards**: do not implement them, do
not mark them UNRESOLVED. In this card `leader_speed` stays at its exe init `-1.0` and
`free` stays at the step-1 `d0`; the code must be shaped so those later cards only fill the
two locals. Source notes: notes/37 §4, notes/29 §3, notes/30 §1.

## Exe evidence (cleaned pseudocode, renamed fields)
`Car` offsets: +0xc lane, +0x14 k14, +0x24 k24, +0x28 k28, +0x2c max_speed, +0x30 accel,
+0x38 decel(k38), +0x88 pos, +0xa8 lateral, +0xac kac, +0xb0 speed, +0xb4 kb4,
+0xb8/+0xbc/+0xc0 next ref {owner, path, idx}, +0xc4/+0xc8/+0xcc cur ref,
+0xdc live, +0xe0 e0, +0x12c k12c (lane-change rate), +0x128 k128.
T = the traffic global 0x5e652c; src passes `Graph &g`.

### 0x4107c0 entry gate (0x4107cc..0x410802)
```
if (car.lane (+0xc) == 0) return;
t = g.sim_time (+0x84);
if (!(t > car.e0 (+0xe0))) return;      // 0x4107eb COMISS e0,t ; JNC return
dt = t - car.e0; car.e0 = t;            // 0x4107f4
```
### step 1: free distance (0x41080a..0x410871)
```
d0 = car.pos - car.k14;                                    // +0x88, +0x14
if (car.next_lane_b (+0xbc) == 0)                          // 0x41081c JZ
    d0 = max(d0 - g.cross_stop_offset (+0x3140), 0.0);     // 0x41084a MAXSS [0x593580=0]
else
    d0 += (car.kc0 (+0xc0) < 0) ? next_path[+0x38]         // lane path length
                                : next_path[+0x30];        // cross path object
target = TrafficCar__targetSpeed(car);                     // 0x41086c -> 0x40dc00, EXISTS:
                                                           //   src car_target_speed, no change
leader_speed = -1.0;                                       // [EBP-0x20], 0x5935ac = -1.0f
g_brake      = (max(car.speed - 2.0, 0.0) * 0.5 + 1.0) * car.k24;   // 0x410888..0x4108b4
                                                           // (only steps 2/3/4 read it: do not compute it yet)
free = d0;                                                 // [EBP-0x8]
```
### steps 2/3/4 (NOT in this card)
0x4108b9..0x411105: leader slots, obstacle scan, lane-change decision. They only modify
`leader_speed`, `free`, the candidate gaps and `car.k128`.
### step 5: speed integration (0x411068..0x41144b)
Simple block (0x411455..0x4114fb), taken when `car.k128 == 0 && car.live (+0xdc) == 0`:
```
limit = 1.5749999 - car.k28;                               // 0x593718 = 0x3fc99999, +0x28
car.kac = 0;                                               // +0xac
if (|car.lateral| (+0xa8) > limit) {
    if (car.lateral >= 0) { v = limit;  if (v <= 0) v = 0; car.lateral = v; }
    else                  { v = car.k28 - 1.5749999; if (v >= 0) v = 0; car.lateral = v; }
}
travelled = target * dt;
car.kb4 = 0;                                               // +0xb4
if (travelled <= free) car.speed = target;
else { car.speed = free / dt; travelled = free; }
car.pos -= travelled;
```
Live block (0x41110a..0x411448), taken otherwise:
```
desired = (leader_speed <= -0.5) ? 0.0                     // 0x593744 = -0.5
        : (d0 - free <= 0.0)     ? min(0.0, leader_speed)
        :                          min(sqrt(2 * car.accel * (d0 - free)), leader_speed);
old = car.speed;
if (target < old) {                        // decelerate, car.k38 (+0x38)
    tt = (old - target) / car.k38;
    if (tt < dt) { travelled = (target*target - old*old) * 0.5 / car.k38 + (dt - tt) * target;
                   new_speed = target; }
    else         { travelled = (old - 0.5 * car.k38 * dt) * dt;
                   new_speed = old - car.k38 * dt; }
} else {                                   // accelerate, car.accel (+0x30)
    tt = (target - old) / car.accel;
    if (dt <= tt) { travelled = (car.accel * 0.5 * dt + old) * dt;
                    new_speed = car.accel * dt + old; }
    else          { travelled = (target*target - old*old) * 0.5 / car.accel + (dt - tt) * target;
                    new_speed = target; }
}
if (free > travelled) desired = sqrt(2 * car.accel * (free - travelled) + desired * desired);
else                  travelled = free;
car.pos -= travelled;
if (car.k128 (+0x128) != 0) {              // lane-change steering (0x4112ac..0x411328)
    up   = min((car.kac + 5.0 * dt) * dt,  0.3 * travelled);        // 0x593738=5, 0x593704=0.3
    lo   = max((car.kac - 5.0 * dt) * dt, -0.3 * travelled);
    push = (float)(int32_t)car.k128 * -3.5 - car.lateral;           // 0x593748 = -3.5
    if (lo <= push) push = min(push, up); else push = lo;
    car.lateral += push; car.kac = push / dt;
} else {                                   // lateral centring (0x41132d..0x41142c)
    limit = 1.5749999 - car.k28;
    if (|car.lateral| > limit || |car.kac| > 5.0 * dt) {
        up   = min((car.kac + 5.0 * dt) * dt,  0.3 * travelled);
        lo   = max((car.kac - 5.0 * dt) * dt, -0.3 * travelled);
        push = 0;
        if (car.lateral >  limit) push = car.lateral - limit;
        else if (car.lateral < -limit) push = car.lateral + limit;
        push = -push;
        if (lo <= push) push = min(push, up); else push = lo;
        car.lateral += push; car.kac = push / dt;
    }
}
car.speed = min(new_speed, desired);       // 0x411431
car.kb4 = (car.speed - old) / dt;          // 0x411440
```
### step 6: ref hand-over (0x411503..0x4115ee)
```
if (car.speed < 0) car.speed = 0;          // 0x411506
if (car.pos < 0) {                         // 0x411519 COMISS 0,pos ; JBE skip
    if (car.next_lane_b == 0) {            // 0x411526
        car.pos = 0;
    } else {
        prev = {car.path_lane (+0xc4), car.path (+0xc8), car.dir (+0xcc)};   // saved in locals
        car.path_lane = car.next_lane_a (+0xb8);   // 0x411546..
        car.path      = car.next_lane_b;
        car.dir       = car.kc0 (+0xc0);
        car.next_lane_a = 0; car.next_lane_b = 0;  // +0xc0 is NOT cleared (0x411576/0x411580)
        car.pos += (car.path != NULL) ? ((car.dir < 0) ? car.path->length  // +0x38
                                                       : /* cross +0x30, see Files */ 0.0f)
                                      : 0.0f;      // 0x41158a..0x4115b3
        TrafficCar__unlinkLane(car);               // 0x4115bb -> 0x40ca40, EXISTS: src lane_unlink
        cur_owner->vf3(car, car.dir);              // 0x4115c0 vtable+0xc -> 0x40cbd0, src lane_attach
        prev_owner->vf4(car, prev.dir);            // 0x4115d2 vtable+0x10 -> 0x40e710, src lane_detach
        car.k12c (+0x12c) = 0;                     // 0x4115e4
    }
}
```
Callers' args are asm-checked: vf3 is `PUSH cur.idx; PUSH car` (idx unused by 0x40cbd0),
vf4 is `PUSH prev.idx; PUSH car` (dir unused by 0x40e710). Steps 7 and 8 (0x4115f1..)
are later cards.

### helper 0x40cbd0 ctLane vf3 (append branch used by the hand-over)
`if (car.lane != lane) { car.lane = lane; append car at lane tail; }` — the lane's intrusive
list tail (lane+0xc) and head (lane+8) when empty. src `lane_attach` deliberately does not
touch `lane.cars` (it assumes `lane_insert` did); the hand-over must append itself.
```
if (car+0xc != lane) { car+0xc = lane; car+4 = lane+0xc; car+8 = 0;
                       (lane+0xc ? lane+0xc->next : lane+8) = car; lane+0xc = car; }
```
`lane_unlink` cleared car.lane first, so this branch is always taken at the hand-over.
### helper 0x40ca40 TrafficCar__unlinkLane
Intrusive doubly-linked unlink from (lane+8 head, lane+0xc tail, car+4/+8), then
`car+4 = car+8 = car+0xc = 0`. Already ported as src `lane_unlink` (vector erase), unchanged.
### helper 0x40e710 ctLane vf4 (called by step 6)
```
lane.kc8 (+0xc8) -= car.k24 + car.k14 + car.k20; if (< 0) = 0
f = lane+8 (new first car); if (f) { TrafficCar__step(f); schedule timeToLaneEnd(f) if lane+0xac == 0 }
if (lane.k34 (+0x34) == -1) car.k124 (+0x124) = 0
```
Already ported as src `lane_detach`, unchanged (it now reaches the real `car_step`).
### helper 0x40d990 TrafficCar__timeToLaneEnd (caller)
`TrafficCar__step(car); stop = car.next_lane_b ? 0 : g.cross_stop_offset;
v = max(targetSpeed(car), 10); return max(0, ((pos - k14) - stop) * (2/v))`.
src `car_time_to_lane_end` must call the new step first (its shortcut comment is stale).
### helper 0x40f4e0 ctLane__activeStep (caller)
For each car of a live lane with `car.next_lane_b == 0`: `TrafficCar__step(car)` then the
next-lane move 0x40e460 (still a shortcut). src `lane_active_step` calls it in the loop.

Constants (all read from the exe bytes): 0x593580=0.0f, 0x593590=0.5f, 0x593598=1.0f,
0x5935ac=-1.0f, 0x593704=0.3f, 0x593718=1.5749999f (0x3fc99999), 0x593728=2.0f,
0x593738=5.0f, 0x593744=-0.5f, 0x593748=-3.5f, 0x5935c0=0x7fffffff (fabs mask).

## Files
- edit: `src/game/traffic.h` — `struct Car` next-ref shape (the exe uses 3 dwords
  {owner, path, idx} for both cur and next; src only stores ids):
  - `uint32_t next_lane_a = 0;` stays (+0xb8 next ref owner / lane index).
  - `uint32_t next_lane_b = 0;` → `const Path *next_lane_b = nullptr;` (+0xbc next ref
    path; the src comment cites +0xbc, keep the name; for a lane ref it is `&lane.path`).
  - `float kc0 = 0;` → `int32_t kc0 = 0;` (+0xc0 next ref idx; notes/37 §3: really an int,
    -1 = lane, unused before this card).
  - add `float k12c = 0.0f;  // +0x12c lane-change rate (step 6 zeroes it, step 4 reads it)`.
- edit: `src/game/traffic.cpp`
  - new `void car_step(Graph &g, uint32_t ci)` implementing the slice above; place it after
    `lane_detach` and forward-declare it before `car_time_to_lane_end` (mutual recursion:
    step 6 -> lane_attach/lane_detach -> car_time_to_lane_end -> car_step; the e0 gate makes
    the same car return at once, and the exe steps the new first car there too).
  - `car_time_to_lane_end`: change to `(Graph &g, uint32_t ci)`, call `car_step(g, ci)` first
    (0x40d990), then the existing body on `g.cars[ci]`. Update the three callers
    (`lane_attach`, `lane_detach`, `set_lane_live`).
  - `lane_active_step`: replace the shortcut with `car_step(g, ci);` (keep the 0x40e460
    next-lane move as a shortcut).
  - `spawn_on_road` and `remove_car`: `car.next_lane_a = car.next_lane_b = 0;` does not
    compile with a pointer member — split into `car.next_lane_a = 0; car.next_lane_b = nullptr;`.
  - gate: the exe tests `car+0xc` (lane pointer); src lane ids are indices where 0 is a valid
    lane, so use the existing "not on a lane" convention `car.serial == 0` (INVENTED, same as
    `remove_car`).
  - cross path `+0x30` case (`kc0 >= 0` in step 1 / `dir >= 0` in step 6): src never puts a car
    on a cross (notes/37 §3), so use `path->length` (+0x38) for the lane case and leave a
    `// shortcut: the cross path +0x30 length has no src field; no src car leaves a lane.`
    Do not add cross support in this card.
  - steps 2/3/4/7/8: one `// shortcut: ... next cards` line at each skipped site.

## Done when
- every ported/edited function keeps its `// 0xADDR Name` comment; the local shapes for the
  deferred steps and the id-vs-pointer choices are marked `INVENTED:` / `shortcut:`
- `make -C src check` green; no new unit tests or hand-made expected values
- race/city harness numbers unchanged (notes/38: vmboot has no live lane and no traffic
  native runs, so `car_step` is only reached with the e0 gate closed)
- card frontmatter: status implemented, branch name
