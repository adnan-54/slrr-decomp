---
id: 044
status: merged
branch: card/044
depends: [043]
---
# app: FrameLoop — main-loop control gate (0x47d760) driving input::update

## Goal
There is no main loop in `src/` yet. Add the smallest piece of the exe's main loop
`0x47d760` that the input layer needs: the ">15 ms control gate" that calls the
input frame `0x566160` (`input::update`, card 043). The other stages of the gate
(sim step 0x47e050, script timers 0x47b700) and the rest of the loop are not
ported; they stay `shortcut:` comments so later cards slot in at the right place.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x47d760 main loop (control gate only)
    last_control = (int)clock_ms();        // 0x569730, at loop entry (local_3c)
    loop {
        if (quit) break;
        now = (int)clock_ms();
        ... event/resource pump 0x4563d0/0x4562c0/0x4563c0, message pump 0x56a1c0 ...
        if (now - last_control > 15) {     // "0xf < iVar8 - local_3c"
            saved = frame_dt;              // DAT_005f0c88
            frame_dt = (float)(now - last_control) * 0.001f;
            0x566160(frame_dt);            // input frame  -> input::update
            0x47e050();                    // sim step (physics, game clock)
            0x47b700();                    // script timers -> Game__queueEvent
            frame_dt = saved;
            last_control = now;
        }
        ... 0x55cf20 (>7 ms), 0x47ba10 animate, 0x47d070 deletes, camera/listener,
            streaming 0x517f60, frontend render, present (fps_lock) ...
    }
Input init `0x565fc0` runs once at startup before the loop (card 020's
`input::install_backend` is its counterpart; the host calls it).

## Changes
1. `src/app/boot.h` (after `class Boot`):
   ```cpp
   // Main-loop control gate of 0x47d760: every frame with more than 15 ms since
   // the last control step runs input::update (0x566160) with that interval.
   class FrameLoop {
     public:
       explicit FrameLoop(int start_ms) : last_control_ms_(start_ms) {}
       // now_ms: the 0x569730 millisecond clock truncated to int.
       void frame(int now_ms);
       int last_control_ms() const { return last_control_ms_; }
     private:
       int last_control_ms_;
   };
   ```
2. `src/app/boot.cpp`: `#include "input/controller.h"`; implement
   ```cpp
   // 0x47d760 control gate.
   void FrameLoop::frame(int now_ms)
   {
       if (now_ms - last_control_ms_ > 15) {
           input::update((float)(now_ms - last_control_ms_) * 0.001f);  // 0x566160
           // shortcut: sim step 0x47e050 and script timers 0x47b700 not ported; add them here.
           last_control_ms_ = now_ms;
       }
   }
   ```
   Keep the arithmetic in this order (int difference, then float multiply).

## Files
- edit: src/app/boot.h, src/app/boot.cpp
- read only: src/input/controller.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
