---
id: 241
status: implemented
branch: card/241
depends: []
---
# traffic: lane-end move 0x40e460 (next-lane hand-over) + CarPathRef set 0x40f950 + lane vf2 0x411f00

## Goal
Port the lane-end move `0x40e460` and the three helpers its lane branch needs:
`0x40f950` (set a car's current/next `{owner, path, dir}` ref), `0x411f00` (lane vf2:
cache the lane's tail car and note the previous tail as this car's leader) and
`0x40f5e0` (TrafficCar__setLive, currently inlined twice in `lane_attach`). Wire it into
both exe callers: `lane_timed_event` (0x40fcf0, call at 0x40fd8c; card 240 left a shortcut)
and `lane_active_step` (0x40f4e0, call at 0x40f5c3; the current shortcut comment is inside
that loop). Do **not** port the cross-routing branch: when both next-lane nodes are null,
`0x40e460` calls `0x40e400`, which dispatches to a cross virtual
(0x4157a0 / 0x4159d0 / 0x40e590). src has no car-on-cross refs and no `Cross+0x40`; that
branch must stay a `// SERVICE MISSING:` stub returning `false` (the exe's "no route" result;
it returns `true` when a cross vf hands the car to a lane or a cross path). Exe evidence for
the follow-up is in the last evidence block.

**Verified from the shipped map:** parsing `bin/the game/maps/city/ai/city.trc` with the
existing `read_lane` grammar (843,301 bytes, consumed exactly) shows `lane+0x88` and
`lane+0x8c` (src `node88[0]/[1]`) are **0 for all 1,572 lanes** (only `node88[2]/[3]`,
the side lanes, are used: 644 lanes). So in the shipped city `0x40e460` always takes the
cross branch and this card alone changes no observable movement; it ports the rest of the
function and prepares the ref plumbing the cross card needs. Do not fake the cross branch
and do not hardcode this map fact.

## Struct map (exe offset -> src field)

| exe | src |
|---|---|
| lane+0x88 / +0x8c | `Lane::node88[0]` / `node88[1]`, node ids; resolve with `Graph::lane_by_node` |
| lane+0x9c | `Lane::k9c` — **a float** (MULSS in 0x40e460; city.trc value is 0x3f666666 = 0.9f). src declares `uint32_t`; change to `float` |
| lane+0xc8 / +0xcc | `Lane::kc8` (sum of car lengths) / `kcc` (path length) |
| lane+0xb0 / +0xb4 / +0xb8 | `Lane::blocker.serial` / `.car` (1-based, 0 = null) / `.k` |
| lane+0xc | lane car-list tail (exe intrusive); src `Lane::cars.back()` (INVENTED) |
| lane+0xa8 / +0xac / +0x18 / +0x10 / +0x38 | `Lane::live` / `pending_events` / `k18` / `road` (node id) / `k38` |
| car+0x128 | `Car::k128` |
| car+0x38 | `Car::k38` |
| car+0x20 / +0x14 / +0x24 | `Car::k20` / `k14` / `k24` |
| car+0x88 | `Car::pos` |
| car+0xdc / +0x44 / +0x8 | `Car::live` / `live_next` / vector lane list (INVENTED) |
| car+0xb8 / +0xbc / +0xc0 | `Car::next_lane_a` / `next_lane_b` / `kc0` |
| car+0xc4 / +0xc8 / +0xcc | `Car::path_lane` / `path` / `dir` |
| car+0xd0 / +0xd4 | `Car::kd0` / `prev_path` |
| car+0x14c / +0x138 / +0xdd / +0x0 | `Car::type_id` / `inst_id` / `notify` / `serial` |

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x40e460 ctLane__laneEndMove (ECX = lane, args: Car* car, float t, uint32_t link = lane.k38) — 294 bytes
```
if (car->k128 != 0) return false;                          // +0x128
next0 = lane->node88[0]; next1 = lane->node88[1];          // +0x88, +0x8c (lane pointers in the exe)
if (next0 == 0 && next1 == 0) {
    cross = road_by_node(lane->road)->node[2 - lane->k18]; // exe: [lane+0x10]->(3-k18)*4
    return cross_dispatch(ECX = cross, car, t, link) != 0; // 0x40e400 (NOT ported, see below)
}
if (t > targetSpeed(car) / car->k38) return false;         // 0x40dc00, FDIV car+0x38, COMISS
if (lane->live == 0 && t > 0.0f) return false;             // +0xa8
need = car->k20 + car->k14;                                // car+0x20 + car+0x14
chosen = next0;
if (next0 == 0) chosen = next1;
else if (next1 != 0) {                                     // both present:
    cap0 = next0->kcc * next0->k9c - next0->kc8;          // +0xcc * +0x9c - +0xc8 (floats)
    cap1 = next1->kcc * next1->k9c - next1->kc8;
    if (cap0 <= need) {                                    // compiles to SETA pairs
        if (cap1 <= need) return false;                    //   neither fits
        chosen = next1;                                    //   only next1 fits
    } else if (cap1 > need && cap1 >= cap0) {
        chosen = next1;                                    //   both fit -> smaller room; tie -> next1
    }                                                      //   (asm: CMOVA lane0 only when cap0 > cap1)
}
setRef(ECX = car, chosen, &chosen->path, -1);              // 0x40f950
return true;
```
Call sites (xrefs): 0x40fd8c inside `0x40fcf0` and 0x40f5c3 inside `0x40f4e0`. The third
argument is the **value** `lane+0x38` (0x40fd7c `PUSH [EDI+0x38]`), not an address.
Both-fit tie-break confirmed from asm (orchestrator, 0x40e54a/0x40e54d `COMISS XMM1,XMM2` /
`CMOVA ECX,EDX`): chosen stays next1 unless `cap0 > cap1` strictly, so a tie picks next1 —
the pseudocode above is the authority; the raw Ghidra output renders this case as a nested
comma assignment (`fVar6 < fVar5`) and is easy to misread.

### 0x40f950 CarPathRef__set (ECX = car, args: owner lane, path*, int32 dir) — 249 bytes
Lane vtable 0x593688: +4 = 0x40e390 (live byte), +8 = 0x411f00, +0xc = 0x40cbd0
(= src `lane_attach`), +0x10 = 0x40e710 (= src `lane_detach`).
```
if (car->live == 0) {
    if (owner->vf1() /* 0x40e390: owner->live (+0xa8) */)
        setLive(ECX = car, 1);                             // 0x40f5e0
    if (car->live == 0) {                                  // destination lane not live
        laneUnlink(ECX = car);                             // 0x40ca40 = src lane_unlink
        if (car->path != 0) {                              // +0xc8
            old = car->path_lane;                          // +0xc4
            old->vf4(car, car->dir);                       // 0x40e710 (dir ignored)
        }
        car->path_lane = owner; car->path = path; car->dir = dir;
        car->kd0 = 0; car->prev_path = 0;                  // +0xd0, +0xd4
        len = 0;
        if (path) len = (dir < 0) ? path->length          // +0x38 (lane Path)
                                  : *(float*)(path+0x30); // Segment.len (cross path; not modelled)
        pos = len - car->k20;                              // +0x20
        car->pos = pos;                                    // +0x88
        if (car->k14 > pos) car->pos = car->k14;           // +0x14
        if (path) {
            owner->vf2(car);                               // 0x411f00
            owner->vf3(car, dir);                          // 0x40cbd0: append + lane_attach
        }
        return;
    }
}
owner->vf2(car);                                           // 0x411f00
car->next_lane_a = owner; car->next_lane_b = path; car->kc0 = dir;   // +0xb8/+0xbc/+0xc0
```

### 0x411f00 ctLane__vf2 (ECX = lane, arg = Car*) — 128 bytes
```
cached = lane->blocker.car;                                // +0xb4 (Car*)
if (cached != 0 && lane->blocker.serial == cached->serial) {          // +0xb0 == car+0
    noteLeader(ECX = arg /*this car*/, leader = cached, flag = 0);    // 0x40d2c0
} else {
    if (cached != 0) { lane->blocker = {0,0,0}; }          // +0xb0/+0xb4/+0xb8
    tail = lane->cars_tail;                                // +0xc (Car*)
    if (tail != 0) noteLeader(ECX = arg, leader = tail, flag = 0);
}
if (arg != 0) { lane->blocker.car = arg; lane->blocker.serial = arg->serial; }
else          { lane->blocker = {0,0,0}; }
```
(`blocker.k` at +0xb8 is left untouched on the set path, exactly as the asm.)

### 0x40f5e0 TrafficCar__setLive (ECX = car, char on) — inlined in 0x40cbd0
```
if (car->live == on) return;
if (on) { live_count++; [notify "car_add"]; car->live = 1;
          if (car->live_next == 0) { car->live_next = T->live_cars; T->live_cars = car; } }
else    { live_count--; [notify "car_rem"]; car->live = 0; }
if (car->live == 0 && car->k128 != 0) car->k128 = 0;
```
This is exactly the two blocks already inlined in src `lane_attach`; the notify is the
existing `// shortcut: the notify sink (T+0x3130) is not modelled` comment.

### 0x40f4e0 ctLane__activeStep (ECX = lane) -> bool, only the per-car loop changes
```
if (lane->live == 0) return false;                          // +0xa8
CALL 0x52f990 (ECX = lane+0x3c)                             // guard_check_icall, empty (notes/11) — no-op
if (T->sim_time - lane->kc0 > 0.5f) { setLive(ECX = lane, 0); return false; }   // 0x40f6d0, 0x593590
for (car = lane->cars_head; car != 0; car = next) {         // exe reads car+0x8 before stepping
    next = car->next;                                       // +0x8
    if (car->next_lane_b != 0) continue;                    // +0xbc
    carStep(car);                                           // 0x4107c0
    stop = car->next_lane_b ? 0.0f : T->cross_stop_offset;  // +0x3140
    target = targetSpeed(car);                              // 0x40dc00
    v = max(target, 10.0f);                                 // 0x59373c = 10.0f
    t = ((car->pos - car->k14) - stop) * (2.0f / v);        // 0x593728 = 2.0f
    if (t < 0.0f) t = 0.0f;                                 // MAXSS with 0.0 (0x593580)
    laneEndMove(ECX = lane, car, t, lane->k38);             // 0x40e460, result ignored
}
return true;
```

### 0x40e400 + cross vf — NOT ported in this card (SERVICE MISSING)
Cross vtables 0x59377c (eq) / 0x5937c0 (light); slots +0x1c/+0x20/+0x24 are the same in both:
0x4157a0 / 0x4159d0 / 0x40e590. Cross ctor 0x40c590 sets `cross+0x40 = -1.0f`; the writer of
`cross+0x40` is UNRESOLVED (no +0x40 store seen in the traced functions).
```
bool cross_dispatch(ECX = cross, car, t, link)
{
    if (T->sim_time <= cross->k40 && cross->k40 != T->sim_time) return false;   // +0x40 UNRESOLVED writer
    r = cross->live_refs;                                   // +0x38
    if (r > 0) return cross->vf1c(car, t) != 0;             // +0x1c = 0x4157a0
    if (r < 0) return cross->vf20(car, t) != 0;             // +0x20 = 0x4159d0
    return cross->vf24(car, link) != 0;                     // +0x24 = 0x40e590
}
```
- 0x4157a0 (live_refs > 0): `if (t > 5.2) return 0`. Over the cross paths (count +0x20,
  0x60-byte records at +0x1c; path_state[i].block at +0x58 stride 8): block == 0,
  `path[i].a == (uint8)link`, priority (+2) >= (t >= 0.1 ? cross+0x5c : 0), target lane =
  `links[path[i].b].node` (+0x14 stride 8) not halted (+0xc4) and room for
  `car->k20+k14`; score = 0x40e3a0(lane, need) + (lane vf1 == (car+0x160 == 1)) +
  2*((car+0x40 & lane+0xa0) != 0); random tie pick via 0x571110 & 0x7fff; claimPath
  (0x415b40), then `0x40f900(car, i)` = `0x40f950(car, &paths[i].seg, i)`, return 1.
  else 0.
- 0x4159d0 (live_refs < 0): over paths with both state counts 0, `a == link`,
  0x40e3a0(car->k20+car->k14), target lane `links[b]` not halted; score =
  `lane->kc8 / lane->kcc` (minus 100 if `(car+0x40 & lane+0xa0)`); strict minimum wins;
  `0x40f930(lane, &lane.path)` = `0x40f950(car, lane, &lane.path, -1)`, return 1; else 0.
- 0x40e590 (live_refs == 0): `link` indexes cross links (+0x14 stride 8, count +0x18);
  skip lanes on the same road (`lane+0x10`), skip flag != 0, skip halted (+0xc4); gap via
  0x40daa0 (`src car_distance_on_ref`) against `lane->path.length (+0x74)` minus the tail
  car's `k20`; score `lane->kc8 / lane->kcc` (−100 mask); strict minimum;
  `0x40f950(car, lane, &lane.path, -1)`, return 1; else 0.
Helpers: 0x40e3a0(lane, need) = lane empty ? 1 : `lane.path.length - (tail.k20 + dist) >=
need` (dist from 0x40daa0; exact ECX of that call not re-verified). Exe return for all three
vfs is 1 when a target was chosen (a ref was set) and 0 otherwise.
**src stub:** both next nodes null -> `return false;` with the addresses and this text.

## Changes
1. `src/game/traffic.h`, `struct Lane`: change `uint32_t k9c` to `float k9c = 0.0f;`
   (`// +0x9c room factor, float (0.9 in city.trc); MULSS in 0x40e460`). Keep `ka0`.
2. `src/game/traffic.cpp`, `read_lane`: `r.u32(&lane.k9c)` -> `r.f32(&lane.k9c)`
   (raw bits identical; the exe reads the field as a float).
3. `src/game/traffic.cpp`, new file-local `car_set_live(Graph&, uint32_t ci, bool on)` =
   0x40f5e0 body above. Rewrite `lane_attach`'s two live blocks to one call
   `car_set_live(g, ci, lane.live != 0);` (0x40cbd0's two branches are exactly this call);
   keep the head-scheduling block and the k124/kc8 lines where they are, and move the
   notify shortcut comment into `car_set_live`.
4. New `lane_cache_tail(Graph&, size_t li, uint32_t ci)` = 0x411f00 body above
   (`lane.blocker.car` is the cached 1-based index; `lane.cars.back()+1` is the tail leader;
   `car_note_leader` is 0x40d2c0). `// INVENTED: bounds check the exe does not have` on the
   cached index.
5. New `car_set_ref(Graph&, uint32_t ci, size_t li, const Path *path, int32_t dir)` =
   0x40f950 body above. Immediate branch only for `dir < 0` reads `path->length`; the
   `dir >= 0` (+0x30 Segment.len) case has no src owner typed for it: `// shortcut: cross
   owners are not modelled (see step 6 of car_step)`. Order in the immediate branch:
   `lane_unlink` -> old `lane_detach` -> assign fields -> `lane_cache_tail` ->
   `lane.cars.push_back(ci); car.lane = li;` -> `lane_attach` (the exe vf3 appends; src
   lane_attach assumes membership, same as the existing step-6 hand-over).
6. New `lane_end_move(Graph&, size_t li, uint32_t ci, float t)` = 0x40e460 body above.
   Use `g.lane_by_node(lane.node88[0])` / `[1]`; `// INVENTED: a non-lane node id is
   treated as null (the exe would dereference it)`. Cross branch: `// SERVICE MISSING:` stub
   returning false (text in evidence). Selected lane index:
   `(size_t)(chosen - g.lanes.data())`.
7. `lane_timed_event` (0x40fcf0): replace the `shortcut: the lane-end move ...` block with
   `if (!lane_end_move(g, li, head, 0.0f)) lane_schedule_if_idle(g, lane, li, 1.0f);`
   (the exe still reaches the trailing `if (pending == 0) schedule_lane(1.0f)`).
8. `lane_active_step` (0x40f4e0): inside the existing index loop, after `car_step`, add the
   stop/target/t/lane_end_move lines above. Remove the `shortcut: the next-lane move 0x40e460
   is not ported.` comment. Keep the existing INVENTED index-loop comment (the exe reads
   `car+0x8` before each step; a hand-over can relink the vector).
Placement (file-local order): `car_set_live` before `lane_attach`; `lane_cache_tail`,
`car_set_ref`, `lane_end_move` after `car_note_leader` (0x40d2c0) and `lane_detach` (0x40e710)
and before `lane_active_step`/`lane_timed_event`, or forward-declare `car_note_leader`.

## Files
- edit: src/game/traffic.cpp (read_lane, lane_attach, car_set_live, lane_cache_tail,
  car_set_ref, lane_end_move, lane_active_step, lane_timed_event)
- edit: src/game/traffic.h (Lane::k9c type)

## Done when
- every ported/changed function has a `// 0xADDR Name` comment; the cross branch is a
  `// SERVICE MISSING:` stub with the addresses and the exe return values; the cross-owner
  length case and non-lane node ids carry `shortcut:`/`INVENTED:` comments; no invented
  behaviour beyond those comments
- `lane_attach` still behaves exactly as before (car_set_live is only the extracted body)
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/241

## UNRESOLVED
- `cross+0x40` (float, ctor 0x40c590 sets -1.0f): writer not found; 0x40e400 tests
  `if (sim_time <= cross+0x40 && cross+0x40 != sim_time) return 0`, raw line
  `if (*(float *)(DAT_005e652c + 0x84) <= (float)in_ECX[0x10] && ...)`.
- 0x40e400 / 0x4157a0 / 0x4159d0 / 0x40e590 are not ported here (follow-up card); their
  helper 0x40e3a0 (room test) and the exact 0x40daa0 argument order were only summarised.
- `lane.k18` is 0 or 1 in city.trc; the exe computes `road+(3-k18)*4` with no guard, so
  k18=3 would read the road vtable. src uses `node[2-k18]` with a k18 <= 2 guard (INVENTED).
- 0x40f950's `dir >= 0` length source (`*(float*)(path+0x30)`, a Segment field) is not
  modelled; unreachable from the lane branch (always dir = -1).
