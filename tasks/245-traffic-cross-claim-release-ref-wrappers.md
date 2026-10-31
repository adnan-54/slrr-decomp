---
id: 245
status: draft
branch:
review:
depends: []
---
# traffic: cross-path claim/release state + ref-set wrappers 0x40f900 / 0x40f930

## Goal
Pin the verified contract of the cross path-claim state and of the two path-ref
wrappers around `car_set_ref` (0x40f950), so cards 242/244 can be promoted against
checked evidence. This card is evidence + comment-only: `cross_claim_path` (0x415b40)
and `cross_eq_step` (0x415c40, the release/decay side) already match the exe exactly,
and 0x40f900/0x40f930 are one-call constructions over `car_set_ref` that src cannot
express until card 242's cross-owner ref tag exists. Adding them now would be dead
code; instead record the exact argument construction at the existing ports and correct
the stale "not traced yet / cross owners not modelled" comments.

Card 242 currently says "0x415b40 (not traced yet)" and describes the cross ref in
prose; this card replaces that with the verified pseudocode below.

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x415b40 ctCross__claimPath (ECX = cross, arg i)
State is `cross+0x58`, stride 8: `count (+0)`, `block (+4)`; `cross+0x1c` = path
records, stride 0x60; `cross+0x20` = path count. All bytes are unsigned; the `block`
vs `c` compare is signed.
```
if (path_state[i].count == 0) {
    a = paths[i].a; b = paths[i].b; prio = paths[i].c;   // byte +0, byte +1, ushort +2
    for (j = 0; j < path_count; j++) {
        if (path_state[j].block < (int)prio) {          // signed
            aj = paths[j].a; bj = paths[j].b;
            if (aj != a) {
                if (bj == b) path_state[j].block = prio;
                else {
                    inA = (a < b) ? (a < aj && aj < b) : (a < aj || aj < b);  // cyclic span
                    inB = (a < b) ? (a < bj && bj < b) : (a < bj || bj < b);
                    if (inA != inB) path_state[j].block = prio;
                }
            }
        }
    }
}
path_state[i].count += 10;                            // unconditional, even if count != 0
```
It writes exactly `path_state[i].count` (+10) and `path_state[j].block` (= `paths[i].c`);
it does not touch the car.

Call sites (Ghidra xrefs to 0x415b40): 0x40f3c1 / 0x40f3e1 (`TrafficCar__liveStep`
0x40f350, next-ref / cur-ref cross branches, still a shortcut in src), 0x415940 /
0x4159ab (inside `0x4157a0`), 0x415b17 (`0x415b00 pathBlocked`), 0x415d37
(`0x415c40 cross_eq_step`). At 0x415940 the success path is
`pick = cand[(msvc_rand() & 0x7fff) % count]; claimPath(pick); 0x40f900(car, pick); return 1;`
(the 0x40f900 call opcode is 0x41594b, i.e. immediately after the claim).

### Release counterpart: 0x415c40 ctCrossEq__step (ECX = cross) — the count decay
There is no dedicated release function in the xref list. The only writers of
`path_state.count` are 0x415b40 (+10) and 0x415c40:
```
for (i) path_state[i].block = 0;
r = cross.live_refs;                                  // +0x38
if (r < 0) {                                          // cross going inactive
    any = false;
    for (i) {
        if (path_state[i].count > 4) path_state[i].count = 5;
        path_state[i].count -= 1;
        if (path_state[i].count < 1) path_state[i].count = 0;
        else { any = true;
               for (j) if (paths[i].b == paths[j].b) path_state[j].block = 0x3ea; }
    }
    if (any) return true;
    cross.live_refs = 0; return false;
}
if (r > 0) {                                          // still live: re-claim, keep count-1
    for (i) {
        if (path_state[i].count > 4) path_state[i].count = 5;
        c = path_state[i].count; path_state[i].count = 0;
        if (c > 1) { claimPath(i); path_state[i].count = c - 1; }
    }
    return true;
}
return false;                                         // r == 0
```
Other writers of `block`: 0x4157a0 sets `0x3e9` on the capacity-reject branch,
0x415d60 (light step) sets `0x3eb`/`0x3ec`, 0x415c40 clears it at entry and sets
`0x3ea` in the dying branch. src `cross_claim_path` / `cross_eq_step` already
implement all of this (traffic.cpp ~0x780 and ~0x817) — do not change their logic.

### 0x40f900 ctCross__setPathRef (ECX = cross; args Car* car, uint32_t i) — 35 bytes, RET 8
Exact asm (0x40f900..0x40f921): `EAX = i; PUSH i; EDX = i*0x60; EAX = [ECX+0x1c];
EAX += 4 + i*0x60; PUSH EAX; PUSH ECX; ECX = [EBP+8] /*car*/; CALL 0x40f950`.
```
0x40f950(ECX = car, owner = cross, path = &cross->paths[i] + 4 /* = &Segment */, idx = i);
```
The record offset is +4 because the exe's in-memory CrossPath is `{a,b,c}` at 0..3,
then the 0x54-byte Segment at +4, then `k58/k5c` at +0x58/+0x5c (notes/19 line 110).
src keeps `k58/k5c` before `seg`, so pass `&c.paths[i].seg` by name, never a record offset.

### 0x40f930 __setLanePathRef (ECX = car, inherited; args owner lane, Path*) — 20 bytes, RET 8
Exact asm: `PUSH -1; PUSH [EBP+0xc] /*path*/; PUSH [EBP+8] /*owner*/; CALL 0x40f950`
— it does not set ECX, so the caller supplies the car. Its only caller (xref
0x415ae0, inside `0x4159d0 routeDying`) does:
`ECX = car ([EBP+8]); PUSH &lane->path (lane+0x3c); PUSH lane; CALL 0x40f930`.
```
0x40f950(ECX = car, owner = lane, path, idx = -1);
```

### 0x40f950 CarPathRef__set (ECX = car; args owner, path, idx) — already src `car_set_ref`
src `car_set_ref` (traffic.cpp ~0x1362) is the lane-owner port. The exe body, for the
cross owner (idx >= 0) that 0x40f900 passes:
```
if (car.live == 0) {                                  // +0xdc
    if (owner->vf1() != 0) car_set_live(car, 1);      // cross vf1 0x40e380 = live_refs != 0;
                                                      // lane vf1 0x40e390 = lane.live
    if (car.live == 0) {                              // unreachable from 0x4157a0 (live_refs > 0)
        lane_unlink(car);                             // 0x40ca40
        if (car.cur_owner != 0) old_owner->vf4();     // lane 0x40e710 lane_detach;
                                                      // cross vf4 0x4883e0 NOT TRACED
        car.cur_owner = owner; car.path = path; car.dir = idx;
        car.prev_owner = 0; car.prev_path = 0;
        len = path ? (idx < 0 ? *(f32*)(path+0x38) /*Path.length*/
                              : *(f32*)(path+0x30) /*Segment.len*/) : 0.0f;
        car.pos = len - car.k20;                      // +0x88 = len - +0x20
        if (car.pos < car.k14) car.pos = car.k14;     // +0x14
        if (path) { owner->vf2(); owner->vf3(); }     // lane 0x411f00 / 0x40cbd0;
                                                      // cross vf2 0x477320 empty (card 242),
                                                      // cross vf3 0x40cb60 NOT TRACED (card 244)
        return;
    }
}
owner->vf2();                                         // cross: empty
car.next_owner = owner; car.next_path = path; car.next_idx = idx;   // +0xb8/+0xbc/+0xc0
```
So for the 0x4157a0 route the wrapper always ends in the tail: it only records the
**next** ref (`next_lane_a = cross node id, next_lane_b = &paths[i].seg, kc0 = i` with
card 242's INVENTED tag; car is forced live because `live_refs > 0`). The lane wrapper
(0x40f930, idx -1) can take the immediate branch and then computes `pos` from
`Path::length` (+0x38) — that is card 241's `car_set_ref(..., lane, &lane.path, -1)`.

## Struct map (exe offset -> src field)
| exe | src (traffic.h) |
|---|---|
| cross+0x58, stride 8, +0 count / +4 block | `Cross::path_state[i].count` / `.block` |
| cross+0x1c, stride 0x60, +0 a / +1 b / +2 c; record+4 Segment | `Cross::paths[i].a` / `.b` / `.c`, `&CrossPath::seg` |
| cross+0x20 | `Cross::paths.size()` |
| cross+0x38 | `Cross::live_refs` |
| car+0xdc / +0x88 / +0x20 / +0x14 | `Car::live` / `.pos` / `.k20` / `.k14` |
| car+0xb8 / +0xbc / +0xc0 | `Car::next_lane_a` / `.next_lane_b` / `.kc0` |
| car+0xc4 / +0xc8 / +0xcc | `Car::path_lane` / `.path` / `.dir` (owner pointer in the exe; lane index today) |
| car+0xd0 / +0xd4 | `Car::kd0` / `.prev_path` |
| lane+0x3c / +0xa8 | `Lane::path` / `.live` |

## Changes
1. `src/game/traffic.cpp`, comments only:
   - above `cross_claim_path` (~line 780): add the release counterpart
     `0x415c40 cross_eq_step` decay rule (count clamp >4 -> 5, -1, floor 0 while
     `live_refs < 0`; re-claim + `c-1` while `live_refs > 0`) and the claim call sites
     (0x40f3c1/0x40f3e1 liveStep, 0x415940/0x4159ab routeLive, 0x415b17 pathBlocked,
     0x415d37 eq step).
   - at `car_set_ref` (~line 1362): state the exact wrapper construction
     `0x40f900(ECX=cross, car, i) -> car_set_ref(ECX=car, owner=cross, &paths[i].seg, i)`
     and `0x40f930(ECX=car, lane, &lane.path) -> car_set_ref(..., -1)`, and that the
     cross owner reaches the tail (next ref) because `live_refs > 0`.
   - at `car_live_step` (~line 1267) and the `dir >= 0` shortcuts in
     `car_ref_pos_offset`/`car_ref_dir`/`car_ref_length`: leave the shortcuts, but
     name 0x40cb60 / 0x4883e0 / 0x413660 / Segment+0x30 as the missing cross-side
     services (same text card 242 uses; no new code).
2. `src/game/traffic.h`: extend the `Car::next_lane_a` / `path_lane` comments to say the
   exe stores an owner pointer that may be a cross (idx >= 0) or a lane (idx -1);
   src keeps a lane index until card 242's tag. No field changes.
3. Do **not** add wrapper functions, change `cross_claim_path`/`cross_eq_step`, or
   touch `lane_end_move`. No behaviour change is expected.

## Files
- edit: src/game/traffic.cpp (comments at cross_claim_path, car_set_ref, car_live_step
  and the three `dir >= 0` shortcuts)
- edit: src/game/traffic.h (Car ref comments only)

## Done when
- comments cite 0x415b40 / 0x415c40 / 0x40f900 / 0x40f930 / 0x40f950 and the release
  rule exactly as above; no function body changes (comment-only diff)
- `make -C src check` green; race 659.414 s and `--city 63` unchanged; no new unit tests
  or hand-made expected values
- card frontmatter: status implemented, branch card/245

## UNRESOLVED
- Whether cross vf3 0x40cb60 (card 244) or vf4 0x4883e0 (card 242) also writes
  `cross+0x58` state: both functions are untraced; the xref/asm evidence here covers
  only 0x415b40 and 0x415c40 as count writers. Wrapper pseudocode lines above carry
  `NOT TRACED` at those calls.
- `owner->vf2()` for a cross is 0x477320 "empty RET 4" per card 242, not re-verified
  in this trace.
