---
id: 166
status: todo
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
