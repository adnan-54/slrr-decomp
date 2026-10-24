---
id: 138
status: todo
branch: card/138
depends: [137]
---
# Process-wide PhysicsWorld: init constants, step accumulator/time, head-insert addBody

## Why
Freeride reaches `java.game.Track` (card 137) but nothing simulates: src has no process-wide
physics world. The exe owns exactly one static world (0x60251c), created at engine startup,
before any script. This card adds that owner and makes `World::step` match the exe's
prologue/epilogue; card 139 drives it from the frame loop. RE: notes/08 "Process-wide world
and Game__physicsTick 0x47e050".

## Exe functions (4)
- `Game__init` 0x47cd20: `game+0x27c = 1.0f`; ...; `PhysicsWorld__init` on the static world.
- `PhysicsWorld__init` 0x4fffb0 (only caller 0x47cd20):
```
w[3] = w[2] = 0.02f (0x3ca3d70a); w[0] = 0 (time); w[1] = 0 (accumulator); w[8] = 0
w[0x1e] = w[0x1f] = 1; gravity w[0x71..0x73] = (0, -9.81f (0xc11cf5c3), 0)
w[0xd9] = 1.2935f (0x3fa57a78); plane point (0,-7,0), normal (0,-1,0); w[0xe5]=300, w[0xe6]=550
root bodies w[0x21] (+0x84, parent of all dynamic bodies), w[0x22]: static, def flag |= 0x100
```
- `PhysicsWorld__step` 0x502060, prologue/epilogue only (the middle is already ported):
```
t = dt + w[1]; w[1] = t
if (t < 0.0001f) return 0            // dt is kept in the accumulator
... (DAT_006052ac contact reset: not this card)
w[1] -= t                            // back to 0
w[3] = t
<existing step body with dt = t>
w[0] += t                            // simulated time
return 1
```
- `PhysicsWorld__addBody` 0x4f7830: new body goes to the **head** of root `w[0x21]`'s child
  list (`body+0x28 = root+0x24; root+0x24 = body`) unless `def+0xb4 & 4`; `world+0x54++`.

## Files (3)
- src/physics/physics.h: `World` gets `float time = 0` (w[0]), `float accumulator = 0` (w[1]),
  `float step_size = 0.02f` (w[2]), `float last_dt = 0.02f` (w[3]); declare
  `World &process_world();` (0x60251c, comment `g_physWorld`).
- src/physics/world.cpp: `process_world()` = function-local static `World` (constructed with
  the 0x4fffb0 values above; gravity/air_density already match). `World::step(dt)`: replace
  `if (dt < 0.0001f) return 0; const float h = dt;` by the accumulator prologue, set
  `last_dt = t`, use `h = t`, and add `time += t` before the final `return`.
  `add_body`: `bodies.insert(bodies.begin(), b)` (head insert, 0x4f7830).
- src/tools/vmboot.cpp: selfcheck hard checks (values are exe constants, cite 0x4fffb0):
  `process_world()` step_size bits == 0x3ca3d70a, gravity.y bits == 0xc11cf5c3,
  air_density bits == 0x3fa57a78, time == 0 and accumulator == 0 before the frame loop.

## Not this card
Static root bodies `w[0x21]/w[0x22]` (src `bodies` already stands for root's child list),
plane/300/550 constants (the 0x800 plane path is a TODO in src), the `DAT_006052ac` reset, perf
counters. Mark nothing INVENTED except the function-local static if you need a comment.

## Done when
- `make -C src -j4 check` green (head insert must not break existing physics/vehicle checks;
  if one depends on append order, report it, do not revert the head insert).
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, the 4 new
  checks ok, `final actualState: java.game.Track`, `failed: 0`.
