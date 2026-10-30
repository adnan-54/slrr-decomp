# 37 — Traffic__step walks, live-car step, per-car step map; `--city` keep-alive observed (RE 2026-10-26)

Follow-up of notes/28 §3, notes/30, notes/36 §5. Decompiled: 0x41b290, 0x415c40, 0x415d60,
0x415b40, 0x415b00, 0x40f350 (+asm), 0x4118f0, 0x4102d0, 0x4107c0, 0x40eff0, 0x40fde0, 0x40d2c0,
0x40daa0, 0x414050, 0x40d970, 0x4154b0, 0x415700, 0x40e380, 0x410680, 0x417080.
New names (analysis/names.json): ctCrossEq__step 0x415c40, ctCrossLight__step 0x415d60,
ctCross__claimPath 0x415b40, ctCross__pathBlocked 0x415b00, TrafficCar__pushInstance 0x4118f0,
TrafficCar__advance 0x4102d0, TrafficCar__checkObstacle 0x40eff0, TrafficCar__startLaneChange
0x40fde0, TrafficCar__noteLeader 0x40d2c0, TrafficCar__distanceOnRef 0x40daa0, ctPath__projectXZ
0x414050, CarPathRef__crossObject 0x40d970.

## 0. Observation: no lane goes live in `vmboot --city` (cards 230/231)
Temporary stderr prints (reverted, not committed) in `Graph::step`, `walk_map_nodes` and
`Runtime::map_node_touched`, capped run `(ulimit -v 1500000; timeout 300 src/build/vmboot --city)`
from the repo root, 63/63 checks pass:
- `Graph::step` runs every frame after Valocity entry (sim_time 288.76 -> 475.0 s); `live_lanes = 0`,
  `live_count = 0`, heap 878 entries for the whole run.
- `walk_map_nodes` is called (statics = 2 in the city: the city ground with 58,210 map nodes and a
  5,148-node second ground), but **every dynamic root is asleep**: the 3 awake roots are
  inv_mass 0 / flags 0x20 bodies (triggers), and all car bodies (~48, 1.2-2.1 t, 7 P records each) sit
  in `World::sleeping` with `asleep = 1` and `obj_kind = 0` (default ±10 box, never refreshed because
  `update_object_boxes` only sees awake states) at about (0, 0.5, 0).
- So the `!b.asleep` / `is_dynamic` filter rejects everything: `map_node_touched` is never called,
  no road object is created, `keep_road_live` never runs. Hook wiring is fine; road nodes are
  irrelevant here (the walk never reaches the node test).
- Why asleep: vmboot never drives in the city (no throttle/steer input after cmd 109), and every car
  body stays at the origin-ish garage/showroom placement. Whether the exe wakes the player car on
  City entry (Track/City `setMatrix` + `command("reset")`, or the player controller attach) is **not
  traced**; next RE question for the keep-alive to fire headless: what wakes the player car root
  in the exe (0x4f9750 needs an awake type-2 child) and whether src's sleep test matches the exe's.
  A harness throttle input in `--city` would be the cheap way to observe lanes going live.

## 1. 0x41b290 Traffic__step(t) (ECX = T), complete
```
g_traffic (0x5e652c) = T; T+0x84 sim_time = t
if (T+0x3118 /*pseudo-car of parked instances, notes/29 §5*/)
    pc+0xe8 = pc+0xe4 = pc+0xe0 = t + 1.0     // its step/advance/push gates never pass
while (T+4 heap count && heap[0].time < t) { ctLane__timedEvent 0x40fcf0(); TrafficHeap__pop 0x41a810(); }
walk (unlink on false, sentinel T):
   T+0x90 cross_eq    ctCrossEq__step    0x415c40, link +0x54
   T+0x94 cross_light ctCrossLight__step 0x415d60, link +0x60
   T+0x88 live cars   TrafficCar__liveStep 0x40f350, link +0x44
   T+0x8c lanes       ctLane__activeStep 0x40f4e0, link +0xa4   (src: card 220)
drain T+0xc8 (link car+0x10, sentinel T): pop head, car+0x10 = 0,
   if (car+0x138 inst) Game__queueEvent(car+0x130, 0, 1 /*code*/, 0, 0)
```
Unlink = `*p = node.next; node.next = 0` (same as src's lane walk). The T+0xc8 list is filled only by
`TrafficCar__checkObstacle` 0x40eff0 (inside TrafficCar__step): head insert when `car+0x10 == 0`.
So the drain is dead until 0x4107c0 is ported. The pseudo-car (T+0x3118) is set only by
Traffic__parkInstance 0x417080 (getInfo 0x38), not in src: the prologue stores are a no-op there.

## 2. Cross path state (both cross kinds), stride 8 at cross+0x58, count = paths (+0x20)
`state[i] = {int32 count (+0), int32 block (+4)}`; path record stride 0x60: byte +0 = entry link
(src `CrossPath::a`), byte +1 = exit link (`b`), ushort +2 = priority (`c`).
### 0x415b40 ctCross__claimPath(i) (ECX = cross)
```
if (state[i].count == 0) {
    a = path[i].a; b = path[i].b; prio = path[i].c
    for j in paths:
        if (state[j].block < prio) {                          // signed compare
            aj = path[j].a; bj = path[j].b
            if (aj != a) {
                if (bj == b) state[j].block = prio
                else {
                    inA = (a < b) ? (a < aj && aj < b) : (a < aj || aj < b)   // cyclic (a,b)
                    inB = (a < b) ? (a < bj && bj < b) : (a < bj || bj < b)
                    if (inA != inB) state[j].block = prio                     // crossing paths
                }
            }
        }
}
state[i].count += 10
```
(all bytes unsigned.)
### 0x415b00 ctCross__pathBlocked(i)
`if (state[i].block != 0) return true; claimPath(i); return false;`

### 0x415c40 ctCrossEq__step (ECX = cross) -> bool keep listed
```
for i: state[i].block = 0
r = cross+0x38 (live_refs)
if (r < 0) {                                  // going inactive (set_lane_live off left -1)
    any = false
    for i:
        if (state[i].count > 4) state[i].count = 5
        state[i].count -= 1
        if (state[i].count < 1) state[i].count = 0
        else { any = true; for j: if (path[i].b == path[j].b) state[j].block = 0x3ea }
    if (any) return true
    cross+0x38 = 0; return false
}
if (r > 0) {
    for i:
        if (state[i].count > 4) state[i].count = 5
        c = state[i].count; state[i].count = 0
        if (c > 1) { claimPath(i); state[i].count = c - 1 }      // claim adds 10, then overwritten
    return true
}
return false                                  // r == 0
```
### 0x415d60 ctCrossLight__step (ECX = light cross)
Light fields (ctor 0x4154b0 zeroes +0x60 link, +0x64, +0x68, +0x70; init 0x415700 sets +0x6c =
sim_time, +0x70 per-link phase, +0x74 phase count, +0x5c = 0): **+0x64 remain (float), +0x68
current phase (int)**, +0x6c phase_time, +0x70 phase[link], +0x74 phase_count. src lacks +0x64/+0x68.
```
ctCrossEq__step()                               // same counters/blocks (+ may zero refs)
r = cross+0x38; if (r < 1) return r != 0
if (remain <= 0) {
    a = 0.1, b = 0.1, na = 0, nb = 0
    for k in links (+0x14 stride 8, count +0x18):
        lane = links[k].node; f = lane+0xc8 / lane+0xcc       // src Lane::kc8 / Lane::kcc
        if (phase[k] == cur) { a += f; na++ } else { b += f; nb++ }
    if (na > 1) a /= na; if (nb > 1) b /= nb
    if ((a + a) / b + phase_time < T.sim_time) {
        phase_time = sim_time; remain = 4.0
        Game__queueEvent(cross+0x44, 0, 0x10, "change_lamp", 0)
    }
} else {
    remain -= sim_time - phase_time; phase_time = sim_time
    if (remain <= 0) { remain = 0; cur = (cur + 1) % phase_count
                       Game__queueEvent(cross+0x44, 0, 0x10, "change_lamp", 0) }
}
if (remain <= 0) { for i in paths: if (phase[path[i].a] != cur) state[i].block = 0x3ec }
else             { for i in paths: state[i].block = 0x3eb }      // amber: everything blocked
return cross+0x38 != 0
```
`cross+0x44` is the ResRefLink of the bound traffic-light object (src `Cross::object`); the event
goes to its scripted instance (render/script side; no sink in src).

## 3. 0x40f350 TrafficCar__liveStep (ECX = car) -> bool keep listed (asm-checked)
Car refs are 3 dwords {owner, path, idx}: next = +0xb8/+0xbc/+0xc0, current = +0xc4/+0xc8/+0xcc
(idx -1 = lane owner, >= 0 = path index of a cross owner), previous = +0xd0/+0xd4/+0xd8.
src: `next_lane_a/next_lane_b/kc0` = next ref (Car::kc0 is really an int idx, unused),
`path_lane/path/dir` = current ref (lane index; src never puts a car on a cross).
```
if (!car+0xdc live) { TrafficCar__pushInstance 0x4118f0(); return false }
if (next.path && next.idx >= 0) {                  // ECX = next.owner (cross)
    if (car+0x88 pos <= speed(+0xb0)^2 / (2*accel(+0x30)) + car+0x14) claimPath(next.idx)
    else if (pathBlocked(next.idx)) next.path (+0xbc) = 0
}
if (cur.path && cur.idx >= 0) claimPath(cur.idx)   // ECX = cur.owner (cross)
if (!next.path && !cur.owner->vf1() && live) {    // lane vf1 0x40e390 = +0xa8 live; cross vf1 0x40e380 = refs != 0
    live_count (0x5e6514)--
    if (!car+0x14c && car+0x138 && car+0xdd) queue(T+0x3130, 0x10, "car_rem %d %d", car+0x138, car)
    car+0xdc = 0; if (car+0x128) car+0x128 = 0
}
TrafficCar__pushInstance 0x4118f0()
return true                                       // tail 0x40f481..0x40f4c2 is dead (NOP)
```
Note the second `%d` is the car pointer (src would pass an INVENTED handle).

## 4. Per-car motion entry points (not ported; map only)
Call chain for a live car each frame: liveStep -> **0x4118f0 TrafficCar__pushInstance**
(gate `car+0xc && live`, `car+0xe8 < sim_time`): first time with an instance reads getInfo
0x40/0x46/0x41 (length/width/bbox) into +0x18/+0x1c/+0x14/+0x20/+0x28; then **0x4102d0
TrafficCar__advance** (gate `car+0xe4 < sim_time`): **0x4107c0 TrafficCar__step**, then world pos
+0x8c..+0x94 from `CarPathRef__posOffset(pos, lateral)`, heading +0xa0/+0xa4, front point +0x98/+0x9c
(global 0x5e6518 = mode 0/1/2 of the front-point smoothing); then, with an instance, packs
"fffffffffffffffi" (pos, dir, up, vel, steer, accel, k158, cross object) and sends it (0x4eb3f0).

0x4107c0 TrafficCar__step (ECX = car, gate `car+0xc` and `car+0xe0 < sim_time`, dt = t - e0), in order:
1. **free distance** `d0 = pos - k14 - (next ref? next length (+0x30 seg / +0x38 path) : T+0x3140)`,
   `v = TrafficCar__targetSpeed 0x40dc00` (src `car_target_speed`), brake gap
   `g = (max(speed-2,0)*0.5+1)*k24`.
2. **leader slots** car+0x48..+0x84 (4 x 16 bytes {serial, car*, flag, dist}): 0x40d2c0
   TrafficCar__noteLeader for the previous car on the lane (`car+4`) or the next cross path's
   holder (`next.owner->vf0(next.idx)`), and for the first car ahead on the lane-change target
   (+0x124); each slot refreshed with 0x40daa0 TrafficCar__distanceOnRef (recursively steps the
   leader first), invalid slots cleared, sorted 0x40c270, nearest gap -> `d`, leader speed -> v_l.
3. **obstacle scan**, 3 passes over obstacle lists (`cur.owner+4`, then `next.owner+4` shifted by
   next length, then lane-change target +0x124 at +0xfc): entries with +0x10 <= -0.5 are free
   bodies (pos +0x1c, vel +0x28, radius +0x34) projected on the path dir; others are on-path
   (pos +0x10, speed +0x14, flag +0x18). Braking distance with `k38` decel, may call 0x40eff0
   TrafficCar__checkObstacle (pushes the car onto T+0xc8 => event 1 to its instance).
4. **lane change decision** (car+0x128 == 0): 0x40fde0 TrafficCar__startLaneChange when slowed
   (`d < 0.9*d0 && v_l < speed && ...`) or randomly (`crt_rand`, rate +0x12c smoothed toward
   0.01/0.001; uses rand15 => consumes the MSVC rand stream!) ; `cur.owner vf5` picks the side lane,
   +0x128 = ±1.
5. **speed integration**: lateral +0xa8 clamped to ±(1.575 - k28) or steered toward
   `+0x128 * -3.5`; speed +0xb0 / accel +0xb4 from accel +0x30 / decel +0x38; pos -= travelled.
6. **ref hand-over** when pos < 0 with a next ref: prev = cur, cur = next, next = 0,
   pos += new length, TrafficCar__unlinkLane 0x40ca40, `cur.owner vf3`, `prev.owner vf4`, +0x12c = 0.
7. **lane-change completion** (+0x128 != 0): ctPath__projectXZ 0x414050 on the target path; if
   |lat| < 1.75 swap cur <-> +0x124, unlinkLane + ctLane__insertCar 0x40d1e0, old lane vf4.
8. **target-lane obstacle registration** (+0x124): project, +0xfc = distance or -1, intrusive link
   +0xec/+0xf0/+0xf8 into the target's obstacle list (+4); unlink when done; +0x124 = 0.
Other callers of 0x4107c0: 0x40d990 timeToLaneEnd, 0x40daa0, 0x40e150 lane init, 0x40e710 lane vf4,
0x40f4e0 activeStep, 0x40fcf0 timedEvent. Porting order suggestion: §4.1+5+6 (pure lane motion,
no leaders/obstacles/lane change) is the smallest slice that moves cars; leaders (2) next; 3/4/7/8
last. Every slice changes the rand stream only once 4 is in.

## 5. Cards
- 232 Cross eq/light steps + claim/blocked + Graph::step eq/light walks.
- 233 TrafficCar__liveStep (lane-owner subset) + Graph::step live-car walk.
- 237 step 3 obstacle scan (0x40eff0, three passes, obstacle list node/helpers).
- 238 step 4 decision + startLaneChange 0x40fde0 + lane vf5 0x40fe50 (notes/39).
- 239 steps 7/8 lane-change completion + target-lane registration + car_release unlink (notes/39).
- 240 Graph::step heap loop (0x40fcf0 timed event, 0x41a810 pop) and T+0xc8 event-1 drain.
Not carded: the timed-event pop 0x40fcf0 (needs 0x4107c0), the T+0xc8 drain (producer in 0x4107c0),
the pseudo-car prologue (parkInstance not ported), 0x4118f0/0x4102d0/0x4107c0.
**Numbers:** in today's vmboot no lane goes live (§0), so both cards are dead code there and race /
city numbers should not move; once a keep-alive fires (car awake) they will, and the light step's
"change_lamp" / liveStep's "car_rem" events are not delivered (shortcut).
