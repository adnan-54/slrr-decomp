---
id: 102
status: merged
branch: card/102
depends: [101]
---
# script VM: JavaMachine::runThreads (0x55caf0) from the frame loop; sim clock; Frontend render wake

## Goal
Replace src's ad-hoc `pump_threads` with a port of the exe scheduler 0x55caf0 and drive it
from `FrameLoop::frame` the way `Game__mainLoop` 0x47d760 does: VM tick every >= 8 ms
(0x55cf20 -> runThreads(10.0, 0)), sim clock advanced in the >= 16 ms control step
(0x47e050), and the per-frame wake of scripts waiting on `java.render.Frontend.render`
(0x54d3a0). Card 101 already made sleep/suspend/stop flag-based.

## Exe evidence (Claude, asm of 0x47d760 + decompiles)
### 0x47d760 Game__mainLoop (per iteration; now = (int)0x569730() ms)
    if (now - last_ctl >= 16) {                       // cmp eax,0x10; jl
        dt = (now - last_ctl) * 0.001f;  [0x5f0c88] = dt;
        0x566160(dt) input; 0x47e050() sim step; 0x47b700() timers;  last_ctl = now;
    }
    if (now - last_vm >= 8) {                         // cmp eax,8; jl
        if (game+0x234) 0x55cf20(10.0f);              // VM tick, push 0x41200000
        last_vm = now;                                // also when +0x234 is 0
    }
    0x47ba10(min(0.1, dt)) animate; 0x47d070 deferred flush; listener 0x568bb0; 0x5690a0;
    0x517f60; 0x518f80;
    if ([0x5fe0c4] /* JavaMachine */) {
        cls = 0x55b100("java.render.Frontend");       // class lookup, no load
        if (cls) { var = 0x549750(cls, "render");     // class static init + static var
                   if (var->value (+8)) 0x54d3a0(ECX = value); }   // Monitor__notifyAll
    }
    ... render when now - last_render >= fps_lock.
### 0x55cf20 JavaMachine::tick(budget)   = GC work (0x559e80..0x55a560, not ported) then
    runThreads(budget, 0).
### 0x55caf0 JavaMachine::runThreads(float budget_ms, int min_prio)
    timed = budget != 0; end = now() + budget;
    q = 10.0; if (timed && budget < 10.0) q = budget;           // quantum
    ran = true;
    for (;;) {
        if (timed && end <= now()) return 1;
        if (cursor(+0x78) == 0) {                   // wrap to the run-list head
            cursor = first thread of the run list (vm+0x18) or 0;
            if (!ran) return 1;                     // a full pass ran nothing
            ran = false;
        }
        t = cursor; if (!t) return 0;
        f = t->flags(+0x2c);
        if (!(f & 0x82)) {
            if (!(f & 0x44) && min_prio <= t->prio(+0x28)) {
                if ((f & 8) && t->wake(+0x30) < now()) t->flags &= ~8;
                if (!(t->flags & 8)) {
                    s = t->prio < 0 ? (t->prio + 10) * 0.089999996f + 0.1f
                                    : t->prio * 9.9f + 1.0f;
                    end = (end - q) + s * q;
                    Thread__run(t, s * q);          // 0x562d70, time budget in ms
                    ran = true;
                }
            }
            if ((t->flags & 0x41) == 0x40) t->flags |= 0x80;   // finished -> dead
        }
        cursor = t->next (0 at the list end);
        if (!(t->flags & 4) && (t->flags & 0x80)) delete t;    // vf0(1)
    }
    The run list holds only runnable threads: wait() (0x564950) and suspend (0x564890)
    move a thread to the wait list, notify/resume move it back to the run-list TAIL.
### 0x47e050 sim step (clock part)
    step = min(game+0x27c /* warp */ * dt, [0x5da468] = 0.1f);
    if (!quit [0x5f0c74] && step > 0) sub-steps of the physics world (0x502060); the world
    time *PTR_DAT_005dadf0 (= System.simTime 0x4e5810, 0x60251c) advances by each sub-step.

## Changes
1. `src/script/natives.cpp`:
   - Rename `pump_threads` to `run_threads(Vm &vm, float budget_ms, int min_prio)` and port
     0x55caf0 over `Scheduler::threads` (registration order = run-list order, INVENTED: the
     exe re-appends woken/resumed threads at the tail; keep the existing vector order).
     Skip a thread when `!t->runner || !t->started || t->done || t->waiting || t->suspended
     || t->in_slice` (the exe keeps those off its run list or flags them 0x80/0x40) or
     `t->priority < min_prio`; apply the sleep wake test (`wake_ms < now_ms` clears
     `sleeping`); if still not sleeping run it. Keep the `pumping` re-entry guard,
     `s.current`, the Done/Failed handling (= flags 0x40 -> 0x80).
     shortcut: src has no per-slice wall clock, so one call makes ONE pass and each eligible
     thread gets `run_slice(256)` (the exe slice is `s * q` ms of wall time with the priority
     factor above; keep the formula in a comment with the constants).
   - `Scheduler::now_ms` (card 101) is set by the host through a new
     `void natives::set_clock(Vm &vm, double now_ms)`.
   - New `void natives::monitor_notify_all(Vm &vm, Object *o)` = the body of
     `object_notify_all` after its argument checks (make `object_notify_all` call it).
   - New `void natives::advance_sim(Vm &vm, float dt)`: `if (s.quit_request) return; float
     step = std::min(s.warp * dt, 0.1f); if (step > 0) s.sim_seconds += step;`
     (shortcut: the exe advances the physics world's clock in world sub-steps 0x502060;
     src has no world in the frame loop, so the whole step is added).
2. `src/script/natives.h`: declare `run_threads`, `set_clock`, `monitor_notify_all`,
   `advance_sim` (with their 0xADDR comments).
3. `src/script/vm.h` / `vm.cpp`: add `bool Vm::get_static_field(const std::string &cls,
   const std::string &name, Value *out, std::string *err)`: `find_class`, the file-static
   `find_static` lookup, `ensure_static` (0x549750 runs the class static init first), then
   copy `statics[slot]`. Return false when the class or field is missing.
4. `src/app/boot.h` / `boot.cpp` `FrameLoop`: add `int last_vm_ms_` (init = start_ms like
   `last_control_ms_`). In `frame(now_ms)`:
   - at the top: `if (rt && rt->vm()) natives::set_clock(*rt->vm(), now_ms);`
   - change the control gate to `now_ms - last_control_ms_ >= 16` (same as `> 15`), and in
     it, before `run_timers`, `natives::advance_sim(*rt->vm(), dt)` with dt = elapsed*0.001f.
   - after the control gate: `if (now_ms - last_vm_ms_ >= 8) { if (rt && rt->vm())
     natives::run_threads(*rt->vm(), 10.0f, 0); last_vm_ms_ = now_ms; }`
     (INVENTED: game+0x234 is treated as set whenever a VM exists; 0x55cf20 GC not ported.)
   - after the audio service lines (end of frame): Frontend wake: `Value v; std::string e;
     if (rt && rt->vm() && rt->vm()->find_class("java.render.Frontend") &&
     rt->vm()->get_static_field("java.render.Frontend", "render", &v, &e) && v.kind ==
     Value::Obj && v.obj) natives::monitor_notify_all(*rt->vm(), v.obj);`  (0x47dc80).

## Files
- edit: src/script/natives.cpp, src/script/natives.h, src/script/vm.h, src/script/vm.cpp,
  src/app/boot.h, src/app/boot.cpp

## Done when
- every ported block has `// 0xADDR`; shortcuts/INVENTED marked as above
- `make -C src clean check` green; no new tests
- card frontmatter: status implemented, branch name
