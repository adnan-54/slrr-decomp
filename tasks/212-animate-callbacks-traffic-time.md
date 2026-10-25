---
id: 212
status: implemented
branch: card/212
depends: [211]
---
# Game: animate callback walk (0x47ba10) + ground traffic time step

## Goal
Port the per-frame animate pass of the main loop: `Game__runAnimateCallbacks` 0x47ba10 walks the
mode-0x1c callback list once per frame; for the City ground (registered by card 210) it calls
type_ground vf13 0x4bac10, whose traffic step 0x41b290 sets `Traffic+0x84` (sim_time) to the
process world time. src has no event heap, active lists, car pool or spawn queue yet (their
producers are the unported car paths, notes/28 §3), so only the time write is ported; the rest
is one `shortcut:` line each.

## Exe evidence
### 0x47d760 Game__mainLoop (per rendered frame, order)
```
if (now - last_ctrl > 15) { input; 0x47e050 physics tick; Game__runTimers; }   // FrameLoop: control gate
if (now - last_vm > 7)  { 0x55cf20 VM threads }                                // FrameLoop: VM gate
Game__runAnimateCallbacks(min(frame_dt, 0.1f /*0x5da468*/));                   // 0x47ba10  <- NEW
Game__flushDeferred();                                                         // 0x47d070 (FrameLoop already)
... listener/voices, Frontend.render notify (FrameLoop already)
```
`frame_dt` = (now_ms - previous frame's now_ms) * 0.001 (DAT_005f0c88).
### 0x47ba10 Game__runAnimateCallbacks(dt)
```
for node in mode-0x1c list (in order; next is read before the body runs):
    if (node.handle.id == 0) { delete node; count--; continue; }
    inst = node.handle.target; if (!inst || !resident(inst)) continue;   // inst+0x7c == 1
    if (inst has no native object) { if (inst has a script object) call script "animate"(); }
    else native type vf13(native_obj, dt);
```
### 0x4bac10 type_ground vf13 (traffic part only)
```
if (ground+0x8c pedestrians) {...}           // shortcut: pedestrians not ported
if (ground+0x88 T) { 0x41a9d0(T); 0x41b290(T, *0x5dadf0 /*process world time*/); 0x417c90(T, 1, cam); }
```
### 0x41b290 Traffic step(t)
```
T+0x84 (sim_time) = t;
if (T+0x3118 player pseudo-car) ...          // null, as in Graph::init
event heap / 4 active lists / car notify list // not in src Graph (empty without cars)
```
0x41a9d0 (reap dead cars) and 0x417c90 (pending spawns) work only on the car pool: no-op in src.

## Files
- edit: src/game/runtime.h / runtime.cpp: `void run_animate_callbacks(float dt)` (0x47ba10) over
  `callbacks_` entries with `mode == 0x1c`: erase nodes whose `handle.id == 0` (this replaces the
  `shortcut:` erase in `unregister_callback` only for mode 0x1c; leave that function as is);
  for a live node whose target has `traffic` set, call `traffic->step(process_world().time)`.
  Any other target: `// shortcut: script "animate" and other native vf13 not dispatched; no
  shipped script registers mode 0x1c (only Chassis.java calls unregisterCallbacks)`.
  `process_world()` is already used in runtime.cpp load_ground.
- edit: src/game/traffic.h / traffic.cpp: `void Graph::step(float t)` // 0x41b290: `sim_time = t;`
  plus one `shortcut:` line for the heap/lists/car notify, and a `// 0x4bac10` note at the caller.
- edit: src/app/boot.cpp `FrameLoop::frame`: call `rt->run_animate_callbacks(std::min(frame_dt, 0.1f))`
  right before the per-frame `rt->flush_deferred()` (0x47d070). Keep the previous frame's
  `now_ms` in a new member for `frame_dt` (add it to boot.h; first frame: dt 0).
- edit: src/tools/vmboot.cpp city block (card 211): check `G.sim_time == process_world().time`
  after the run (0x41b290 writes the world time every frame; exact float equality).

## Done when
- each ported function has its `// 0xADDR` comment; guards and harness bits are `INVENTED:`.
- `make -C src clean check` green; `--selfcheck`/`--race` output unchanged (no 0x1c node exists
  outside the City); the new `--city` check passes.
- card frontmatter: status implemented, branch name.
