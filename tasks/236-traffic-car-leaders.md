---
id: 236
status: merged
branch: card/236
review: deepseek-pass
depends: [235]
---
# traffic: TrafficCar__step leader slots + nearest gap/leader speed

## Goal
Port the step-2 slice of `TrafficCar__step` 0x4107c0 (ECX = car) into card 235's
`car_step(Graph &, uint32_t)`: the brake-gap value, the four 16-byte leader slots at
`car+0x48..0x84` (marking `TrafficCar__noteLeader` 0x40d2c0, refresh with
`TrafficCar__distanceOnRef` 0x40daa0 + helper 0x40e810, sort with 0x40c270), and the
nearest-gap/leader-speed update of card 235's `free` and `leader_speed` locals. Step 5
(already in 235) then consumes those two locals.

Steps 3 (obstacle scan), 4 (lane change, 0x40fde0), 7/8 (projection/registration) stay
later cards: do not implement them, do not mark them UNRESOLVED. The two step-2 branches
that only exist for a lane-change target (`car+0x124` / `car+0xfc`) are kept out with
`shortcut:` comments (see below).

## Exe evidence (cleaned pseudocode, renamed fields)
Field map used here (all offsets cited by existing src comments unless marked new):
`+0x14 k14`, `+0x20 k20`, `+0x24 k24`, `+0x48..+0x84 leader slots`, `+0x88 pos`,
`+0xb0 speed`, `+0xb8 next owner (next_lane_a)`, `+0xbc next path (next_lane_b)`,
`+0xc0 kc0`, `+0xc4/+0xc8/+0xcc cur ref {lane/path/dir}`, `+0xd0/+0xd4/+0xd8 prev ref
{kd0, path, dir}` (new +0xd0/+0xd8 fields; +0xd4 already commented in traffic.h),
`+0xfc lane-change projection distance (not modelled)`, `+0x124 lane-change target lane
(not modelled)`. Lane list: `lane+8` head = first car = smallest pos, `lane+0xc` tail;
`car+4` prev (smaller pos = ahead), `car+8` next (notes/29 §2, notes/30). src `lane.cars`
is ascending pos, so the leader of `ci` is the element before `ci` in `lane.cars`.

### 0x40d2c0 TrafficCar__noteLeader (ECX = car, args: Car *leader, uint8 flag; 0x40d2c0..0x40d371)
```
slot[i] = car+0x48+16i = {serial +0, car +4, flag +8, dist +0xc}
for (i = 0; i < 4; i++)                                    // 0x40d2d0..0x40d2e8
    if (slot[i].car == leader && (leader == 0 || slot[i].serial == leader->serial)) {
        slot[i].flag = flag; return;                       // 0x40d308
    }
for (i = 0; i < 3; i++) {                                  // 0x40d2f0..0x40d304
    if (slot[i].car == 0) break;                           // 0x40d2f3
    if (slot[i].serial != slot[i].car->serial) {           // 0x40d2f9
        slot[i].serial = slot[i].car = 0; slot[i].flag = 0; // 0x40d319..0x40d326
        break;
    }
}
if (leader) { slot[i].serial = leader->serial; slot[i].car = leader; }  // 0x40d359/0x40d361
else        { slot[i].serial = 0; slot[i].car = 0; slot[i].flag = 0; }  // 0x40d339..0x40d347
slot[i].flag = flag;                                       // 0x40d36a
```
The search covers slots 0..2 only; when all three are valid it writes slot 3 (0x40d304
`CMP EDX,3`). `leader->serial` is `*(int*)leader`, i.e. car+0.

### 0x40daa0 TrafficCar__distanceOnRef (ECX = leader car; args ref1, ref2, float *out; 0x40daa0..0x40dbbc)
```
car_step(leader)                                           // 0x40daa6, ECX = leader (recursion)
if (leader.path        (+0xc8) == ref1) { *out = leader.pos (+0x88); return 1; }
if (leader.prev_path   (+0xd4) == ref1) {
    *out = leader.pos - ref_len(leader.path, leader.dir (+0xcc)); return 1;   // 0x40dad2..0x40db04
}
if (leader.next_path   (+0xbc) == ref1) {
    *out = leader.pos + ref_len(leader.next_path, leader.kc0 (+0xc0)); return 1; // 0x40db18..0x40db46
}
if (ref2) {
    if (leader.path == ref2) {
        *out = leader.pos - ref_len(leader.path, leader.dir); return 1;         // 0x40db5f..0x40db74
    }
    if (leader.prev_path == ref2) {
        *out = (leader.pos - ref_len(leader.path, leader.dir))
             - ref_len(leader.prev_path, leader.prev_dir (+0xd8)); return 1;    // 0x40db80..0x40dba6
    }
    if (leader.next_path == ref2) { *out = leader.pos; return 1; }               // 0x40dbae->0x40dab6
}
return 0;                                                  // 0x40dbb6
```
The ref structs passed to 0x40e810 are `LEA ECX,[leader+0xc4]` (cur) and
`LEA ECX,[leader+0xd0]` (prev) (0x40db5f, 0x40db91).

### 0x40e810 ctPathRef__length (ECX = ref struct {owner +0, path +4, dir +8})
```
p = *(void**)(ecx + 4);
if (!p) return 0.0;
if (*(int*)(ecx + 8) < 0) return *(float*)(p + 0x38);   // lane ref: Path::length (notes/19: +0x38)
return *(float*)(p + 0x30);                              // cross ref: Segment::len (notes/19)
```
src refs are always lane refs (`dir = -1`; notes/29 §2 spawn), so only the +0x38 branch is
reachable. The +0x30 (Segment len of a cross ref) has no src representation: shortcut, see below.

### 0x40c270 slot sort, cdecl `(LeaderSlot *a, int n)` (0x40c270..0x40c3df)
Exact min-heap build + heapsort; `car == 0` (null pointer in the exe) compares smaller than
every real slot and every comparison is on `dist` only. Result: **descending dist, empty
slots last** (checked by replaying the asm: {5,3,8,1} -> {8,5,3,1}; empty first -> empty last).
```
for (i = 1; i < n; i++) {                                  // 0x40c284..0x40c2ec
    e = a[i]; j = i;
    while (j != 0) {
        p = (j - 1) / 2;
        if (a[p].car == 0) break;                          // 0x40c2b1
        if (e.car != 0 && !(e.dist < a[p].dist)) break;    // 0x40c2b8..0x40c2c5
        a[j] = a[p]; j = p;
    }
    a[j] = e;
}
for (end = n - 1; end > 0; end--) {                        // 0x40c2ee..0x40c3d3
    temp = a[0]; i = 0; left = 1;
    while (left < end) {                                   // 0x40c30a, 0x40c3a6
        right = 2*i + 2;
        if (right >= end || a[left].car == 0 ||
            (a[right].car != 0 && a[left].dist <= a[right].dist))
            child = left;                                  // 0x40c319..0x40c33e
        else child = right;
        if (a[end].car == 0 ||
            (a[child].car != 0 && a[end].dist <= a[child].dist)) break;  // 0x40c343..0x40c38c
        a[i] = a[child]; i = child; left = 2*i + 1;
    }
    a[i] = a[end]; a[end] = temp;                          // 0x40c3b9..0x40c3c7
}
```

### 0x4107c0 TrafficCar__step, step 2 slice (0x41087b..0x410a55)
Insert after card 235's step 1 (`leader_speed = -1.0f; free = d0;`) and before step 5.
```
0x410888  brake = (max(car.speed(+0xb0) - 2.0f, 0.0f) * 0.5f + 1.0f) * car.k24(+0x24)
0x4108b9  leader = 0
          if (car.dir (+0xcc) == -1) leader = car.prev-in-lane;      // car+4; 0 = none
          if (!leader && car.next_path (+0xbc))
              leader = car.next_owner(+0xb8)->vf0(car.kc0 (+0xc0));  // 0x4108cb..0x4108df,
                                                                     // indirect CALL EAX (vtable[0])
          if (leader) car_note_leader(leader, flag 0);               // 0x4108e1..0x4108e6
0x4108eb  /* target-lane car+0x124 first car with pos < car+0xfc, noteLeader(.,1) --
             kept out: car+0x124 is only set by the lane-change code, always 0 in src */
0x410920  for (i = 0; i < 4; i++) {                                  // slot = car+0x48+16i
              if (!slot.car) continue;                               // 0x410935..0x410937
              if (slot.serial != slot.car->serial) {                 // 0x41093d..0x410942
                  slot.serial = slot.car = 0; slot.flag = 0; continue;  // 0x4109cc
              }
              if (slot.flag != 0) {                                  // 0x410948
                  /* car+0x124 / car+0xfc target-lane branch -- kept out, see below */
              } else {
                  if (!distanceOnRef(slot.car, car.path(+0xc8), car.next_path(+0xbc), &slot.dist))
                      { slot.serial = slot.car = 0; slot.flag = 0; continue; }   // 0x41099c..0x4109b6
                  slot.dist += slot.car->k20;                        // 0x4109b8..0x4109c5, +0x20
              }
          }
0x4109ec  slot_sort(car.slots, 4);
0x4109f7  if (car.slot[0].car) {
              if (car.slot[0].serial != car.slot[0].car->serial) {   // 0x410a0b..0x410a0f
                  car.slot[0].serial = car.slot[0].car = 0; car.slot[0].flag = 0;  // 0x410a11
              } else {
                  gap = (car.pos - car.k14) - brake - car.slot[0].dist;  // 0x410a24..0x410a35
                  if (gap < free) { free = gap; leader_speed = car.slot[0].car->speed; } // 0x410a3a..0x410a52
              }
          }
```
`slot[0]` after the sort is the largest dist = the nearest car (positions count down to the
lane end, so the closest car has the largest pos/dist). Steps 3/4 further reduce `free` after
this block and clamp it; that clamp is a later card.

### 0x40e150 ctLane init: does not seed the slots
0x40e150 writes lane+0xc8 (occupied length), lane+0xc0 (sim time), lane+0xcc, lane+0xc4 and
steps lane+8, but nothing at `car+0x48..0x84`. The slots are zeroed by the car ctor
(0x40c400/0x40c710, notes/29 §2) and freed by the release dtor. src `car_ctor` does
`c = Car();` so a value-initialized slot array is enough; `car_release` needs no change.

### 0x41153e..0x41155c prev-ref store (needed by 0x40daa0)
Step 6 (card 235) copies the old cur ref to the prev ref before overwriting cur:
```
0x41153e  XMM0 = *(u64*)(car+0xc4)            // old lane, old path
0x41154c  *(u64*)(car+0xd0) = XMM0            // +0xd0 = old lane, +0xd4 = old path
0x41155c  *(u32*)(car+0xd8) = *(u32*)(car+0xcc)  // +0xd8 = old dir
0x411568  *(u64*)(car+0xc4) = *(u64*)(car+0xb8)  // cur = next ref
0x411570  *(u32*)(car+0xcc) = *(u32*)(car+0xc0)
```
Card 235's step-6 block must persist this to the new Car fields (if it kept prev only in
locals, add the stores at the same point); `distanceOnRef` on a leader that just handed over
depends on them.

Constants: 0x5935ac = -1.0f, 0x593728 = 2.0f, 0x593590 = 0.5f, 0x593598 = 1.0f.

## Already done on main (card 234)
`Car.kd0` / `Car.prev_path` (+0xd4) / `Car.prev_dir` (+0xd8) exist and `car_step`'s
hand-over already stores the prev ref (0x41154c/0x41155c); `car_ref_length` takes
`(path, dir)`. Skip those edits; keep the LeaderSlot work and the step-2 slice.

## Files
- edit: `src/game/traffic.h` — `struct Car`:
  ```cpp
  struct LeaderSlot {            // car+0x48..0x84, 4 x 16 bytes
      uint32_t serial = 0;       // +0x00 serial copy (validity check)
      uint32_t car = 0;          // +0x04 INVENTED: 1-based car index, 0 = empty (a Car* in the exe)
      uint8_t flag = 0;          // +0x08 0 = lane leader, 1 = target-lane slot
      float dist = 0.0f;         // +0x0c
  };
  LeaderSlot leaders[4];         // +0x48
  ```
  replace `uint32_t kd0 = 0, kd4 = 0;  // +0xd0, +0xd4 second ref` with
  ```cpp
  uint32_t kd0 = 0;                // +0xd0 prev ref lane/owner
  const Path *prev_path = nullptr; // +0xd4 prev ref path
  int32_t prev_dir = 0;            // +0xd8 prev ref dir
  ```
- edit: `src/game/traffic.cpp`
  - fix the two existing `car.kd0 = car.kd4 = 0;` (`spawn_on_road`, `remove_car`) to
    `car.kd0 = 0; car.prev_path = nullptr; car.prev_dir = 0;`.
  - add `void car_step(Graph &, uint32_t);` forward declaration (if not already visible)
    before the new helpers.
  - add in the anonymous namespace near `car_step` (card 235's location): `ref_len`,
    `car_note_leader`, `car_distance_on_ref`, `slot_sort` exactly as in the evidence; each
    keeps a `// 0xADDR Name` comment (`0x40e810`, `0x40d2c0`, `0x40daa0`, `0x40c270`).
  - `car_step`: insert the step-2 slice above after `leader_speed = -1.0f; free = d0;`
    (`brake` local included) and before the step-5 speed integration. Card 235's
    `leader_speed`/`free` locals are assigned here.
  - `car_step` step 6: add the +0xd0/+0xd4/+0xd8 prev-ref stores (0x41154c/0x41155c) if
    card 235 did not already keep them.
  - shortcuts (each one line):
    - cross next-ref leader `car.next_owner->vf0(kc0)` 0x4108db is an indirect vtable call;
      no src car has a next ref yet (a later card sets it) -> no leader from it.
    - target-lane marking 0x4108eb..0x41091b and the `flag != 0` refresh branch: `car+0x124`
      is the lane-change target (a later card) and `car+0xfc` its projection distance; src
      `k124` is 0 for all current data (comment at `car_target_speed`) -> skipped.
    - `ref_len` cross branch: the exe reads a Segment's `+0x30` len for a cross ref; src
      refs are lane paths (`dir < 0`) -> return `path->length` and mark the cross case.
  - mark the slot `car` index / empty-0 encoding `INVENTED:` (exe stores a Car pointer).

## Done when
- each ported function has a `// 0xADDR Name` comment; every gap, encoding and skipped
  branch is marked `INVENTED:` or `shortcut:` (none of the out-of-scope steps 3/4/7/8 is
  marked UNRESOLVED)
- `make -C src check` green; no new unit tests or hand-made expected values
- race/city harness numbers unchanged (notes/38: vmboot has no live lane, so `car_step`
  never reaches step 2 there)
- card frontmatter: status implemented, branch name
