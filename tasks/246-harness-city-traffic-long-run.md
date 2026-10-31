---
id: 246
status: implemented
branch: card/246
depends: [243, 244]
---
# harness: run the city far enough for a real lane-end hand-over, and check the traffic invariants

## Goal
`vmboot --city` touches the road nodes **once** (card 243) and runs 3 frames of 16 ms. A lane
whose `kc0` is older than 0.5 s is dropped by `ctLane__activeStep 0x40f4e0`, so after ~30
frames nothing is live, and in 48 ms no car reaches a lane end. Result: cards 234-244 are
built but never exercised — the lane-end move, the cross routing and the cross hand-over all
sit on paths the harness cannot reach.

Change the city probe to behave like the exe's per-frame render LOD walk: touch the road
nodes **before every frame**, then run enough frames (cap 300 = 4.8 s of sim time) that a
traffic car reaches a lane end and is handed over. Then assert invariants that only hold if
the ported traffic code really runs.

The touch is the same INVENTED stand-in as card 243 (the exe gets it from
`0x45f180 -> 0x461980 -> 0x4d44d0 -> lane vf7 0x40eed0`); the *invariants* below are not
invented, they follow from the ported exe code.

## Exe evidence
- `ctLane__activeStep 0x40f4e0`: `if (sim_time - lane.kc0 > 0.5f) { setLive(lane, 0); }`
  (0x40f6d0), and `0x40eed0` stamps `lane.kc0 = sim_time` on every touch. So the exe keeps
  lanes live by touching them every frame; a one-shot touch expires.
- `TrafficCar__step 0x4107c0` advances `car.pos` by the car's speed (steps 1/5/6, cards
  234-236) and `lane_end_move 0x40e460` hands the car to the next lane or, when both next
  nodes are null (every shipped city lane, card 241), to the cross routing `0x40e400`.
- `0x40e590` / `0x4159d0` hand the car to a **lane** (`car_set_ref` -> `lane_attach`);
  `0x4157a0` hands it to a **cross path** (`car_set_ref` with `kc0 >= 0`, cross geometry
  `0x413660`, card 244) and `ctCross__vf3 0x40cb60` re-refs it onto the exit lane when the
  path is done.

## Changes (src/tools/vmboot.cpp only)
In `city_checks`, after the existing report checks and before the current 3-frame loop,
replace the single touch with a loop. Keep the existing `touched` / `live_lanes` checks and
everything else unchanged:

```cpp
// INVENTED HARNESS: the exe's render LOD walk touches the road nodes every frame
// (0x45f180 -> 0x461980 -> 0x4d44d0 -> lane vf7 0x40eed0); a one-shot touch expires
// after 0.5 s in ctLane__activeStep 0x40f4e0.
const std::vector<float> start_pos = car_positions(*graph);      // helper below
int handed_over = 0;
for (int k = 0; k < 300; k++) {
    boot.runtime()->touch_road_nodes(map_res);                    // every frame
    loop.frame(loop.last_control_ms() + 16);
    if (any_car_handed_over(*graph, start_pos)) { handed_over = 1; break; }
}
```
with two small file-local helpers (no new game code):
- `car_positions(graph)` — `car.pos` per `car.serial != 0`, plus each car's current lane /
  `kc0` tag, so a hand-over is detectable. Simplest sound test: remember
  `(serial, lane, kc0)` and report a hand-over when a car's `(lane, kc0)` pair changed, or
  its `lane` changed to another index.
- `any_car_handed_over(graph, start)` — compares against the recorded triples.

New checks (message text must cite the exe address):
- `c.check(handed_over, "city: within 300 frames some live-lane car is handed over (0x40e460 -> 0x40e400)")`
- `c.check(all_positions_sane(*graph), "city: every traffic car stays inside its current ref (lanes 0x414050, cross segments 0x413660)")`
  (dir < 0: `0 <= pos <= lane.path.length`; `kc0 >= 0`: `0 <= pos <= segment len`; skip cars
  with no ref.)
- `c.check(heap_ok_again, "city: the lane event heap is still a min-heap after the run (0x41a950)")`
- Keep the existing three-frame contract green: the earlier `heap.size() == events` check runs
  **before** this new loop (it must keep its current position in the function).

If a check fails because the port really is wrong, **do not weaken the check**: report the
failure, the observed values and the car/lane ids in the card completion note.

## Files
- edit: src/tools/vmboot.cpp (`city_checks`)
- read only: src/game/traffic.h (`Car`, `Lane`, `Graph`), src/game/runtime.h
  (`touch_road_nodes`)

## Done when
- the touch loop and the three new checks carry `// INVENTED HARNESS:` / `// 0xADDR` comments
- `make -C src check` green; report `--city`, `--race` (must stay 659.414 s), `--selfcheck`
  counts and the frame count at which the hand-over happened
- no unit tests, no hand-made expected values, no engine change
- card frontmatter: status implemented, branch card/246
