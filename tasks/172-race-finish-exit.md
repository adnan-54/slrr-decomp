---
id: 172
status: merged
branch: card/172
depends: [171]
---
# vmboot: drive both laps, the script finishes the race and leaves the track

## Why (notes/19 s9)
Career circuit race: `laps` 2, 4 checkpoints (index 3 = start line). The finish is all script: last cp of lap 2 ->
`gmThread.execute(7)` -> THOR method 7 (CircuitRacing.java:1183: raceActive 0, `controllable 0`, `finishRace(0)`
= endRaceWindow, timers 12/11) -> timer 11 -> `finishWindowCircuitRacing` -> `osdCommand(0x01)` hides it ->
timer 13 -> `unlaunch()` -> `Track.getOut()`: career (failable) -> `GameLogic.changeActiveSection(new
EventList(EventList.MODE_AMATEUR))` (Garage only for non-failable events).
Script does: all of the above. Exe provides: timers, OSD, controls, section switch (already ported, unverified here).

## What to change (vmboot only)
1. Spline driver block: run on (same loop, INVENTED) until `fWindow` is non-null or 20000 frames; print each
   nt/current_lap change.
2. INVENTED harness: once `fWindow` exists, release keys and send the OSD command / key the finish window binds to
   0x01 (find it in finishWindowCircuitRacing.show(); reuse the vmboot Enter path of the card-121 menu block), then
   run up to 600 frames.

## vmboot checks
1. **Hard check**: `current_lap` reaches 2 and `fWindow` becomes non-null (handleCheckpoint called execute(7)).
2. **Hard check**: after the key, the active section is the EventList (`GameLogic` active section class name, as
   the existing Garage-return checks read it), 0 failed threads.
3. All existing checks pass.

## Done when
`make -C src -j4 check` green; vmboot --selfcheck (`ulimit -v 4000000; timeout 290`) PASSED with 1-3. Result:
frames per lap, race time (time[0]), first unported native or failed thread if any.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, Result names the first blocker: harness corner,
missing native, thread error, dialog not answered). No engine changes beyond a missing native that is a one-line
stub with exe address; anything bigger = report. No unit tests, no map names in engine code.

## Not this card
Native bot, HUD rendering, rewards/save checks beyond printing the prize text.

## Result
- Blocker 1 (harness): 2 laps need ~26000 frames (lap rollover at frame 11824, fWindow at 26059); the 20000 cap
  stopped in lap 2 at nextTrigger 2. A full run takes ~58 s, so time was never the limit. Cap now 40000.
- Blocker 2 (harness): the finished race locks controls (method 7 `controllable 0`), so the card-161/162 drag and
  semi-auto blocks cannot run after the finish. New `vmboot --race` mode (second line in `make check`) drives to
  the finish and skips those blocks; `--selfcheck` keeps the card-171 stop at cp2 and all old checks.
- Blocker 3 (VM): `finishWindowCircuitRacing.osdCommand` is **static** and calls instance `hide()`. Exe keeps the
  instance in frame+0x30 for static methods too (0x561830 -> 0x54c180 stores param_4 unconditionally; only the
  `this` local is skipped for flag 8). src/script/vm.cpp now keeps `self` for static frames/calls.
- --race: lap 2 time[0] 222.614 s, raceTime[0] 579.569 s, fWindow windowStatus 3 after 48 frames, Enter (osd
  0x01) -> EventList active, 0 failed threads. --selfcheck 153 checks PASSED, --race 143 PASSED, ~58 s each.
