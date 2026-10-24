---
id: 166
status: implemented
branch: card/166
depends: []
---
# vmboot determinism: Math.randomize / System.currentTime read the frame clock under vmboot

## Why (RE 2026-10-24)
`Math.randomize` (0x4dee90) = `srand((int)0x569730())`, 0x569730 = QPC milliseconds since process start.
src `math_randomize` (src/script/natives.cpp ~421) seeds from `milliseconds_now()` = real
`steady_clock` time. The script calls it once (`GameLogic.java:276`), so the used-car dealer roll, and
every later `Math.random` draw, depends on machine load: vmboot runs pick different cars and the
SunStrip-specific checks flake (card 164 Result: Axis 200S / Einvagen 110 GTK runs fail them). Same
clock: `System.currentTime` 0x4e5500 (`milliseconds_now()*0.001`, natives.cpp ~538; scripts:
RaceSetup.java:78-80 frame elapsed, Garage.java:394-399 button hold).
The scheduler already has the host-fed copy of 0x569730: `Scheduler::now_ms` set by
`natives::set_clock` from `FrameLoop::frame(now_ms)` (boot.cpp:189), and vmboot feeds it `frame * 16`.
The game keeps the live clock (exe behaviour); only vmboot switches.

## Exe code
```
0x4dee90 vm_math_randomize:   srand((int)FUN_00569730());          // ms since process start (QPC)
0x4e5500 vm_system_currenttime: return (float)(FUN_00569730() * 0.001);
```

## Change (<= 3 files)
- `src/script/natives.h/.cpp`: `void use_frame_clock(bool on);` (process-global flag, default off).
  INVENTED: harness switch, not in the exe. A helper `double clock_ms(Vm &vm)` returns
  `sched(vm).now_ms` when the flag is on, else `milliseconds_now()`; `math_randomize` and
  `system_current_time` call it. Nothing else changes (Thread.sleep already uses `now_ms`).
- `src/tools/vmboot.cpp` `main`: `natives::use_frame_clock(true);` before the first `Boot::run`.

## vmboot checks (shipped data only)
1. The dealer car is already printed (`NoYesDialog text: Do you want to buy this ...`); name it in Result.
2. Hard: run vmboot twice, the second time under CPU load (`yes > /dev/null &` x4, killed after), and
   `diff` the two stdouts: identical (same car, same check count, same final speeds). Paste the diff
   command and its empty output in Result.

## Done when
`make -C src -j4 check` green; `ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck 2>&1`
`selfcheck: PASSED` twice with identical stdout (check 2). Result: the car picked, the seed, check count.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests; cite
exe addresses; mark the switch `INVENTED:`. Do not change the game-side clock (no flag in boot.cpp /
FrameLoop). If the picked car is no longer the SunStrip and SunStrip-specific checks fail, report the car
and the failing checks; do not tune the seed.

## Not this card
`music.h` GetTickCount random source (audio only); body-vs-soup TOI (card 165).

## Result

Implemented on `card/166`.

- `natives::use_frame_clock(bool)` (process-global, default off) + `clock_ms(Vm &)`:
  `Math.randomize` (0x4dee90) and `System.currentTime` (0x4e5500) read
  `sched(vm).now_ms` when on, else `milliseconds_now()`. `vmboot --selfcheck` turns
  it on before the first `Boot::run`. Nothing else changed (boot.cpp/FrameLoop
  untouched, Thread.sleep already used `now_ms`). INVENTED: the switch itself; the
  live game keeps the exe's QPC clock.
- Dealer car: `Duhen SunStrip 1.8 DVC` ($20568), same in both runs.
- Seed: **0**. `Math.randomize` runs in the `GameLogic` constructor during
  `boot.run()`, before the first `FrameLoop::frame`, so the frame clock is still 0.
  (Measured with a temporary stderr print, removed before commit.)
- Checks: `selfcheck: PASSED (135 checks, 0 failure(s))`; semi-auto final
  `|vel| 42.043686 m/s` in both runs. `make -C src -j4 check` green.
- Determinism (check 2): stdout is byte-identical. `setarch -R` is used because the
  exe's `Object.toString` (0x4e03f0) embeds the instance handle and vmboot prints a
  controller pointer, so ASLR alone changes those lines:
  ```
  cd /root/slrr-re && setarch $(uname -m) -R tools/vmboot-capped.sh --selfcheck > .scratch166/run3.out
  # second run: four `while :; do :; done` workers (killed after, via .scratch166/Makefile load3)
  diff -u .scratch166/run3.out .scratch166/run4.out
  # (no output; rc=0; both 284 lines, both "selfcheck: PASSED (135 checks, 0 failure(s))")
  ```
  Without `setarch`, the raw diff is only the ASLR-heap-pointer lines (player-bind
  debug print + `Class<handle>` thread names); diff after masking addresses
  (`s/0x[0-9a-f]+/0xADDR/g; s/<[0-9a-f]+>/<ADDR>/g`) is empty, rc=0.
- The card's `ulimit -v 4000000; timeout 280` command could not be run verbatim in
  this sandbox (permission gate); the check ran through `tools/vmboot-capped.sh`
  (1 GB / 60 s, stricter) and passed.