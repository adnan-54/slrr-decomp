---
id: 169
status: todo
branch: card/169
depends: [168]
---
# vmboot spline driver (INVENTED harness): the player car follows the track's AI spline to checkpoint 0

## Why (RE 2026-10-24, notes/19 section 8 "Race bots")
After card 168 the career race (CircuitRacing on Norisring) runs and its triggers fire, but the player car only
drives straight: checkpoint 0 (48.8, -5.7, -542.4) is past the hairpin at z -567, so `nextTrigger[0]` never
advances. In the exe only the `native bot` driver (sl:0x6E "botbrain", 0x4803a0 / 0x480850 / 0x482630 /
0x486020 / 0x483bd0, ~20 functions plus a spline class) steers a car by itself, which is too big for now
(bot cards are listed in notes/19 section 8). Stopgap: a **vmboot-only** driver (`INVENTED:` harness, nothing
in src/ engine code) presses the existing keys to follow the same AI spline file the script hands to the bots.
Script does: checkpoint order, lap counting (CircuitRacing.handleCheckpoint). Exe provides (already ported):
trigger contacts + EVENT_TRIGGER_ON, the controls (Up/Down/Left/Right axes). The harness provides: key presses.

## Facts to use (exe + shipped data, read 2026-10-24)
- Path file = `track_data.splinesAI.elementAt(0)` (Norisring track_data.java:38:
  `"multibot\\maps\\Norisring\\data\\route_1.spl"`; read the string from the live script object, do not
  hardcode the map). The exe loader `0x482630` reads the whole file and scans each line with
  ```
  sscanf(line, "%lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %d", &pos.x,&pos.y,&pos.z, &dir.x,&dir.y,&dir.z,
         &width, ...);   if (fields > 5) { if (!first) 0x4122b0(segment prev -> this); prev = this; }
  ```
  Shipped lines have 7 fields `x y z<TAB>dx dy dz<TAB>width` with CRLF; route_1.spl has 38 points and the last
  equals the first (closed loop). Use only pos x/z.
- Direction: the bot planner `0x486020` looks ahead at `param - d` everywhere (`local_28 - 20.0`,
  `local_28 - 5.1`, `local_28 - (local_28 - local_30) * 0.8`), i.e. bots drive the spline in **decreasing
  index** order. Backwards, route_1 matches the script's raceposData order and passes cp0 at point 28
  (48.3, -538.5) right after the hairpin (points 33..29).
- Keys (shipped active_control_set group 1, notes/09): Up 200 throttle, Down 208 brake, Left 203 steer +1,
  Right 205 steer -1. vmboot already has `key(dik, down)`, `frames(n)`, `restore_spawn()`,
  `game::car_command(..., "reset")`.
- Script success signal (CircuitRacing.java:507-520): `handleCheckpoint` with the player car id and
  `cpTrigger[nextTrigger[0]]` == the sender -> `nextTrigger[0]++` (0 -> 1).

## Files
- `src/tools/vmboot.cpp` only (no engine/src changes; if the car cannot reach cp0 because of an engine bug,
  stop and report, do not patch physics here).

## What to add (vmboot, career-race block)
1. In the existing "Up + Left" control block (card 148) record the mean `root->angular_velocity().y` over its
   60 frames -> `left_yaw_sign = +1/-1`; print it. (Which way Left turns is measured, not assumed.)
2. New block after "Check 5: restore the shipped transmission" and before the card 167 trigger pose re-check:
   `restore_spawn()`, `car_command "reset"`, settle like the semi-auto block. Load the spline (path from the
   script, `fs::read_file` under `kGameDir`, backslashes -> `/`); keep points in **reverse** file order as a
   closed polyline (x/z).
3. Per frame (max 4000 frames, stop early when `nextTrigger[0] >= 1`), all `INVENTED:` (harness):
   - project the car root x/z onto the polyline -> arc position s; target = point at s + L along the reversed
     polyline, `L = clamp(8 + 0.6 * |v|, 8, 30)` m;
   - heading = horizontal velocity direction when |v_h| > 2 m/s (else no steering key);
     `a = atan2(cross(heading, to_target).y, dot(heading, to_target))`;
     press Left if `a * left_yaw_sign > 0.05`, Right if `< -0.05`, neither otherwise;
   - target speed 30 m/s, 12 m/s when |a| > 0.25 rad or the polyline turns > 0.6 rad within the next 40 m;
     Up when `|v| < target`, Down when `|v| > target + 3`, neither otherwise.
   Release all keys at the end. Print: frames used, `nextTrigger[0]`, `rpTotal[0]`, `current_lap`, final pos,
   closest distance to the cp0 sensor body (cpTrigger[0]), max distance from the polyline.

## vmboot checks (shipped data only)
1. `left_yaw_sign` measured with |mean yaw rate| > 0.05 rad/s (harness precondition).
2. **Hard check**: `nextTrigger[0] == 1` within 4000 frames: the script's `handleCheckpoint` accepted cp0 for
   the player car (CircuitRacing.java:513-520) after the hairpin.
3. Every existing check passes (card 168: 145), "no V-Clip overrun in the career-race block" still 0,
   0 failed threads.

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-3 pass. Result lists the printed numbers above.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result: closest cp0 distance, where
the car left the polyline, speed there). Do not tune by trial beyond those 2 runs. No unit tests, no
synthetic inputs; every harness rule carries `INVENTED:`; no map names in code (the path comes from the
script). Do not port script rules (lap counting) or bot logic.

## Not this card
The native bot (`native bot` create/commands/spline class/planner, notes/19 section 8), finishing the lap
(cp1..cp3), finish window, return to Garage.
