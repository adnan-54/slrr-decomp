---
id: 073
status: todo
branch:
depends: []
---
# game/script: GameType.addTimer / removeAllTimers + timer pass in the frame loop

## Goal
Port the script timer list of the Game object (global 0x5fde90, list sentinels at
+0x180/+0x1a4, count +0x228): `GameType.addTimer(float deadline, int timerid)` (0x4da3c0)
appends a one-shot `TimedEvent`, `GameType.removeAllTimers()` (0x4da930) cancels the
receiver's timers, and the timer pass 0x47b700 fires due timers as `EVENT_TIME` (0x80)
events with `param = timerid` through the existing `Runtime::queue_event` (0x47c760). The
main loop runs the pass every control step right after the sim step (notes/00 "Main loop",
`FrameLoop::frame` already has the placeholder comment). Example user: `game/Bot.java`
`setEventMask(EVENT_TIME); addTimer(3.0, 0);` then `handleEvent(..., EVENT_TIME, ...)`.

## Exe evidence (Claude-traced, asm/decompile checked)
Node = `TimedEvent` (0x48 bytes, ctor 0x4e7530, base `ResourceRefDLNode` ctor 0x47b090):
`+0 vtbl, +4 next, +8 prev, +0x24 embedded Handle {+0x24 link, +0x28, +0x2c id, +0x30 Resource*}`
(0x47b090 binds it to the given handle's resource via 0x40cac0), `+0x34 due` (float),
`+0x38 period` (ctor: 0), `+0x3c flags`, `+0x40 param`, `+0x44 method` (strdup or 0).

### 0x4da3c0 GameType.addTimer(FI)V
    getArgs(ctx, &self, &deadline, &timerid);
    h = Native__instanceToHandle(self);          // 0x4ecc70
    if (h == 0) { log "vm_gameref_addtimer: scripted resource %x not found in file ..."; return; }
    // asm 0x4da3f1..0x4da412: ECX = h
    Handle__addTimer(h, *g_simTime + deadline, 0x80000080, timerid, 0);   // 0x4e7a60
`g_simTime` = `*PTR_DAT_005dadf0` = float at 0x60251c, the same clock `System.simTime`
0x4e5810 returns (src: `Scheduler::sim_seconds`).

### 0x4e7a60 Handle__addTimer(due, flags, param, method), ECX = handle
    if (h->id (+8) == 0) return 0;
    n = new TimedEvent(h, due, flags, param, method);   // 0x4e7530: period = 0
    append n at the list tail (DAT_005fe03c = Game+0x1ac); count(+0x228)++;
    return 1;

### 0x4da930 GameType.removeAllTimers()V
    getArgs(ctx, &self);
    h = Native__instanceToHandle(self);
    if (h == 0) { log "vm_gameref_delalltimers: scripted resource %x not found in file ..."; return; }
    0x4e93c0(ECX = h):
      if (h->id == 0) return;
      for (n = first; n; n = next)             // list head DAT_005fe014 = Game+0x184
          if (n->handle.id == h->id) { unbind n->handle (0x40cac0 inverse); n->handle.id = 0; }
      // nodes stay in the list; the timer pass deletes them

### 0x47b700 Game__runTimers (ECX = Game)
    n = first;
    if (list not empty)
      while (n) {
        next = n->next; if (next is the tail sentinel) next = 0;   // taken BEFORE dispatch
        if (n->handle.id == 0) { delete n; count--; }
        else if (n->due <= *g_simTime) {
            Game__queueEvent(&n->handle, /*sender*/0, n->flags & 0x0fffffff, n->param, n->method);  // 0x47c760
            if ((int)n->flags < 0) { delete n; count--; }          // one-shot (sign bit)
            else n->due = n->period + n->due;
        }
        n = next;
      }
Consequence of `next` being read before dispatch: a timer appended during the dispatch of
the last node is not visited in this pass; appended while a later node exists it is.

## Changes
1. `src/game/runtime.h`, public section of `Runtime`, after `queue_event`:
   ```cpp
   // TimedEvent (0x4e7530): Game timer list +0x180, appended by 0x4e7a60.
   struct Timer {
       Handle handle;          // +0x24 own ref, bound to the receiver's resource
       float due = 0.0f;       // +0x34
       float period = 0.0f;    // +0x38 (ctor 0)
       uint32_t flags = 0;     // +0x3c (addTimer: 0x80000080, sign bit = one-shot)
       int32_t param = 0;      // +0x40 timerid
   };
   // 0x4e7a60: false when h has no id.
   bool add_timer(Handle *h, float due, uint32_t flags, int32_t param);
   // 0x4e93c0: unbind every timer whose handle id equals h->id.
   void remove_timers(Handle *h);
   // 0x47b700: fire due timers at sim time now.
   void run_timers(float now);
   ```
   private: `std::list<std::unique_ptr<Timer>> timers_;` (`#include <list>`; unique_ptr so the
   Handle address stays stable for bind). `~Runtime` must unlink each timer handle before
   the resources go away (`unlink(&t->handle)`), INVENTED cleanup.
   The +0x44 method string is always 0 from addTimer: `shortcut:` not modelled (pass
   `nullptr` to `queue_event`).
2. `src/game/runtime.cpp`: implement the three, citing the addresses above.
   - `add_timer`: `if (!h || h->id == 0) return false;` make node, `bind(&t->handle, h->target)`
     (0x47b090 binds to the handle's resource), set fields, `timers_.push_back`.
   - `remove_timers`: `if (!h || h->id == 0) return;` for each t with `t->handle.id == h->id`:
     `unlink(&t->handle)` (unlink already zeroes id/target).
   - `run_timers(now)`: iterate `it`; compute `nx = std::next(it)` and `last = (nx == end)`
     before dispatch; id 0 -> erase; else if `t->due <= now` -> `queue_event(t->handle.target,
     nullptr, t->flags & 0x0fffffffu, Value::make_int(t->param))`, then one-shot -> erase, else
     `t->due = t->period + t->due`; `if (last) break; it = nx;`. Note queue_event may run
     script code that calls add_timer/remove_timers (std::list keeps `nx` valid; nothing else
     erases during the pass).
3. `src/script/natives.cpp` + `src/script/natives.h`:
   - natives.h: `double sim_time(Vm &vm);` (declared in `namespace natives`); natives.cpp
     defines it as `return sched(vm).sim_seconds;` (0x60251c, same clock as
     `system_sim_time`). Put the definition inside `namespace natives` (outside the anonymous
     namespace if the helpers live in one; check how `table()` is placed).
   - `gt_add_timer` (0x4da3c0): `need_rt`, `receiver_object`, `want(...,3)`, `get_f(a[1])`,
     `get_i(a[2])`; `h = value_handle(a[0])`; `!h` -> `std::fprintf(stderr,
     "vm_gameref_addtimer: scripted resource not found\n")` (same style as `rr_unload`), return
     null; else `game_rt()->add_timer(h, (float)sched(vm).sim_seconds + deadline, 0x80000080u,
     timerid)`. Returns null (void).
   - `gt_remove_all_timers` (0x4da930): same shape, message
     `"vm_gameref_delalltimers: scripted resource not found\n"`, then `remove_timers(h)`.
   - registry rows `"java.lang.GameType", "addTimer"` and `"removeAllTimers"`: point at the
     new functions, `engine = false`.
4. `src/app/boot.cpp` `FrameLoop::frame`: replace the placeholder comment with
   `// shortcut: sim step 0x47e050 not ported.` and, after `input::update`, run
   `if (Runtime *rt = Runtime::active()) if (rt->vm()) rt->run_timers((float)natives::sim_time(*rt->vm()));  // 0x47b700`
   (add the includes `game/runtime.h`, `script/natives.h` if missing).

## Files
- edit: `src/game/runtime.h`, `src/game/runtime.cpp` (Timer, add/remove/run_timers, dtor)
- edit: `src/script/natives.cpp`, `src/script/natives.h` (sim_time, gt_add_timer,
  gt_remove_all_timers, two registry rows)
- edit: `src/app/boot.cpp` (`FrameLoop::frame`)
- read only: `src/game/runtime.cpp` (`unlink`, `bind`, `queue_event`), `src/script/natives.cpp`
  (`gt_set_event_mask` as the shape to copy, `rr_unload` log style)

## Done when
- every ported function carries its `// 0xADDR Name` comment; untraced logic is `INVENTED:`
  (dtor cleanup) and the unmodelled method string is `shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
