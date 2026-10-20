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



## State vector (one of two per body, stride 0xd4, base `body+0x64`; offsets as float index `s[i]`)
`s[0..2]` position · `s[3..5]` linear velocity · `s[6..8]` position offset added after integration · `s[9]` local time · `s[0x10..0x1b]` 3×4 orientation (rows at `+0xa4,+0xb4,+0xc4`) · `s[0x1c..0x1e]` angular velocity (`+0xd4`) · `s[0x1f..0x21]` angular momentum (`+0xe0`) · `s[0x25..0x28]` quaternion (`+0xf8`) · `s[0x3x]` inertia-frame copies. Flags word at `s[0xf]` (bit0 = dirty/valid, bit1).

## Integrator `FUN_00506530(cur, next, dt)` (BodyDef = `body+0x5c`)
- Compound child (`body flags & 4`): pose is the parent's pose ∘ local transform (no integration).
- Else, if flag `0x100` is clear: **classic RK4** — derivative `FUN_00505c40(state, h, k_i)` (k1..k4 in globals `DAT_006059c0…`), combination `FUN_00504ca0(y, k, weight, out)` with weights 0.5, 0.5, 1 for the stages and **1/6, 1/3, 1/3, 1/6** for the final sum (`0x3e2aaaab`, `0x3eaaaaab`); then `pos += s[6..8]`, orientation re-orthonormalised (`FUN_005680d0`), quaternion refreshed (`FUN_005665c0`).
- Flag `0x100` (kinematic/scripted): body def flag `0x200` → plain Euler (`pos += v·dt`, `ω` from `BodyDef+0x2c`·dt); otherwise one derivative step.
- Optional extra angular damping when `BodyDef+0x9c < 0`: `1 + mass·coef·dt·100` on the angular momentum.
- **Safety clamps (must be reproduced):** angular speed: `|ω|²·0.0025 > 1` ⇒ scale ω to `|ω|=20`; linear speed: `|v|² > 62500` ⇒ scale to **250 m/s**.

## Force model `FUN_004f8460(body)` (recursive over children; `ECX` = world)
- Skips bodies with flags `&0x81 != 1`, `BodyDef+0xb4 & 4` (no integration) or zero mass non-compound.
- Clears bit0 of the *other* state's flags (`+0x174 - idx*0xd4`).
- Linear drag: `BodyDef+0x98 < 0` ⇒ force `= coef · v`.
- Gravity: if `world+0x78` set and body bit1 clear ⇒ force `= g_vec(world+0x1c4..0x1cc) / mass` (applied via `FUN_00505360`).
- BodyDef flag `0x80`: extra constant acceleration `−BodyDef+0xa0 · world+0x364 · g_dir`.
- BodyDef flag `0x800` with `world+0x80`: **ground/water-plane contact**: tests the body against `world+0x384` planes (stride 6 floats), plane at `world+0x368..` normal `+0x374..`; computes a plane-penetration contact (`FUN_0050f810/0050f8a0`), applies friction/restoring impulse with energy cap (kinetic energy before/after, `world+0x394/0x398` coefficients), and queues an event `FUN_0047d550(node, 0, 0x200, &contactInfo)` (collision event dispatched to script, type `0x200`).

## Constraint solver `FUN_00502410`
Iterates the constraint-group list `world+0xe8` (each group: count at `+0x34/+0x35`): per group virtual calls `+0x1c` (build Jacobian), `+0x20`, then assembles block-sparse matrices with `FUN_00569ea0/569cc0/569db0` (matrix alloc/mul/copy helpers = small linear-algebra library), solves, and applies results with vfunc `+0x24`. Hence a Lagrange-multiplier / LCP-style joint solver (joints = `ResourceType_Physics_Constraint`).

## Next
`FUN_004fc610` (contact resolve), `FUN_004f9af0`, `FUN_004fa4d0/a900` (broad/narrow phase), the `FUN_005680d0`/`FUN_00566xxx` math helpers (matrix/quaternion library — name and reimplement first), and the vehicle code (~100 callers of `FUN_004eabc0` in 0x488000–0x4d5000).

## Math helpers (reimplemented in `src/engine/slrr_math.h`)
Matrices are 3 rows × stride 4 floats (`Mat34`, 12 floats), quaternions `{x,y,z,w}`.
| exe | function |
|---|---|
| 0x5665c0 | quaternion → rotation matrix |
| 0x5680d0 | quaternion normalize (zero → identity) |
| 0x567b40 | quaternion product (7-multiply form) |
| 0x567ea0 | matrix → quaternion (trace branch / largest diagonal) |
| 0x566c10 | 3×3 multiply |
| 0x567380 | 3×3 transpose (in place if same pointer) |
| 0x566830 | 3×3 inverse (adjugate/determinant) |
| 0x5666d0 | Euler → rotation matrix (SSE code; `0x570fcb`=sin, `0x570e7e`=cos inferred) |
