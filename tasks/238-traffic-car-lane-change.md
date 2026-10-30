---
id: 238
status: todo
branch:
review:
depends: [237]
---
# traffic: TrafficCar lane-change decision, startLaneChange and lane vf5 side-lane picker

## Goal
Port the lane-change decision block of `TrafficCar__step` 0x4107c0 (ECX = car) into
card 235/236's `car_step`: `TrafficCar__startLaneChange` 0x40fde0, the lane vf5
side-lane picker 0x40fe50 it calls, the decision conditions + random branch with the
`+0x12c` rate smoothing (`powf(0.01, dt)`, MSVC `rand()`). Helpers already in src:
`msvc_rand` (0x571110), `powf` (0x571143, used as `powf(0.01f, dt)`), `path_project_strict`
(0x414050), `path_pos_offset_at` (0x4133f0), `Graph::lane_by_node`.

Card 237 (obstacle scan) lands first: it owns the unconditional clamp
`free = min(max(free, 0), d0)` at 0x410fc8..0x410fdd (asm: `CMP [edi+0x128],0` at
0x410fc1 sets the flags, the clamp runs, `JNZ 0x41110a` at 0x410fe2 uses those flags,
so the clamp is outside the `k128 == 0` test). Do not repeat the clamp.

Partial-port state (do not "fix"): steps 7/8 (0x4115f1..) remain unported, and the
step-2 target-lane branch (`car+0x124`) stays a shortcut. Consequence: once this card
sets `car.k128` the change never completes or inserts into the target lane until the
steps 7/8 card lands; update the stale `// shortcut:` comments in `car_step`
accordingly. The decision block also consumes the shared MSVC rand stream: every live
car with `k128 == 0` and no next ref consumes one `rand()`, and vf5 consumes one more
when both side lanes are valid.

## Exe evidence (cleaned pseudocode, renamed fields)
Field map (all cited in src unless noted): `+0x14 k14`, `+0x20 k20`, `+0x24 k24`,
`+0x2c max_speed`, `+0x30 accel`, `+0x38 k38`, `+0x88 pos`, `+0xa8 lateral`,
`+0xb0 speed`, `+0xbc next_path (next_lane_b)`, `+0xdc live`, `+0x124 k124 target lane`,
`+0x128 k128`, `+0x12c k12c`. Lane: `+0x3c path`, `+0x34 k34`, `+0x90/+0x94` side lanes
(src `node88[2]`/`node88[3]`, raw node ids; exe pointers), `+0xc4 halted`,
`+0x08/+0x0c` car list head/tail (src `lane.cars`, ascending pos; head = smallest pos).
`/` = lane vtable at `+0x14` is 0x40fe50 (vtable dwords at 0x593688..: vf0 0x40dbc0,
vf1 0x40e390, vf3 0x40cbd0, vf4 0x40e710, vf5 0x40fe50, read from 0x59369c).

### 0x40fde0 TrafficCar__startLaneChange (ECX = car, one float arg, RET 0x4)
```
if (car.live == 0 || car.next_path != 0) return;
l = lane_vf5(g.lanes[car.path_lane], f, &car);   // 0x40fdf8 ECX = car+0xc4 owner
if (l == 0) return;
car.k124 = l;                                    // 0x40fe1b
car.k12c = 0.0f;                                 // 0x40fe27
car.k128 = (g.lanes[car.path_lane].node88[2] == l) ? -1 : 1;   // 0x40fe21..0x40fe3b:
                                                 // (lane+0x90 != l)*2 - 1
```
(`l` is a lane in the exe, a side-lane node id here; `k124` already stores node ids.)

### 0x40fe50 ctLane__vf5 (ECX = lane, args float f, Car *car; RET 0x8) -> lane or 0
```
need = car.k24 + car.k14 + 12.666666f;           // 0x593740
if (lane.k34 == 0xffffffffu) return 0;
if (!(need < car.pos)) return 0;
prev = car before ci in lane.cars;               // exe car+4; INVENTED list lookup
if (prev && !(prev.pos + prev.k20
              - prev.speed*prev.speed / prev.k38 < car.pos - need)) return 0;
if (lane.node88[2] == 0 && lane.node88[3] == 0) return 0;
p = posOffset(car, car.pos, car.lateral);        // 0x40dde0, below
for (i = 0; i < 2; i++) {                        // 0x40ff31..0x41019a
    Lane *side = lane_by_node(lane.node88[2 + i]);   // INVENTED bounds (exe pointer)
    if (!side || side.halted) continue;          // 0x40ff3b, lane+0xc4
    if (!path_project_strict(side.path, p[0], p[2], &d, &lat)) continue;
    if (!(need < d)) continue;
    /* side.cars, ascending pos: */
    c = last car with pos < d (scan from the tail);  // exe car+0xc walk via car+4
    nxt = c ? car after c : first car of side;       // exe lane+8 head default,
                                                     // else c+8; 0 = none
    float brake_car = 0;
    if (nxt) {                                       // 0x410001..0x410087
        inv = 1 / nxt.k38; v = nxt.speed;
        t = (v*v - car.speed*car.speed) * inv;
        if (!(0.0f <= t)) t = 0.0f;
        v -= 2.0f; if (!(0.0f < v)) v = 0.0f;
        brake_car = (nxt.pos - nxt.k14) - (v*0.5f + 1.0f)*nxt.k24 - t;
    }
    float brake_obs = 0;
    if (after)                                       // 0x410097..0x4100ea: obstacle
        brake_obs = (after.pos - after.radius)       // entry after d (list lane+4)
                  - max(0.0f, (after.speed*after.speed
                               - car.speed*car.speed) * (1.0f/6.0f));  // 0x593700
    if (c && !(c.pos + c.k20 - c.speed*c.speed/c.k38 < d - need
               && c.pos + c.k20 < d - f)) continue;              // 0x4100f6..0x41013a
    if (nxt && !(car.k20 + d < brake_car)) continue;             // 0x410143..0x410153
    if (before && !(d - max(need, f) > before.radius + before.pos)) continue;
    if (after  && !(car.k20 + d < brake_obs)) continue;          // 0x410176..0x410186
    ok[i] = 1;                                       // 0x410188
}
if (ok[0] && ok[1]) return lane.node88[2 + (msvc_rand() & 1)];   // 0x4101ae..0x4101ba
if (ok[0]) return lane.node88[2];                    // 0x4101c7
if (ok[1]) return lane.node88[3];                    // 0x4101d9
return 0;                                            // 0x4101e9
```
`before` / `after` = nearest obstacle entry of `side+4` with `pos < d` (largest pos;
`pos > -0.5` filter, 0x593744) / `pos > d` (smallest pos). Card 237 landed the list
service: `Lane::obstacles` (vector of car indices, LIFO head insert, `INVENTED`; exe
intrusive node at car+0xec) and the per-entry fields `Car::kfc` (+0x10 path distance,
-1 = off-path, excluded by the `> -0.5` filter), `Car::k100` (+0x14 captured speed) and
`Car::k120` (+0x34 radius). Port `before`/`after` as a scan of `side->obstacles`
comparing `g.cars[oi].kfc`; on a tie the exe keeps the first walk hit (head insert order
is LIFO, so the walk order is newest-first) - keep the first strict comparison winner.
When the list is empty `before = after = 0` and both obstacle guards pass. `side`'s path
projection and this list walk are the only consumers of the side lane; return 0 makes
startLaneChange a no-op (k124/k128/k12c untouched).

### 0x40dde0 CarPathRef__posOffset (ECX = &car.cur_ref {owner,path,dir}; out, d, off)
```
out = {0,0,0};
if (ref.path == 0) return out;
if (ref.dir >= 0) out = 0x413660(ref.path, d, off);   // cross Segment, see shortcut
else              out = 0x4133f0(ref.path, d, off);   // path_pos_offset_at(lane.path)
```
src port: `if (!car.path) p = {0,0,0}; else if (car.dir < 0) path_pos_offset_at(*car.path,
car.pos, car.lateral, p);` `// shortcut: cross ref dir >= 0 has no src Segment path; no
src car is on a cross (same as 0x4107c0 step 1).` vf5 uses `p[0]`, `p[2]`.

### 0x4107c0 decision slice (0x410fe2..0x411105), insert after card 237's clamp
The clamp `free = min(max(free, 0.0f), d0)` (0x410fc8..0x410fdd) is card 237's and is
unconditional; this slice starts at the `JNZ 0x41110a` on `car.k128 == 0`.
```
if (car.k128 == 0) {                             // 0x410fe2 JNZ 0x41110a
    if (free < d0 * 0.9f                           // 0x59370c
        && leader_speed < car.speed               // 0x410ff5..0x411005
        && ((leader_speed*leader_speed
             - (target*0.8f)*(target*0.8f)) * 0.5f)   // 0x593708, 0x593590
           / (free + 1.0f) < -car.accel)         // 0x411007..0x411047
        car_start_lane_change(g, ci,
            brake + free + car.k14 + 1.0f);      // 0x411049..0x41105e
    if (car.k128 != 0) goto step_5;              // 0x411068 JNZ 0x41110a
    if (car.live) {                              // 0x411075, else simple block
        float lo = (target < 0.01f) ? 0.01f : 0.001f;    // 0x593584/0x5935d0
        car.k12c = powf(0.01f, dt) * (car.k12c - lo) + lo;  // 0x4110a6..0x4110cb
        if (car.next_path == 0) {                // 0x4110d3
            float r = (float)(msvc_rand() & 0x7fff) * 3.0517578e-05f;  // 0x5936fc
            if (r < car.k12c)                    // 0x4110d5..0x4110f9 (strict)
                car_start_lane_change(g, ci, 0.0f);         // 0x411105
        }
    }
}
// fall through into the existing step 5 (car.live == 0 -> simple, else live)
```
The argument `brake + free + car.k14 + 1.0f` is asm-checked (XMM6 = brake `[EBP-0x1c]`,
XMM4 = free `[EBP-0x8]`, `+ k14`, `+ [0x593598]=1.0`). With the unported obstacle lists
empty, XMM6 keeps the step-2 brake value (reloaded 0x410f87 from `[EBP-0x1c]`).

Constants (read from the exe): 0x593584=0.001f, 0x5935d0=0.01f, 0x5935d4=0.1f,
0x593590=0.5f, 0x593598=1.0f, 0x5935ac=-1.0f, 0x593700=0.16666667f,
0x593704=0.3f, 0x593708=0.8f, 0x59370c=0.89999998f, 0x593718=1.5749999f,
0x593728=2.0f, 0x593738=5.0f, 0x593740=12.666666f, 0x593744=-0.5f,
0x593748=-3.5f, 0x5936fc=3.0517578e-05f (1/32768), 0x593570=sign mask 0x80000000.

## Files
- edit: `src/game/traffic.cpp`
  - anonymous namespace near `car_step`: `lane_side_pick` (`// 0x40fe50 ctLane__vf5`,
    returns a side-lane node id or 0), `car_pos_offset` (`// 0x40dde0`),
    `car_start_lane_change` (`// 0x40fde0`).
  - `car_step`: insert the decision slice after card 237's `car_scan_obstacles` call and
    clamp, before the step-5 comment; keep the step-2 `car.k124` target-branch shortcut
    (update its comment: k124 is now set by this card, the branch waits for the steps 7/8
    card). Do not add a clamp; card 237 already did it.
  - use `msvc_rand()`, `powf`, `path_project_strict`, `path_pos_offset_at`,
    `Graph::lane_by_node`.
- read only: `src/game/traffic.h` (`Car::k124/k128/k12c`, `Lane::node88/halted/path/k34`).

## Done when
- each ported function has a `// 0xADDR Name` comment; every gap (obstacle lists, cross
  ref offset, step-2 target-lane branch, steps 3/7/8, list-order lookups) is `shortcut:`
  or `INVENTED:`; no UNRESOLVED left for the ported pieces
- `make -C src check` green; no new unit tests or hand-made expected values
- race/city numbers unchanged or the change reported (no lane goes live in the current
  vmboot, notes/37 §0/notes/38); do not adjust harness expectations
- card frontmatter: status implemented, branch name
