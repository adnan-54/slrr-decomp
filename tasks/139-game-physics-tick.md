---
id: 139
status: implemented
branch: card/139
depends: [138]
---
# Game__physicsTick 0x47e050: sub-step the process world, timers + flush per sub-step, simTime = world time

## Why
src `natives::advance_sim` ports only the clock of 0x47e050 (whole capped step added to a double
`sim_seconds`), and `FrameLoop::frame` (src/app/boot.cpp:190-200) runs flush, advance, timers
once per control step. The exe sub-steps the process world (card 138) and runs timers and the
deferred flush after **every** sub-step; `System.simTime` 0x4e5810 reads the world time `w[0]`
(float at 0x60251c). RE: notes/08 "Process-wide world and Game__physicsTick 0x47e050".

## Exe functions (4)
- `Game__physicsTick` 0x47e050 (ECX = game; consts 0x5da468 = 0.1f, 0x5938c4 = 0.0001f,
  0x593590 = 0.5f, world `+8` = 0.02f):
```
step = min(game+0x27c /*warp*/ * frameDt, 0.1f)
ctrlAcc += step; if (ctrlAcc > 0.0001f) { 0x47bc20(ctrlAcc); ctrlAcc = 0 }   // control callbacks: NOT this card
done = 0; nominal = world.step_size
if (!quit /*0x5f0c74*/) {
  0x4f8450: DAT_006052ac = 1                                                  // not this card
  while (step > 0) {
    if (quit) break
    sub = step
    if (step >= nominal) sub = (step - nominal > nominal) ? nominal : step * 0.5f
    if (!world.step(sub)) break                // 0x502060
    step -= sub; done += sub
    Game__runTimers()                          // 0x47b700, sees world.time
    Game__flushDeferred()                      // 0x47d070
  }
}
if (!(done > 0)) Game__flushDeferred()         // one flush when nothing stepped
// perf avg world+0x20, frame counter 0x5f1e40, camera snapshots DAT_005f1e44: not this card
```
- `Game__runTimers` 0x47b700 and `Game__flushDeferred` 0x47d070: already ported
  (`Runtime::run_timers(now)`, `Runtime::flush_deferred()`); only their call site moves.
- `System.simTime` 0x4e5810: returns `*(float*)0x60251c` = `process_world().time`.

## Files (3)
- src/script/natives.cpp: `advance_sim(vm, dt)` becomes the loop above, using
  `physics::process_world()`, `game_rt()->run_timers(world.time)` and
  `game_rt()->flush_deferred()` (null-check game_rt like the other natives). Keep the warp field
  (`s.warp`) as `game+0x27c`. Drop `sim_seconds`: `sim_time()`, `System.simTime` (~line 819)
  and `addTimer` (~line 2524) read `process_world().time`. Delete the old "shortcut:" comment;
  add one `shortcut:` line naming what stays unported (control/drive callback lists
  0x47bc20/0x47be30, perf, camera snapshots).
- src/app/boot.cpp `FrameLoop::frame`: inside the >=16 ms gate keep `input::update(dt)`, then
  only `natives::advance_sim(*rt->vm(), dt)` (it now flushes and runs timers itself); remove the
  extra `rt->flush_deferred()` and `rt->run_timers(...)` there. Keep the per-frame flush outside
  the gate (0x47d760). If `rt` has no VM, call `rt->flush_deferred()` once (the exe's
  no-step path).
- src/tools/vmboot.cpp: hard checks after the run: `process_world().time > 0`;
  `natives::sim_time(vm)` equals `process_world().time` (same float); still `failed: 0`,
  `final actualState: java.game.Track`, all existing timer-dependent checks still pass.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, the new
  checks ok. Report the world time after 30000 frames vs. the old `sim_seconds` value (they
  should agree to float precision: same 0.1 cap, the split only re-partitions the step).

## Next (not this card)
Car body into the world (`Vehicle__createCar` 0x4992e0 is reached only via a vtable: find the
slot), mode-8/9 callback consumers (0x47bc20/0x47bc80 "control", 0x47be30/0x47be90 "drive",
registered by natives through `registerCallback` 0x47d670), native ground `type_ground__vf3`
0x4bade0.
