---
id: 116
status: todo
branch:
depends: [115]
---
# vmboot: run the booted game for simulated frames and report script threads

## Goal
After `Boot::game_init()` the shipped GameInit constructor completes (vmboot check
"shipped Init(int) constructor runs to completion"). Nothing drives the game further:
`FrameLoop` (src/app/boot.h/.cpp, 0x47d760 control gate: timers, scheduler pass
`natives::run_threads` every >= 8 ms, sim time) has no host. Add that drive to the vmboot
selfcheck so we see how far the shipped scripts get, like the exe's main loop would.

## Changes (src/tools/vmboot.cpp only; read src/app/boot.h, src/script/natives.h)
1. In the GameInit check function (the one printing `THRD-CREATE: ...`), after it:
   create `FrameLoop loop(0)` and call `loop.frame(t)` for t = 16, 32, ... up to
   `kFrames` (= 600 frames, i.e. ~10 s of game time). Keep it deterministic (simulated
   clock, never the wall clock). Natives that read the clock use what FrameLoop sets.
2. Then print one line per script thread from `natives::thread_report(*boot.vm())`:
   `  thread <name>: <state>` using the same state words as the THRD-CREATE print
   (factor that state-string code into a small static function and use it for both).
   Print at most 40 threads, then `  ... N more`.
3. Print `  frames: <n> run, threads: <total>, failed: <k>`; for failed threads the
   state already contains the error text.
4. Add NO new pass/fail check (the game is far from fully running; this is a report).
   Exception: keep it bounded — if `frame()` itself throws or the process would run away,
   the existing caps of `tools/vmboot-capped.sh` / make check stop it; do not loop more
   than kFrames.

## Done when
- `make -C src clean check` green.
- Report the output of `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E '^  (thread|frames)' | head -50`
  in your final message.
