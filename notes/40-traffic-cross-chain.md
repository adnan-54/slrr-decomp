# 40 — Traffic: live lanes in the harness, lane-end hand-over and the cross chain

Follow-up of notes/37 (per-car motion) and notes/39 (step 7/8). This note records what
cards 241-246 established, so a later session does not re-derive it. Everything here is
either from the exe (addresses cited) or explicitly marked INVENTED.

## 0. The harness finally runs traffic (card 243)

`vmboot --city` used to leave `Graph::live_lanes == 0`: a lane only goes live when its
road's map node is touched, and the shipped city's 15 touched map records contain no
`native road` node. The exe gets the touches from the per-frame render LOD walk
`0x45f180 -> 0x461980 -> type_road vf5 0x4d44d0 -> lane vf7 0x40eed0 -> ctLane__setLive
0x40f6d0`, and `0x40eed0` also stamps `lane+0xc0 = sim_time`; `ctLane__activeStep 0x40f4e0`
drops a lane once `sim_time - lane.kc0 > 0.5` (0x593590), so the exe must re-touch every
frame.

src has no render walk, so `Runtime::touch_road_nodes` (INVENTED harness) touches every
resident `native road` map object through the already-ported create path (`0x4fe6f0`
residency, `0x5195a0` vf3 -> `Traffic__bindObject 0x41a3e0`, keep-alive `0x4fe852` ->
`ctRoad__vf1 0x40f8e0` -> `Graph::keep_road_live` -> `set_lane_live 0x40f6d0`). **Only the
trigger is invented**; the effect path is the exe's. Result in `--city`: 755 road nodes
touched, 1,572/1,572 lanes live, 844 non-empty lanes with one heap entry each.

## 1. Lane end: `ctLane__laneEndMove 0x40e460` (card 241)

Two callers, both wired: `ctLane__timedEvent 0x40fcf0` at 0x40fd8c (arg `t = 0`) and
`ctLane__activeStep 0x40f4e0` at 0x40f5c3 (arg `t` from `((pos - k14) - stop) * 2/max(target,
10)`, clamped at 0). Third arg is the **value** `lane+0x38`.

```
if (car.k128) return false;                                  // mid lane change
if (!lane.node88[0] && !lane.node88[1]) -> 0x40e400 cross route
if (t > targetSpeed(car) / car.k38) return false;
if (!lane.live && t > 0) return false;
choose next0/next1 by room cap = kcc*k9c - kc8 (both fit: larger room wins, tie -> next1)
car_set_ref(car, chosen, &chosen.path, -1)
```

`0x40f950 car_set_ref`: live gate -> `0x40f5e0 setLive` -> `lane_unlink 0x40ca40` -> old
owner vf4 `0x40e710` -> set `path_lane/path/dir` (+ `kd0 = 0`, `prev_path = 0`) ->
`pos = len - k20` clamped to `k14` -> owner vf2 `0x411f00` -> vf3 `0x40cbd0 lane_attach`.
`0x411f00` caches the lane tail in `lane.blocker` and notes it as the car's leader.

**In the shipped city `node88[0]/[1]` are 0 for all 1,572 lanes** (city.trc), so every lane
end takes the cross branch.

## 2. Cross routing `0x40e400` (card 242)

```
if (cross.k40 > sim_time) return 0;                 // k40 = -1 in ctor 0x40c590, writer unknown
if (live_refs > 0) -> vf1c 0x4157a0 (routeLive)     // live_refs > 0 is the normal case:
if (live_refs < 0) -> vf20 0x4159d0 (routeDying)    // setLive 0x40f6d0 sets 1/-1 per lane
else               -> vf24 0x40e590 (idle)
```

- `0x40e3a0 lane_room(lane, need)`: empty -> 1; else `path.length - (tail.k20 + dist) >= need`
  with `dist` from `0x40daa0`.
- `0x4157a0` (548 B): `t > 5.2` -> 0; scans the cross paths (`+0x1c` stride 0x60, count
  `+0x20`), skipping blocked ones and ones whose `a` byte != link; priority gate `+2 >=
  (t >= 0.1 ? cross.k5c : 0)`; target lane `links[b].node` not halted and with room; score
  = `0x40e3a0` + (lane vf1 == `car.k160 == 1`) + 2*((`car.k40 & lane.ka0`) != 0); random
  tie pick (`rand & 0x7fff`); then `cross_claim_path 0x415b40` and `car_set_ref(car, cross,
  &paths[i].seg, i)` (through the 35-byte wrapper `0x40f900`).
- `0x4159d0` (routeDying): only paths whose two state counts are 0; score `kc8/kcc`
  (-100 when `car.k40 & lane.ka0`); strict minimum; `0x40f930` = `car_set_ref(car, lane,
  &lane.path, -1)`.
- `0x40e590` (idle): `link` indexes the cross links; same road skipped; gap test through
  `0x40daa0` against `path.length - tail.k20`; score `kc8/kcc` (-100 mask); strict minimum;
  `car_set_ref` onto the lane.
- `0x415b40 cross_claim_path` was already ported and is exact: `count += 10` unconditionally,
  `block = paths[i].c` on crossing paths, no other field (card 245). The only other writer of
  `count` is `ctCrossEq__step 0x415c40` (already ported).

## 3. Cross refs and the hand-over side (cards 242/244)

Ref tag (INVENTED, mirrors the exe's raw pointers): `next_lane_a` = lane index when
`kc0 < 0`, cross node id when `kc0 >= 0`; `next_lane_b` = `&lane.path` or the raw
`&paths[i].seg`; `kc0` = dir (-1 lane) or path index. Every consumer must branch on
`kc0`. Step 6 copies the next ref into the current ref.

- `0x413660 segment_pos_offset(seg, d, off)`: `0x4134e0 segment_pos` + `0x4131a0
  segment_side * off`; the cross ref's `dir` comes from `0x412f60 segment_dir`; the cross
  "path length" is the Segment's `+0x30`.
- `0x40cb60 cross vf3`: tail-append the car to the cross list (`cross+0x8` head,
  `cross+0xc` tail; node in the car at `+0x4/+0x8/+0xc`), then re-ref the car onto its exit
  lane `links[paths[i].b].node` (dir -1), then `0x410680 syncInstance(cross+0x44)`.
- `0x410680 syncInstance`: void in every branch; reparents the car's instance link
  (`+0x130`, target `T+0x311c`), parks/spawns through untraced services
  (`0x4ec210`, `0x417080`, `0x488080`, `0x41a500`) and the counter globals
  `0x5e64f4/f8/fc`. src keeps the branch skeleton with `shortcut:` marks; nothing binds
  `road+0x68`/`cross+0x44` yet, so it is a no-op in every src branch.

## 4. What is still missing (order matters)

1. **Proof that it runs**: the city probe still uses 3 frames and a one-shot touch, so no car
   reaches a lane end (card 246 re-touches every frame, runs up to 300 frames and checks a
   hand-over happened plus the position/heap invariants).
2. `Cross::k40` writer (gates `0x40e400`; ctor sets -1.0f, no writer found) — with -1 the
   gate never blocks, so the port is behaviour-equal until a writer is found.
3. cross vf4 `0x4883e0` (release/unlink half of `0x40cb60`) untraced.
4. `0x410680`'s four services and the counter globals.
5. `0x4107c0` step 1 still omits the Segment length for a `kc0 >= 0` next ref (comment
   updated in card 244; unobservable until a car actually carries a cross next ref).
6. `car_live_step` keeps a cross-owner car live only through its next ref; cross vf1
   `0x40e380` is not ported.
