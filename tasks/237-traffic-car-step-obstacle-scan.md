---
id: 237
status: implemented
branch: card/237
depends: []
---
# traffic: TrafficCar__step step-3 obstacle scan + obstacle-entry list node

Promotion note (orchestrator): the four UNRESOLVED items are read-side gaps of out-of-slice
producers (k104/k108/k114 writers, T+0xc8 event drain, lane+4 population outside step 8).
Implement the scan exactly as below with `shortcut:` comments for those reads; do not invent
writers. The clamp-vs-k128 dispute is settled by asm (0x410fc1 CMP flags, clamp, 0x410fe2 JNZ):
the clamp is unconditional and belongs here.

## Goal
Port the step-3 slice of `TrafficCar__step` 0x4107c0 (ECX = car) into card 235/236's
`car_step(Graph &, uint32_t)`: the three obstacle-list passes over the current lane's
list (`owner+4`), the next ref's list (`owner+4`, base shifted by the next length) and
the lane-change target's list (base `car+0xfc`), the free-body / on-path braking maths
with `k38`, `TrafficCar__checkObstacle` 0x40eff0 (head-inserts the car onto the
event-1 list `T+0xc8`), and the unconditional tail clamp
`free = min(max(free, 0), d0)`. Also add the obstacle-entry node layout (the exe's
lane obstacle list) and the list insert/remove helper that step 8 (0x4116fe) uses to
register/unregister the lane-change target; the helper is inlined in 0x4107c0, there
is no separate callee.

The lane-change **decision** in the same requested range (`0x410fc1..0x411105`:
0x40fde0 + the rand branch) is already carded as **task 238**. This card must not port
it. In particular the clamp is *unconditional* and belongs to this card (see
"Interaction with card 238" below); card 238's pseudocode currently puts it inside
`k128 == 0`.

## Exe evidence (cleaned pseudocode, renamed fields)
Field map (offsets cited by existing src comments unless marked NEW):
`+0x14 k14`, `+0x20 k20`, `+0x24 k24`, `+0x2c max_speed`, `+0x30 accel`,
`+0x38 k38` (decel 6.0), `+0x88 pos`, `+0x8c world[3]`, `+0x98 front_x`, `+0x9c front_z`,
`+0xa8 lateral`, `+0xb0 speed`, `+0xb8 next_lane_a (owner)`, `+0xbc next_lane_b (path)`,
`+0xc0 kc0`, `+0xc4 path_lane (owner)`, `+0xc8 path`, `+0xcc dir`, `+0xdc live`,
`+0x124 k124` (target lane), `+0x128 k128`. `d0` = step-1 free distance (EBP-0x5c),
`free` = step-2 gap (EBP-8), `ego_len` = step-2 `brake` value (EBP-0x1c),
`target` = `car_target_speed` result (0x40dc00, already `car_target_speed`),
`leader_speed` = step-2 best speed (starts -1.0f).

NEW car fields (traffic.h `Car`), exe offsets relative to the car:
```
event_next   +0x10           event-1 list next (0 = not listed)
ob_prev/ob_next +0xec/+0xf0  obstacle-list links (node+0 = prev, node+4 = next)
ob_owner     +0xf8           lane whose obstacle list holds this node
kfc          +0xfc           lane-change projection distance (-1 = none)
k100         +0x100          speed captured at obstacle registration
k104         +0x104          obstacle flag (UNRESOLVED writer)
k108[3]      +0x108..0x110   obstacle body position (UNRESOLVED writer)
k114[3]      +0x114..0x11c   obstacle body velocity (UNRESOLVED writer)
k120         +0x120          obstacle radius (= k20 written at registration)
```
The lane obstacle list head is `lane+0x04` (a *separate* list from the lane car list
at `lane+0x08/+0x0c`, src `Lane::cars`). For a car entry the list node starts at
`car+0xec`, so entry `+0x10 -> car.kfc`, `+0x14 -> car.k100`, `+0x18 -> car.k104`,
`+0x1c..0x30 -> car.k108/k114`, `+0x34 -> car.k120`.
Exe `Lane::obstacles` = `lane+4` (head) + the node links; src models it with a vector
of car indices, `INVENTED` (`Lane::cars` precedent).

### 0x40eff0 TrafficCar__checkObstacle (ECX = car, arg = obstacle entry; 0x40eff0..0x40f0a1)
```
dx = car.front_x - car.world[0];  dz = car.front_z - car.world[2];   // +0x98-0x8c, +0x9c-0x94
l  = sqrt(dx*dx + dz*dz);                                            // y dir is 0 (0x90-0x90)
if (l != 0.0f) { dx /= l; dz /= l; }
back = -car.speed;                                                   // 0x40f04c -[+0xb0]
rx = o.k108[0] - car.world[0]; ry = o.k108[1] - car.world[1]; rz = o.k108[2] - car.world[2];
vx = o.k114[0] - dx*back;      vy = o.k114[1];      vz = o.k114[2] - dz*back;
dot = vx*rx + vy*ry + vz*rz;
if (dot > 0.0f) return;
dist = sqrt(rx*rx + ry*ry + rz*rz);
spd  = sqrt(vx*vx + vy*vy + vz*vz);
k = -(((dot / (spd*dist)) * dist) / spd);          // keep this float order
rx += vx*k; ry += vy*k; rz += vz*k;
if (o.k120 + 1.0f < rx*rx + ry*ry + rz*rz) return; // entry +0x34
if (car.event_next == 0) {                         // +0x10
    car.event_next = g.event1;                     // T+0xc8 (0x5e652c+0xc8)
    g.event1 = ci + 1;
}
```

### 0x4107c0 TrafficCar__step, step 3 (0x410a57..0x410fe2)
Each pass picks the list and `base_pos`; a missing owner/path skips the pass. Pass 0
has no null test in the exe (it dereferences `car+0xc4`); src guards (`INVENTED`).
```
for (pass = 0; pass < 3; pass++) {
    const std::vector<uint32_t> *obs;
    if (pass == 0) {
        if (car.path_lane >= g.lanes.size()) continue;  // INVENTED bounds (exe pointer)
        base_pos = car.pos;                         // +0x88
        obs      = &g.lanes[car.path_lane].obstacles;   // +0xc4 owner
    } else if (pass == 1) {
        if (!car.next_lane_b) continue;             // +0xbc
        // shortcut: kc0 >= 0 is a cross path (length at +0x30, owner is a cross);
        // src has no car on a cross (same shortcut as step 1)
        if (car.kc0 >= 0) continue;
        base_pos = car.pos + car.next_lane_b->length;   // +0x38
        obs      = &g.lanes[car.next_lane_a].obstacles; // +0xb8 owner
    } else {
        if (!car.k124) continue;                    // +0x124
        if (!(car.kfc > -0.5f)) continue;           // 0x593744
        base_pos = car.kfc;                         // +0xfc
        // src k124 stores a raw side-lane node id (lane_attach 0x40cbd0, task 238),
        // the exe a lane pointer; resolve it like 0x40fe50 does.
        const Lane *tl = g.lane_by_node(car.k124);
        if (!tl) continue;                          // INVENTED bounds (exe pointer)
        obs = &tl->obstacles;
    }
    for (uint32_t oi : *obs) {                      // exe lane+4 head, node+4 next
        const Car &o = g.cars[oi];
        float s = o.kfc;                            // entry +0x10
        if (s <= -0.5f) {                           // free body
            float p[3], dir[3];
            car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);  // 0x4133f0
            car_ref_dir(car.path, car.dir, car.pos, dir);                    // 0x412ed0
            float e0 = p[0]-o.k108[0], e1 = p[1]-o.k108[1], e2 = p[2]-o.k108[2];
            float dot = dir[0]*e0 + dir[1]*e1 + dir[2]*e2;
            if (!(dot > 0.0f)) continue;
            float lat = -dir[2]*e0 + dir[1]*e1 + dir[0]*e2;
            if (fabsf(lat) - o.k120 >= 2.0f) continue;             // 0x593728
            float d = dot - (o.k120 + car.k14 + ego_len);
            float rear = ego_len;
            if (d < 0.0f) { rear = (-ego_len <= d) ? d + ego_len : 0.0f; d = 0.0f; }
            if (free <= d) continue;
            float closing = -(dir[0]*o.k114[0] + dir[1]*o.k114[1] + dir[2]*o.k114[2]);
            float brake_d = d;
            if (car.speed <= 1.0f || 1e-5f > d || closing < 0.0f ||
                (closing*closing - car.speed*car.speed) / (d + d) >= -car.k38) {
                leader_speed = fmaxf(closing, 0.0f);
            } else {
                float bd = (car.speed <= car.k38*dt)
                         ? car.speed*car.speed / (car.k38 + car.k38)
                         : car.speed*dt - dt*dt*car.k38*0.5f;
                float v2 = car.speed - car.k38*dt;
                float bd_raw = bd;
                brake_d = fmaxf(bd, d);
                rear += d;
                leader_speed = fmaxf(v2, 0.0f);
                if (rear <= bd_raw && closing <= v2)
                    car_check_obstacle(g, ci, o);      // 0x40eff0, unconditional for free bodies
            }
            free = fminf(free, brake_d);
        } else {                                   // on-path
            if (base_pos <= s) continue;           // 0x410b3e JBE
            float d = base_pos - (o.k120 + s + car.k14 + ego_len);
            float rear = ego_len;
            if (d < 0.0f) { rear = (-ego_len <= d) ? d + ego_len : 0.0f; d = 0.0f; }
            if (free <= d) continue;               // 0x410b82 JBE
            float os = o.k100;                     // entry +0x14
            float brake_d = d;
            if (car.speed <= 1.0f || 1e-5f > d || os < 0.0f ||
                (os*os - car.speed*car.speed) / (d + d) >= -car.k38) {
                leader_speed = fmaxf(os, 0.0f);
            } else {
                float bd = (car.speed <= car.k38*dt)
                         ? car.speed*car.speed / (car.k38 + car.k38)
                         : car.speed*dt - dt*dt*car.k38*0.5f;
                float v2 = car.speed - car.k38*dt;
                float bd_raw = bd;
                brake_d = fmaxf(bd, d);
                rear += d;
                leader_speed = fmaxf(v2, 0.0f);
                if (rear <= bd_raw && os <= v2) {
                    if (o.k104) {                  // 0x410c63 flag: 0 -> no event, use rear
                        car_check_obstacle(g, ci, o);
                    } else {
                        brake_d = rear;
                        leader_speed = fmaxf(os, 0.0f);
                    }
                }
            }
            free = fminf(free, brake_d);
        }
    }
}
free = fminf(fmaxf(free, 0.0f), d0);               // 0x410fc8..0x410fdd, unconditional
```
Notes: the `2.0`/`1.0` are the exe's literal comparisons (`radius+1 < dist²` in
0x40eff0, `|lat|-radius < 2` in the scan); keep them as is, no dimensional cleanup.
Comparison direction is exactly `<=`/`<` as marked (the exe uses `JBE`/`JA`).

### 0x4107c0 inlined obstacle-list insert/remove (step 8, 0x411801..0x411859 / 0x411861..0x4118b0)
No separate callee (procedure_callees of 0x4107c0 lists no list helper; the code is
inlined). Node layout as above; `node+0 = car+0xec` (prev), `node+4 = car+0xf0` (next),
`node+0xc = car+0xf8` (owner lane), head is `owner+4`.
```
unlink(node):
  owner = car.ob_owner;
  if (owner != 0) {
      if (car.ob_prev != 0) car.ob_prev.ob_next = car.ob_next;
      else                  owner.head(+4) = car.ob_next;
      if (car.ob_next != 0) car.ob_next.ob_prev = car.ob_prev;
      car.ob_owner = 0;
  }
  car.ob_next = 0; car.ob_prev = 0;
insert(node, target):                              // 0x411838..0x411859, head insert
  if (target.head != 0) target.head.ob_prev = node;
  node.ob_prev = 0;
  node.ob_next = target.head;
  target.head = node;
  node.ob_owner = target;
```
Step 8 calls it only after `0x414050` (src `path_project_strict`) succeeded on the
target path (`car+0x124` + 0x3c), writes `car.k100 = car.speed` (0x4117b6) and
`car.k120 = car.k20` (0x4117fb), and skips when `car.ob_owner == car.k124`
(0x4117e6). That call site is a later card; this card only adds the two helpers and
their field layout. src form (`INVENTED`: vector instead of intrusive links):
```
void obstacle_unlink(Graph &g, uint32_t ci);            // 0x41180d..0x411832 / 0x411868..0x4118b0
void obstacle_link(Graph &g, uint32_t li, uint32_t ci); // 0x411801..0x411859
```
`ob_owner` sentinel: a lane index may be 0, so use `0xffffffffu` (`kNoneLane`) for
"none" (`INVENTED`; the exe uses a null pointer).

## Helpers already in src (do not re-port)
- `car_target_speed` (0x40dc00) -> `target`.
- `car_ref_pos_offset` (0x4133f0 lane / 0x413660 cross) and `car_ref_dir`
  (0x412ed0 lane / 0x412f60 cross) already pick on `car.dir` and return zeros for the
  cross case; reuse them for the free-body projection.
- `msvc_rand` (0x571110, already 15-bit) is needed only by card 238 here.

## Constants (read from the exe, keep hex)
`0x593744 = -0.5f`, `0x5936f8 = 1e-5f` (0x3727c5ac), `0x593598 = 1.0f`,
`0x593728 = 2.0f`, `0x593590 = 0.5f`, `0x593708 = 0.8f`, `0x59370c = 0.9f`,
`0x5935ac = -1.0f`, `0x593580 = 0.0f`, `0x5935c0 = 0x7fffffff` abs mask,
`0x593570 = 0x80000000` sign mask, `0x593584 = 0.001f`, `0x5935d0 = 0.01f`,
`0x5936fc = 3.0517578e-05f`, `0x59371c = 1.75f`, `0x593718 = 1.5749999f`.

## Interaction with card 238
Card 238 (`tasks/238-traffic-car-lane-change.md`) ports `0x410fc1..0x411105`
(`0x40fde0` + decision + rand). This card must land first and owns the unconditional
clamp `free = min(max(free,0), d0)` (0x410fc8..0x410fdd); card 238 must not repeat it
(its evidence currently shows it inside `if (k128 == 0)`). The exe tests k128 at
0x410fc1 and only uses the flags after the clamp (`0x410fe2 JNZ`), so the clamp is
outside the `k128 == 0` branch. Card 238 must also depend on this card (its
`leader_speed` is the scan output).

## Files
- edit: `src/game/traffic.h`
  - `Car`: new `event_next`, `ob_prev`, `ob_next`, `ob_owner`, `kfc`, `k100`, `k104`,
    `k108[3]`, `k114[3]`, `k120` (comments with the offsets above; `UNRESOLVED` on
    `k104`/`k108`/`k114` writers).
  - `Lane`: new `std::vector<uint32_t> obstacles;` (exe `lane+4`; `INVENTED` vector).
  - `Graph`: new `uint32_t event1 = kLiveEnd;` (exe `+0xc8`, event-1 list head; drain
    not ported). `constexpr uint32_t kNoneLane = 0xffffffffu;`.
- edit: `src/game/traffic.cpp`
  - before `car_step`: `car_check_obstacle` (`// 0x40eff0 TrafficCar__checkObstacle`),
    `car_scan_obstacles` (`// 0x4107c0 TrafficCar__step step 3`), `obstacle_unlink`
    (`// 0x41180d..0x4118b0, inlined in TrafficCar__step`), `obstacle_link`
    (`// 0x411801..0x411859, inlined in TrafficCar__step`).
  - `car_step`: replace the "steps 3/4 ... later cards" shortcut comment with a call
    to `car_scan_obstacles(g, ci, dt, d0, brake, free, leader_speed)`, then the clamp,
    then `// card 238: lane-change decision 0x410fc1..0x411105`. Keep the step-2
    target-lane branch shortcut (k124 stays 0 in src).

## Done when
- each ported function has a `// 0xADDR Name` comment; the cross-ref, `k104`/`k108`/
  `k114` writer and event-drain gaps are `shortcut:`/`INVENTED:` as above; no
  `UNRESOLVED` left except the writer offsets this card does not touch
- `make -C src check` green; no new unit tests or hand-made expected values
- race/city numbers unchanged or the change reported (no lane goes live in the current
  vmboot, notes/37 §0)
- card frontmatter: status implemented, branch name
