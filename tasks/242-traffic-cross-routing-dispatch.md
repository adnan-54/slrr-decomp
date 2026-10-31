---
id: 242
status: merged
branch: card/242
review: deepseek-pass
depends: [241]
---
# traffic: cross routing 0x40e400 + the three cross vf routes (0x4157a0/0x4159d0/0x40e590) + lane room 0x40e3a0

## Goal
Port the cross-routing branch that card 241 leaves as `// SERVICE MISSING:` in
`lane_end_move` (0x40e460): when a lane's two next-lane node ids are null, the exe
calls `0x40e400` with the road's cross object. Port `0x40e400` (pick a cross vf by the
sign of `live_refs`), the three route vfs `0x4157a0` (live_refs > 0, random pick among
the best-scored cross paths), `0x4159d0` (live_refs < 0, cheapest exit lane), `0x40e590`
(live_refs == 0, cheapest link lane with room) and their room helper `0x40e3a0`. The
lane targets (vf20/vf24) go through card 241's `car_set_ref`; the cross-path target
(vf1c, through `0x40f900` + `0x40f950` with a cross owner) needs the new tagged next-ref
plus small guards where src currently assumes every owner is a lane.

Verified from the shipped map (card 241): `lane+0x88`/`+0x8c` are 0 for all 1,572 city
lanes, so `lane_end_move` always reaches this branch once a lane is live. No harness
makes a lane live yet (CHECKPOINT), so this card must leave race 659.414 s / `--city 63`
unchanged; do not fake a harness and do not hardcode the map fact.

## Promotion note (orchestrator): promoted 2026-10-30, round 3
Card 243 merged: the city harness now touches every `native road` node, so **1,572/1,572
lanes are live** and `lane_end_move` reaches this branch every time a car hits a lane end —
the "unverifiable" objection is gone. The other two objections are handled by scope:
- `claimPath 0x415b40` is **already ported** (`cross_claim_path`; card 245 confirms the
  body matches: `count += 10` unconditionally, `block = paths[i].c` on crossing paths, no
  other field). `0x40f900`/`0x40f930` are the wrapper argument sets recorded in card 245:
  `cross_claim_path` then `car_set_ref(cdx, cross, &paths[i].seg, i)` and
  `car_set_ref(ci, lane, &lane.path, -1)`.
- the cross-owner ref (`dir >= 0`) is set here (tag below) but a car routed onto a cross
  path has no world position until **card 244** (`0x413660`) lands; that is accepted only
  because 244 follows immediately in this round — do not ship 242 without 244.
- the INVENTED tag stands: the exe itself stores a raw `&Segment` in the +0xbc path slot,
  so a tagged pointer in `Car::next_lane_b` is faithful; what matters is that nothing
  dereferences it as a `Path` while `kc0 >= 0`, and that every such site is guarded.

## Struct map (exe offset -> src field)

| exe | src |
|---|---|
| cross+0x38 | `Cross::live_refs` (int32; > 0 / < 0 / == 0 selects the vf) |
| cross+0x40 | **new** `Cross::k40` float (ctor 0x40c590 stores -1.0f; writer UNRESOLVED) |
| cross+0x5c | `Cross::cycle` (int; minimum priority gate when t >= 0.1) |
| cross+0x14 stride 8 (lane ptr + flag byte at +4) / +0x18 count | `Cross::links[i].node` (node id, resolve `lane_at`) / `.flag` / `links.size()` |
| cross+0x1c stride 0x60 / +0x20 count | `Cross::paths[i]`: `.a` = +0, `.b` = +1, `.c` = +2 (ushort) |
| path record +4 = Segment start, +4+0x30 = length | `&c.paths[i].seg` (src CrossPath keeps `k58/k5c` before `seg`, so never use record offsets) and `c.paths[i].seg.len` (Segment +0x30) |
| cross+0x58 stride 8 (`count` +0, `block` +4) | `Cross::path_state[i].count` / `.block` |
| car+0x14 / +0x20 / +0x40 / +0x160 | `Car::k14` / `k20` / `k40` (bitmask) / `behaviour` |
| car+0xb8 / +0xbc / +0xc0 | `Car::next_lane_a` / `next_lane_b` / `kc0` (see tag below) |
| car+0xc4 / +0xc8 / +0xcc | `Car::path_lane` / `path` / `dir` |
| lane+0x10 | `Lane::road` |
| lane+0x74 (path+0x38) / +0xc | `Lane::path.length` / `Lane::cars.back()` (exe intrusive tail) |
| lane+0x9c / +0xa0 / +0xa8 / +0xc4 | `Lane::k9c` (float after card 241) / `ka0` / `live` / `halted` |
| lane+0xc8 / +0xcc | `Lane::kc8` (sum of car lengths) / `kcc` (path length) |

**Next-ref tag (INVENTED):** the exe's car+0xb8 is the owner *pointer* and +0xc0 the
path index, `kc0 < 0` = lane owner, `kc0 >= 0` = cross owner (notes/37 §3). src keeps a
lane index in `next_lane_a` for a lane owner; for a cross owner store the **cross node
id** in `next_lane_a` and the path index in `kc0`, and `next_lane_b =
reinterpret_cast<const Path *>(&c.paths[i].seg)` (the exe's raw &segment pointer; with
`kc0 >= 0` nothing may dereference it as a `Path`). Same tag after step 6 copies it into
`path_lane`/`path`/`dir`.

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x40e400 ctCross__route (ECX = cross; args Car* car, float t, uint32_t link) — 94 bytes, RET 0xc
The three-arg call is set up before the branch; vf24 has 2 args and reads `link` as its
second. Return: the vf's result (`1` = a ref was set, `0` = no route).
```
if (cross->k40 > T->sim_time) return 0;              // +0x40; T+0x84 sim_time, COMISS/JBE
r = cross->live_refs;                                // +0x38
if (r > 0)  return cross->vf1c(car, t, link);        // vt+0x1c = 0x4157a0
if (r < 0)  return cross->vf20(car, t, link);        // vt+0x20 = 0x4159d0
return cross->vf24(car, link);                       // vt+0x24 = 0x40e590 (2 args, RET 8)
```
Vtables 0x59377c (eq) / 0x5937c0 (light) slots: +4 0x40e380 vf1 = `live_refs != 0`,
+8 0x477320 vf2 (empty, `RET 4`), +0xc 0x40cb60 vf3, +0x10 0x4883e0 vf4.

### 0x40e3a0 ctLane__room (ECX = lane; arg float need) — 85 bytes, RET 4
```
tail = lane->cars_tail;                              // +0xc (src lane.cars.back())
if (!tail) return 1;
if (!0x40daa0(ECX = tail, &lane->path, 0, &dist))    // src car_distance_on_ref(g, tail, &lane.path, nullptr, &dist)
    return 0;
room = lane->path.length - (tail->k20 + dist);       // +0x74, +0x20
return !(need > room);                               // COMISS need,room; JBE -> 1
```

### 0x4157a0 ctCross__routeLive (ECX = cross; args Car* car, float t, uint32_t link) — 548 bytes
```
if (t > 5.2f) return 0;                              // 0x593800 = 0x40a66666
need = car->k20 + car->k14;
behaviour1 = (car->behaviour == 1);                  // +0x160
prio = ((double)t >= 0.1) ? (int)cross->cycle : 0;   // +0x5c; 0x593568 = double 0.1
best = 0; count = 0;                                 // ints, candidates[128]
for (i = 0; i < cross->paths.size(); i++) {
    if (path_state[i].block != 0) continue;          // +4
    if (paths[i].a != link) continue;                // full 32-bit compare of the byte vs link
    if ((int)paths[i].c < prio) continue;            // ushort zero-extended, signed compare
    lane = lane_at(cross->links[paths[i].b].node);   // exe: links[b] pointer, unchecked
    if (!lane) continue;                             // INVENTED guard
    if (lane->halted) continue;                      // +0xc4 byte
    cap = lane->kcc * lane->k9c - lane->kc8;         // +0xcc * +0x9c - +0xc8, MULSS/SUBSS
    if (!(cap > need)) { path_state[i].block = 0x3e9; continue; }   // 0x3e9 = 1001
    score = lane_room(g, *lane, need) ? 1 : 0;       // 0x40e3a0
    if (lane->live == behaviour1) score += 1;        // lane vf1 0x40e390 = +0xa8
    if (((uint32_t)car->k40 & lane->ka0) != 0) score += 2;
    if (count == 0 || score > best) { best = score; cand[0] = i; count = 1; }
    else if (score == best) cand[count++] = i;       // ties are all kept
}
if (count != 0) {
    pick = cand[(msvc_rand() & 0x7fff) % count];     // 0x571110 & 0x7fff, signed IDIV
    cross_claim_path(cross, pick);                   // 0x415b40
    cross_path_ref(g, ci, cross_node, cross, pick);  // 0x40f900 (see below)
    return 1;
}
for (i = 0; i < cross->paths.size(); i++) {          // stale-block sweep, only when none scored
    if (paths[i].a != link) continue;
    if ((int)paths[i].c < prio) continue;
    if ((int)paths[i].c <= path_state[i].block + 5) continue;     // proceed only when block+5 < c
    lane = lane_at(cross->links[paths[i].b].node);
    if (!lane || lane->halted) continue;             // INVENTED null guard
    cross_claim_path(cross, i);                      // 0x415b40
}
return 0;
```
Score selection asm: 0x4158d7..0x4158ff (`CMP best,score` / `CMOVGE`, append at index
`(best >= score) ? count : 0`; `best = max(best, score)`), so a strictly higher score
resets the candidate array and a tie appends.

### 0x4157a0's cross ref: 0x40f900 ctCross__setPathRef (ECX = cross; args car, uint32_t i) — 35 bytes
```
0x40f950(ECX = car, owner = cross, path = &cross->paths[i] + 4 /* = &seg */, dir = i);
```
0x40f950 (card 241's `car_set_ref`, 0x40f950) with a cross owner and `dir >= 0`:
```
if (car->live == 0) {
    if (cross->live_refs != 0) car_set_live(car, 1);   // cross vf1 0x40e380 = +0x38 != 0
    if (car->live == 0) { /* immediate branch: unreachable from 0x4157a0 (live_refs > 0) */ }
}
// cross vf2 0x477320 is empty (RET 4)
car->next_lane_a = cross;  car->next_lane_b = &paths[i].seg;  car->kc0 = i;
return;
```

### 0x4159d0 ctCross__routeDying (ECX = cross; args Car* car, float t /*unused*/, uint32_t link) — 300 bytes
```
need = car->k20 + car->k14;
best = -1;                                          // int
for (i = 0; i < cross->paths.size(); i++) {
    if (path_state[i].block != 0) continue;          // +4
    if (path_state[i].count != 0) continue;          // +0 (both zero)
    if (paths[i].a != link) continue;
    lane = lane_at(cross->links[paths[i].b].node);
    if (!lane) continue;                             // INVENTED guard
    if (!lane_room(g, *lane, need)) continue;         // 0x40e3a0, before the halted check
    if (lane->halted) continue;                      // +0xc4
    score = lane->kc8 / lane->kcc;                   // DIVSS +0xc8/+0xcc
    if (((uint32_t)car->k40 & lane->ka0) != 0)
        score = (float)((double)score - 100.0);      // 0x593730 = double 100.0, CVTPS2PD/SUBSD/CVTPD2PS
    if (best == -1 || score < best_score) { best = i; best_score = score; }   // strict min, ties keep first
}
if (best == -1) return 0;
lane = lane_at(cross->links[paths[best].b].node);
car_set_ref(g, ci, lane index, &lane->path, -1);     // 0x40f930 -> 0x40f950 immediate/bottom
return 1;
```

### 0x40e590 ctCross__routeIdle (ECX = cross; args Car* car, uint32_t link) — 336 bytes, RET 8
```
need = car->k20 + car->k14;
if (link >= cross->links.size()) return 0;           // INVENTED: the exe indexes unchecked
l0 = lane_at(cross->links[link].node);
if (!l0) return 0;                                   // INVENTED
road0 = l0->road;                                    // +0x10
best = -1;
for (i = 0; i < cross->links.size(); i++) {
    if (cross->links[i].flag != 0) continue;         // +4 byte
    lane = lane_at(cross->links[i].node);
    if (!lane) continue;                             // INVENTED
    if (lane->road == road0) continue;               // same road as the incoming lane
    if (!lane->cars.empty()) {                       // +0xc tail pointer
        if (!0x40daa0(ECX = tail, &lane->path, 0, &dist)) continue;   // car_distance_on_ref
        if (need > lane->path.length - (tail->k20 + dist)) continue;  // COMISS need,room; JA skip
    }
    if (lane->halted) continue;                      // +0xc4
    score = lane->kc8 / lane->kcc;                   // +0xc8 / +0xcc
    if (((uint32_t)car->k40 & lane->ka0) != 0)
        score = (float)((double)score - 100.0);      // double 100.0 at 0x593730
    if (best == -1 || score < best_score) { best = i; best_score = score; }
}
if (best == -1) return 0;
lane = lane_at(cross->links[best].node);
car_set_ref(g, ci, lane index, &lane->path, -1);     // 0x40f950
return 1;
```

## Call sites / wiring
- `lane_end_move` (0x40e460, card 241) cross branch:
  ```
  Road *rd = road_by_node(g, lane.road);
  uint32_t cn = rd ? rd->node[2 - lane.k18] : 0;      // 2-k18, guard as in card 241
  return cross_dispatch(g, cn, ci, t, lane.k38);      // 0x40e400, link = lane+0x38 value
  ```
  Delete the `// SERVICE MISSING:` stub; the exe returns the dispatch result directly
  (0x40e460 `return cross_dispatch(...) != 0`), before the targetSpeed/t checks.
- 0x40e460 passes `link` = **lane+0x38 value** (0x40fd7c `PUSH [EDI+0x38]`).

## Changes
1. `src/game/traffic.h`, `struct Cross`: add
   `float k40 = -1.0f;   // +0x40 route gate vs T+0x84; ctor 0x40c590 stores -1.0f, writer UNRESOLVED`
   and update the `Car::next_lane_a` comment to the next-ref tag above.
2. `src/game/traffic.cpp`, new file-local functions (place all together immediately
   before `lane_end_move`; `car_set_ref` and `car_distance_on_ref` are defined earlier,
   `cross_claim_path` is at ~0x415b40, `msvc_rand` at the top):
   - `bool lane_room(Graph &g, const Lane &lane, float need)` = 0x40e3a0 (tail =
     `lane.cars.empty() ? npos : lane.cars.back()`; `car_distance_on_ref(g, tail,
     &lane.path, nullptr, &dist)`; `return !(need > lane.path.length - (g.cars[tail].k20 + dist));`).
   - `void cross_path_ref(Graph &g, uint32_t ci, uint32_t cross_node, Cross &c, size_t i)`
     = 0x40f900 + 0x40f950 cross-owner path: `if (g.cars[ci].live == 0) car_set_live(g,
     ci, true);` (cross vf1 = `live_refs != 0`, always true here) then set the three next
     fields as above. `// SERVICE MISSING:` the 0x40f950 immediate branch for a cross
     owner (cross vf2 0x477320 is empty; vf3 0x40cb60 / vf4 0x4883e0 not ported), and
     note it is unreachable from 0x4157a0 because live_refs > 0.
   - `bool cross_route_live(Graph &g, uint32_t cross_node, Cross &c, uint32_t ci, float t, uint32_t link)`
     = 0x4157a0. Use `std::vector<size_t> cand`; `cross_claim_path(c, pick)` then
     `cross_path_ref(g, ci, cross_node, c, pick)`.
   - `bool cross_route_dying(Graph &g, Cross &c, uint32_t ci, uint32_t link)` = 0x4159d0.
   - `bool cross_route_idle(Graph &g, Cross &c, uint32_t ci, uint32_t link)` = 0x40e590.
   - `bool cross_dispatch(Graph &g, uint32_t cross_node, uint32_t ci, float t, uint32_t link)`
     = 0x40e400: resolve `Cross *c = cross_by_node(g, cross_node, nullptr)`, return false
     if null (INVENTED: a road/lane node id is not a cross; the exe would call a vtable on
     it), then the k40 gate and the live_refs sign split.
   Every function carries its `// 0xADDR Name` comment; lane index in the two lane
   selects = `(size_t)(lane - g.lanes.data())`.
3. `src/game/traffic.cpp`, guards for the new cross refs (small, each one comment):
   - `car_target_speed` (0x40dc00): `if (car.dir < 0 && car.path_lane < g.lanes.size() && ...)`
     for the current-lane k98 read (a cross current ref now stores a node id in `path_lane`).
   - `car_scan_obstacles` (0x4107c0 step 3, pass 0): `if (car.dir >= 0) continue;`
     `// SERVICE MISSING: a cross owner's obstacle list (exe owner+4) is not modelled`.
   - `car_start_lane_change` (0x40fde0): add `car.dir < 0` to the early-out (the current
     owner must be a lane; exe uses owner vf5, 0x46acd0 for a cross, untraced).
   - `car_step` step 6 (0x411503): attach to a lane only when `car.dir < 0`
     (`if (car.dir < 0 && car.path_lane < g.lanes.size()) { ... }`); when `car.dir >= 0`
     the car keeps the cross segment ref. `// SERVICE MISSING:` cross vf3 0x40cb60
     (appends the car to the cross's intrusive car list +8/+0xc, re-refs it onto the
     exit lane links[paths[idx].b] with 0x40f950 dir -1, then syncInstance 0x410680
     &cross+0x44); vf4 0x4883e0 untraced. Also guard the step-6 prev detach with
     `car.prev_dir < 0` (a cross prev owner would hit cross vf4 0x4883e0, not
     `lane_detach` 0x40e710). Consequence: until the follow-up card, a car
     on a cross segment sits at pos 0 and its world position is (0,0,0) through the
     existing `dir >= 0` shortcuts in `car_ref_pos_offset`/`car_ref_dir`.
   - `Graph::remove_car` (0x40f290): add `car.dir < 0 &&` to the `lane_detach` guard.
   Leave `car_ref_length`/`car_ref_pos_offset`/`car_ref_dir` `dir >= 0` shortcuts as they
   are (0x413660 ctSegment__posOffset and the +0x30 length stay a later card).
4. Do **not** touch `lane_end_move`'s lane branch, `lane_active_step`, or `lane_timed_event`.

## Files
- edit: src/game/traffic.cpp (new lane_room/cross_path_ref/cross_route_live/
  cross_route_dying/cross_route_idle/cross_dispatch; lane_end_move cross branch;
  car_target_speed, car_scan_obstacles pass 0, car_start_lane_change, car_step step 6,
  Graph::remove_car guards)
- edit: src/game/traffic.h (Cross::k40, Car next-ref comment)

## Done when
- each ported function has a `// 0xADDR Name` comment and the exact constants
  (5.2f / double 0.1 / double 100.0 / 0x3e9 / 0x7fff) and comparison directions from the
  evidence above; logic not taken from the exe is `INVENTED:`/`shortcut:`
- the five missing services are marked in place: cross vf3 0x40cb60, vf4 0x4883e0,
  0x410680 syncInstance, 0x413660 segment pos offset (+0x30 length), cross vf6 0x40dd00;
  vf2 0x477320 documented as an empty function; cross+0x40 writer still UNRESOLVED
- `make -C src check` green; race 659.414 s and `--city 63` unchanged; no new unit tests
  or hand-made expected values
- card frontmatter: status implemented, branch card/242

## UNRESOLVED
- `cross+0x40` float: ctor 0x40c590 stores -1.0f, no writer found (same as card 241);
  0x40e400 gates on it (`if (k40 > T.sim_time) return 0`).
- 0x4883e0 (cross vf4, detach) and 0x410680 (syncInstance) not traced. 0x40cb60 (cross
  vf3) is traced only far enough to know it appends the car to the cross list and hands
  it to the exit lane; not ported.
- 0x4157a0/0x4159d0/0x40e590 index `links[b]` and `paths[i]` unchecked; src guards with
  `lane_at`/size checks (INVENTED).
