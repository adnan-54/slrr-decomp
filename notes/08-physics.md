# Physics engine (in-house rigid-body world) — first map

Found from the `PhysicsRef` natives (see `06-native-methods.md`). All addresses observed in decompiled code; names are mine.

## Entry
Main loop (`FUN_0047d760`, `notes/00-overview.md`) → every >15 ms `FUN_0047e050(dt)` → **`PhysicsWorld::step(dt)` = `FUN_00502060`** (thiscall, ECX = world; only caller is 0x47e050). `FUN_00502370(dt, body)` is the single-body variant (`FUN_0047bc80/0047be90` pre/post callbacks, `4f8460` forces, `5001c0` integrate, `504090`, `502530`).

## World::step (0x502060)
1. `world[1] += dt` (accumulator); if `< 0.0001` return 0. On `DAT_006052ac` set, clear `+0xb8..0xc0` of every constraint in the list at `world[0x76]` (reset flag).
2. `world[1] -= dt`, `world[3] = dt`, step counter `DAT_006028e0++`, zero iteration counters `world[4]`, `[5]`, `[0xb]`.
3. For every **root body** (children of `world[0x21]` via `+0x24` first child / `+0x28` next sibling): `FUN_004f8460(body)` (apply forces/gravity/aero/vehicle model), then `FUN_004ffca0()`, `FUN_0047be30(dt)` (script/pre-step callbacks), then `FUN_005001c0(body, dt)` (integrate), `FUN_004fa4d0()` (broad phase / contact generation setup).
4. **Sub-iteration loop (max 0x13 = 19 iterations)**: `world[4]++`; `world[0xe7] = 1` (= "converged"); `FUN_00502410()` (solve pass); if not converged `FUN_004f9af0()` and call vfunc `+0x10` on each constraint in the `world[0x76]` list (sum of returned counts); `FUN_005040f0()`, `FUN_004fc610()` (contact resolve; timed by `FUN_0056a230/0056a260` perf counters into `DAT_006052b4/b8`), `FUN_004ffd20()` (convergence check); re-integrate roots (`FUN_005001c0`); loop until `world[0xe7]==0` converged or 19 iterations.
5. Finish: `FUN_004fa900()`, `FUN_00502530(body)` for each root, `world[0] += dt` (simulated time), running average of iteration count `DAT_006028e4 += (iters - DAT_006028e4) * 0.1`, return 1.

## Body layout (`FUN_005001c0`, `504180`, `5030b0`)
| off | meaning |
|---|---|
| `+0x1c` | local transform (for compound children: offset `x,y,z` at `+4,+8,+0xc`, quaternion/matrix at `+0x10`) |
| `+0x20` | parent body (compound) |
| `+0x24` / `+0x28` | first child / next sibling |
| `+0x50` | linked game/scene node; `+0x54` spatial (bounds) node (`FUN_00504180` writes AABB: kind=1, center, half extents = |Δpos|/2 + radius, from both state buffers) |
| `+0x58` | flags: bit0 active, bit2 child-of-compound, bit4 compound parent (mass-weighted centre, `m = r²·3.14`), bit7 excluded |
| `+0x5c` | body definition: `+0x2c` mass-ish (0 → static, not integrated), `+0x90 -> +0xc` bounding radius, `+0xb4` flags (bit2 = no integration) |
| `+0x64 + i*0xd4` | state *i* position (xyz), `+0xa4..` 3×4 orientation rows, `+0xf8` quaternion; state stride **0xd4**; two states, active index `+0x20c`; `^1` is the previous/other buffer |
| `FUN_00506530(cur, prev, dt)` | integrates state (called with active and the other buffer) |

Compound bodies: child world pose = parent pose ∘ local (`FUN_005030b0`, `005001c0` bit2 path).

## Link to scripting
`PhysicsRef.create/createBox/createSphere` build bodies via `FUN_004f7b50/4f7d60` (generic, box) and attach them to the scripted resource (`FUN_004e8a50`). Scripts reach positions through `FUN_004eabc0` (GameRef → physics body, resource type `0xa0000000`).

## Next
Decompile `FUN_004f8460` (force model), `FUN_00502410`/`FUN_004fc610` (solver/contacts), `FUN_00506530` (integrator), and the vehicle code under the ~100 callers of `FUN_004eabc0` in 0x488000–0x4d5000.
