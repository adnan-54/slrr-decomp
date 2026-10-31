---
id: 248
status: todo
branch: card/248
depends: [246]
---
# harness: let the city lanes expire so a routed car actually commits its cross ref

## Goal
Card 246 proves the lane-end dispatch (`0x40e460 -> 0x40e400 -> 0x40f950` records a **next**
ref). But `ctLane__activeStep 0x40f4e0` skips any car that already carries a next ref
(`if (car->next_lane_b != 0) continue;`), and the harness re-touches every road every frame,
so a routed car stays parked with a next ref forever and `car_step` step 6 (which copies
next -> current) never runs on a live lane.

The exe reaches step 6 through the **timed event** `ctLane__timedEvent 0x40fcf0`, which
steps every car of a non-live lane with no next-ref skip. A lane goes non-live when
`sim_time - lane.kc0 > 0.5` (`0x40f4e0`, `0x593590`). So: after the long run, stop touching
the roads for ~60 frames (0.96 s of sim time) and the lanes expire, the timed events step
their cars, and any car holding a cross next ref must commit it.

Add that phase to `city_checks` and check the outcome.

## Exe evidence
- `0x40f4e0` (card 241): `for (car = head; car; car = next) { next = car->next; if
  (car->next_lane_b) continue; carStep(car); ... }` and the expiry
  `if (T->sim_time - lane->kc0 > 0.5f) { setLive(lane, 0); return false; }`.
- `0x40fcf0` (card 240): `if (lane->a8 /*live*/) return;` then `for (car = lane->8; car;
  car = next) TrafficCar__step(car);` with **no** next-ref skip, plus the lane-end move and
  the re-schedule.
- `0x40f6d0 setLive(lane, 0)` schedules a lane event for a lane that still has cars
  (`car_time_to_lane_end`), which is what lets `0x40fcf0` run afterwards.
- `TrafficCar__step 0x4107c0` step 6 (card 236): when the current ref is finished, the next
  ref is copied into `path_lane/path/dir` (`kc0 >= 0` = cross path, geometry `0x413660`) and
  the old owner's vf4 runs.
- In src the cross ref tag is `kc0 >= 0` with `path` = `&cross.paths[i].seg` (card 242), the
  current ref carries it in `car.path`/`car.dir` (`dir >= 0`).

## Changes (src/tools/vmboot.cpp only)
In `city_checks`, **after** card 246's 300-frame loop and its checks (keep those exactly as
they are), add:

```cpp
// INVENTED HARNESS: the exe's render walk keeps a visible road's lanes alive; dropping
// the touch for 0.5 s (0x40f4e0, 0x593590) makes ctLane__activeStep 0x40f4e0 expire them,
// which lets ctLane__timedEvent 0x40fcf0 step the cars without the next-ref skip and lets
// TrafficCar__step 0x4107c0 step 6 commit a recorded cross next ref.
int committed = 0;
for (int k = 0; k < 60; k++) {
    loop.frame(loop.last_control_ms() + 16);
    if (any_car_on_cross_ref(*graph)) { committed = 1; break; }
}
c.check(committed, "city: after the lanes expire a routed car commits its cross ref "
                   "(0x4107c0 step 6, cross geometry 0x413660)");
c.check(all_positions_sane(*graph), "city: every traffic car stays inside its current ref "
                                    "after the expiry phase (lanes 0x414050, segments 0x413660)");
```
with one new file-local helper:
```cpp
// INVENTED HARNESS: a car whose current ref is a cross path (path index, not a lane).
bool any_car_on_cross_ref(const traffic::Graph &g) { ... car.dir >= 0 && car.path ... }
```

Rules for this card:
- **Do not** change the engine (`src/game/**`) — this is a harness card.
- **Do not** weaken an existing check or adjust `--race`/`--city` expectations.
- If `committed` stays 0, that is a finding, not a failure to hide: report the frame count,
  the number of cars still holding a cross next ref, their `pos`, their current ref length
  and their `dir`, and note whether `0x40fcf0` ran at all (the lane event heap must be
  non-empty in that phase). Keep the check as written.
- Reuse the helpers card 246 added (`CarRefTag`, `car_positions`, `all_positions_sane`).

## Files
- edit: src/tools/vmboot.cpp (`city_checks` + one helper)
- read only: src/game/traffic.h, src/game/traffic.cpp (`set_lane_live`, `lane_timed_event`,
  `car_step` step 6)

## Done when
- the phase and both checks carry `// INVENTED HARNESS:` / `// 0xADDR` comments
- `make -C src check` green; report `--city`, `--selfcheck`, `--race` counts and the race
  time (must stay 659.414 s), plus how many frames the expiry phase needed
- no unit tests, no hand-made expected values
- card frontmatter: status implemented, branch card/248
