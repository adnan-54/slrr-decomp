---
id: 252
status: implemented
branch: card/252
review:
depends: []
---
# traffic: car_target_speed (+0x124 lane-change target) — replace the `shortcut:`

## Goal
`car_target_speed` (0x40dc00) in `src/game/traffic.cpp` currently has a
`shortcut:` for the `car.k124` (+0x124) branch: it treats the target's vf6 value as
0.0 and halves `s`. In the exe +0x124 holds a Lane* whose vtable+0x18 (vf6) is read
and averaged with the already lane-limited speed. Replace the shortcut with the real
read: `s = (target_lane->k98 + s) * 0.5f`, where `target_lane = lane_by_node(k124)`
(src keeps +0x124 as a side-lane node id, exe as a Lane*). Only the `+0x124` body
changes; the current-owner and next-owner vf6 branches stay as they are.

## Exe evidence (cleaned pseudocode, renamed fields)
Field map used below (src comments cite these offsets): `max_speed` +0x2c,
`path_lane` +0xc4 (current ref owner), `path` +0xc8, `dir` +0xcc,
`next_lane_a` +0xb8, `next_lane_b` +0xbc, `kc0` +0xc0, `k124` +0x124,
`k128` +0x128, `k12c` +0x12c, Lane `k34` +0x34, Lane `path` +0x3c,
Lane `node88[2]/[3]` +0x90/+0x94, Lane `k98` +0x98.

### 0x40dc00 TrafficCar__targetSpeed (ECX = Car) — full branch order
```
s = car.max_speed;                                     // +0x2c
if (car.path_lane (+0xc4) != 0) {                      // current ref owner
    v = current_owner->vf6(car.dir);                   // vtable+0x18, arg +0xcc
    if (v < s) s = v;                                  // strict
}
f = 0.0f;
if (car.next_lane_b (+0xbc) == 0) {                    // no next ref path
    if (car.k124 (+0x124) != 0) {
        v = k124->vf6(0xffffffff);                     // vtable+0x18, arg -1
        f = 0.0f;
        s = (v + s) * 0.5f;                            // no min, no clamp
    }
} else {                                               // next ref exists
    f = car.max_speed;                                 // +0x2c
    if (car.next_lane_a (+0xb8) != 0) {                // next ref owner
        v = next_owner->vf6(car.kc0);                  // vtable+0x18, arg +0xc0
        if (v < f) {
            f = v;
            if (v <= s) f = s;
            return f;                                  // = max(v, s)
        }
    }
}
if (f <= s) f = s;
return f;                                              // = max(f, s)
```
Constants: `0xffffffff` (-1 argument), `0.5f` (the multiply). The `+0x124` average is
mutually exclusive with the next-owner selection: it is only reached when `+0xbc == 0`.

### which slot the +0x124 object uses
The branch is `(**(code **)(**(int **)(car+0x124) + 0x18))(0xffffffff)`: deref
+0x124 to its vtable, call slot at byte offset **+0x18 (slot 6, vf6)**. +0x124 is
always a Lane* (writers below), and the ctLane vtable at 0x593688 has +0x0c =
0x40cbd0 (vf3), +0x10 = 0x40e710 (vf4), +0x14 = 0x40fe50 (vf5), +0x18 = 0x40dd50
(vf6). So the called function is 0x40dd50, which ignores its argument and returns
`*(float *)(ECX + 0x98)` — the same value as the current/next lane refs.

### 0x40dd50 ctLane__vf6 (ECX = Lane)
```
return *(float *)(lane + 0x98);                        // argument unused
```

### 0x40fde0 TrafficCar__startLaneChange (ECX = Car) — writer of +0x124
```
if (car.live (+0xdc) != 0 && car.next_lane_b (+0xbc) == 0) {
    lane = car.path_lane (+0xc4);
    target = lane->vf5();                               // vtable+0x14 = 0x40fe50
    if (target != 0) {
        car.k124 (+0x124) = target;                     // a Lane*
        car.k12c (+0x12c) = 0;
        car.k128 (+0x128) = ((lane+0x90) != target) * 2 - 1;   // +1 or -1
    }
}
```
0x40fe50 (ctLane__vf5) returns one of the current lane's side lanes
(`lane+0x90`, `lane+0x94`; both pass => `lane+0x90 + (msvc_rand() & 1) * 4`) or 0.

### 0x40cbd0 ctLane__vf3 (lane_attach), tail — second writer
```
if (lane.k34 (+0x34) == -1)
    car[0x124] = lane+0x90 ? lane+0x90 : lane+0x94;     // a Lane*
```
So +0x124 can differ from the current ref owner (+0xc4): it is the lane the car
wants to move to (set by the lane-change decision), or after the step-7 swap
(0x4116d3 `k124 = old`) the lane it came from. Step 8 clears it
(`k124 != 0 && k128 == 0 && ob_owner == 0` -> 0, 0x4118d6), lane vf4 0x40e710
clears it on detach from a `k34 == -1` lane. src already carries k124 as a node id:
`lane_attach` stores `node88[2] ?: node88[3]` (traffic.cpp, 0x40cbd0), the decision
stores the node id from `lane_side_pick`, step 7 stores `lane_node_id(old)`.

## Files
- edit: src/game/traffic.cpp, `car_target_speed` (0x40dc00), the `+0x124` body
  inside `if (!car.next_lane_b) { ... }` — replace
  `s = (0.0f + s) * 0.5f;` and its `shortcut:` comment with:
  ```cpp
  // 0x40dc00: +0x124 target lane vf6 (0x40dd50 = lane+0x98), averaged with s.
  // INVENTED: null guard (the exe dereferences a non-null target lane pointer).
  const Lane *target = g.lane_by_node(car.k124);
  if (target) s = (target->k98 + s) * 0.5f;
  ```
  Keep the enclosing `if (car.k124)`, keep the current-owner and next-owner
  branches unchanged, and drop the old comment's "reachable since ... sets +0x124".
- read only: src/game/traffic.h (Car::k124, Lane::k98/node88, Graph::lane_by_node)

## Done when
- `car_target_speed` keeps its `// 0x40dc00 TrafficCar__targetSpeed` comment and the
  new vf6 read is cited; no `shortcut:` remains in that branch; invented guards are
  `INVENTED:`
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
