# Physics engine (in-house rigid-body world) — first map

Found from the `PhysicsRef` natives (see `06-native-methods.md`). All addresses observed in decompiled code; names are mine.

## Entry
Main loop (`FUN_0047d760`, `notes/00-overview.md`) → every >15 ms `FUN_0047e050(dt)` → **`PhysicsWorld::step(dt)` = `FUN_00502060`** (thiscall, ECX = world; only caller is 0x47e050). `FUN_00502370(dt, body)` is the single-body variant (`FUN_0047bc80/0047be90` pre/post callbacks, `4f8460` forces, `5001c0` integrate, `504090`, `502530`).

## World::step (0x502060)
1. `world[1] += dt` (accumulator); if `< 0.0001` return 0. On `DAT_006052ac` set, clear `+0xb8..0xc0` of every constraint in the list at `world[0x76]` (reset flag).
2. `world[1] -= dt`, `world[3] = dt`, step counter `DAT_006028e0++`, zero iteration counters `world[4]`, `[5]`, `[0xb]`.
3. For every **root body** (children of `world[0x21]` via `+0x24` first child / `+0x28` next sibling): `FUN_004f8460(body)` (apply forces/gravity/aero/vehicle model), then `FUN_004ffca0()`, `FUN_0047be30(dt)` (script/pre-step callbacks), then `FUN_005001c0(body, dt)` (integrate), `FUN_004fa4d0()` (broad phase / contact generation setup).
4. **Sub-iteration loop (corrected, see `11-collision-shapes.md`)**: `world[4]++`; `world[0xe7] = 1` ("converged"); `PhysicsWorld__solvePass` (0x502410, per-row constraints of `world+0xe8`); then `PhysicsWorld__updateContacts` (0x4f9af0, may clear `world[0xe7]` when a contact gets within 0.02); if still converged → go straight to the event phase; else sum `vf4` (`c_collision__resolve`) over the contact list `world+0x1d8`: sum==0 → `world[0xe7]=1` and event phase, sum≠0 → keep iterating (event phase only at iteration 19). Event phase: run `PhysicsWorld__purgeEvents` (0x5040f0), `PhysicsWorld__dispatchCollisionEvents` (0x4fc610; perf counters into `DAT_006052b4/b8`) and, if events were dispatched, `PhysicsWorld__resolveImpacts` (0x4ffd20, mode-6 impacts → mode 7; non-zero ⇒ `world[0xe7]=0`). Every sub-iteration re-integrates all roots from the start-of-step state (`PhysBody__integrateTree`). Exit when `world[0xe7]≠0` or `world[4] > 19` (≤ 20 iterations).
5. Finish: `FUN_004fa900()`, `FUN_00502530(body)` for each root, `world[0] += dt` (simulated time), running average of iteration count `DAT_006028e4 += (iters - DAT_006028e4) * 0.1`, return 1.

## Body layout (`FUN_005001c0`, `504180`, `5030b0`)
| off | meaning |
|---|---|
| `+0x1c` | local transform (for compound children: offset `x,y,z` at `+4,+8,+0xc`, quaternion/matrix at `+0x10`) |
| `+0x20` | parent body (compound) |
| `+0x24` / `+0x28` | first child / next sibling |
| `+0x50` | linked game/scene node; `+0x54` spatial (bounds) node (`FUN_00504180` writes AABB: kind=1, center, half extents = |Δpos|/2 + radius, from both state buffers) |
| `+0x58` | flags: bit0 active, bit2 child-of-compound, bit4 compound parent (mass-weighted centre, `m = r²·3.14`), bit7 excluded |
| `+0x5c` | body definition `rigidbody_type`: `+0x2c` **inverse mass** (0 → static), `+0x30` inverse body inertia (3×4), `+0x60` inertia, `+0x90` shape (`+0xc` bounding radius, `+0x10` margin), `+0x98/+0x9c` linear/angular drag (−0.005·m, −0.01·m), `+0xa0` volume, `+0xb4` flags (bit2 = no integration) — see 11 §1 |
| `+0x18` / `+0x210` / `+0x214` | impact-event impulse threshold / earliest contact TOI / that contact (flag 0x400) |
| `+0x64 + i*0xd4` | state *i* position (xyz), body-relative `+0xa4..` = state `+0x40..` 3×4 orientation rows, body `+0x108..` = state `+0xa4..` world inverse inertia, `+0xf8` quaternion; state stride **0xd4**; two states, active index `+0x20c`; `^1` is the previous/other buffer |
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
- BodyDef flag `0x80`: extra constant acceleration `−BodyDef+0xa0 · world+0x364 · g_dir` (= volume × 1.2935 (air density?) × g: buoyancy-like term; `PhysicsWorld__init` 0x4fffb0 sets the constants, gravity (0,−9.81,0), dt 0.02).
- BodyDef flag `0x800` with `world+0x80`: **ground/water-plane contact**: tests the body against `world+0x384` planes (stride 6 floats), plane at `world+0x368..` normal `+0x374..`; computes a plane-penetration contact (`FUN_0050f810/0050f8a0`), applies friction/restoring impulse with energy cap (kinetic energy before/after, `world+0x394/0x398` coefficients), and queues an event `FUN_0047d550(node, 0, 0x200, &contactInfo)` (collision event dispatched to script, type `0x200`).

## Constraint solver `0x502410` = `PhysicsWorld__solvePass` (corrected)
Not an LCP/global solve: it gathers each constraint's 1-row rhs/λ vectors into world vectors (`world+0x3a0/+0x3a8`; an N×N matrix at `+0x3b0` is sized but unused), with vf7 = rhs, vf8 = λ = rhs/K, vf9 = apply (impulse for mode 6, force λ/dt otherwise). The real contact response is the per-contact sequential impulse/force routine `c_collision__resolve` (0x4f0dd0): restitution impulse for approach speeds > 0.2 m/s, Baumgarte-like bias (0.03 m slop, gain 10) as a force, Coulomb friction as a force capped by μ·N, and a 0.021 m position-offset correction. Full formulas, contact fields and constants: **`notes/11-collision-shapes.md` §5**.

## Next
`FUN_004fc610` (contact resolve), `FUN_004f9af0`, `FUN_004fa4d0/a900` (broad/narrow phase), the `FUN_005680d0`/`FUN_00566xxx` math helpers (matrix/quaternion library — name and reimplement first), and the vehicle code (~100 callers of `FUN_004eabc0` in 0x488000–0x4d5000).

## Math helpers (reimplemented in `src/engine/math.h`)
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

## Contacts (corrected naming)
- **`FUN_004f9af0` = narrow-phase / contact refresh** (ECX = world). Walks the contact-pair list `world+0x1d8` (doubly linked, sentinel-terminated; each node is a polymorphic pair object: vfunc `+0x18` "still valid?", `+8` unlink; world counters `+0x60` live pairs, `+0x1c` removed). Pair fields: `+0x48` body A, `+0x50` body B, `+0x58` optional override shape (shape at `+0x2c`), `+0x64/+0x68` cached V-Clip features A/B, `+0x74`/`+0x80` local contact points A/B, `+0x8c` normal (B→A), `+0x98` world point, `+0xa4` separation (pairs are `c_collision` objects, see 11 §5). Uses the *previous* state of each body (`idx^1`) and the root body's definition flag `(~BodyDef+0xb4 >> 2) & 1` (≠ "no integration") to decide whether the pair matters.
  - Calls the shape-distance routine **`FUN_0050d9d0(stateA, stateB, shapeA, shapeB, outPtA, outPtB, ..., outNormal)`** → signed separation. Shapes have a margin/radius at `+0x10`. A pair is dropped when `dist > rA + rB + 0.05`.
  - If `dist < rA + rB + 0.02` the pair is in contact: clears bit0 of the other-state flags of both root bodies (wake/dirty).
  - If penetrating (`dist <= rA + rB`) it runs a **10-step bisection in time** (`FUN_00506c00(prev, cur, out, t)` interpolates both bodies between previous and current state; `t` starts 0.5, step 0.25, halves each time) to find the time of first touch, stores resulting contact points/normal, then offsets the points by the radii along the normal (`±rA`, `+rB`) and stores the final distance in `+0xa4`.
- `FUN_004fa4d0`: prunes constraint objects in `world+0xe8` whose vfunc `+0x18` says dead (counter `world+0x5c`).
- **`FUN_004fc610` is not a contact solver**: it walks the global collision-event list `DAT_00605230`, checks each with `FUN_004f9750`, bumps `world+0x50`, and **dispatches collision events to the owning script objects** (`FUN_0040cac0`, `FUN_004e9ac0`, `FUN_004edf80`; at most 16 distinct targets per call, de-duplicated), then frees the nodes; returns the sum of `FUN_004ee460` results (events handled). So collision response lives in the script layer via these events, plus the constraint solver above.

## Shape distance dispatcher `FUN_0050d9d0` (observed; kinds decoded in `11-collision-shapes.md`: **1 = vertex, 2 = edge, 3 = face** Voronoi features of a V-Clip polyhedron, not shape types)
`dist = shapeDistance(stateA, stateB, shapeA, shapeB, &ptA, &ptB, &outPtA?, &outPtB?, &normal)`; shapes are polymorphic records whose first dword is a **shape kind** (1, 2 or 3). `key = kindA*0x100 + kindB`; for mixed pairs the arguments are swapped (`DAT_006060e0` = swap flag, result normal negated afterwards). Lazily initialises each shape's cached data (`FUN_00507ce0`, flag bit0 at shape+0x14/+0x34).
| key | handler (test / distance) |
|---|---|
| 0x101 | `0x50d050` / `0x50ce30` |
| 0x102, 0x201 | `0x50bc90` / `0x50b870` |
| 0x103, 0x301 | `0x50c660` / `0x50c400` |
| 0x202 | `0x509f10` / `0x509130` (returns *squared* distance → `sqrt`, negated if inside) |
| 0x203, 0x302 | `0x50aa60` then segment/plane clip code (`0x50e2d0` transforms points between the two bodies' frames) |
Returns signed separation (negative = penetration); unknown kind pairs return 0. `FUN_00506c00(a, b, out, t)` = state interpolation (`pos = a·t + b·(1-t)`, quaternion slerp `FUN_005681c0`, matrix rebuilt, time lerped). `FUN_004f9750(body)` = collision-event filter: body not flagged `0x200000`, speed² ≤ 10000 (100 m/s), and some attached game object (type `0xa0000000`) with physics data whose flags `&0xa0 == 0`.

## Process-wide world and `Game__physicsTick` 0x47e050 (2026-10-24, card 138/139 RE)
**Owner.** One static `PhysicsWorld` at **0x60251c** (`g_physWorld` = `PTR_DAT_005dadf0`, bytes `1c 25 60 00`). Created once at engine startup by `Game__init` **0x47cd20** (only caller of `PhysicsWorld__init` 0x4fffb0): sets `game+0x27c = 1.0f` (time warp; `System.timeWarp` 0x4e5880 -> 0x47dfd0 changes it), zeroes the game callback counters `+0x214/+0x218/+0x21c`, then `PhysicsWorld__init`. Not per track/map: the world exists from boot, before any script runs.
**`PhysicsWorld__init` 0x4fffb0** (float slots `w[i]`): `w[0]=0` simulated time (read by `System.simTime` 0x4e5810), `w[1]=0` step accumulator, `w[2]=w[3]=0.02` (0x3ca3d70a; `w[2]` = nominal sub-step read by 0x47e050 at `+8`, `w[3]` = last step dt), `w[8]=0` (avg ms per sim-second), `w[0x1e]=w[0x1f]=1` (+0x78 gravity on, +0x7c), gravity `w[0x71..0x73]=(0,-9.81 [0xc11cf5c3],0)`, `w[0xd9]=1.2935` (0x3fa57a78), plane point `w[0xda..0xdc]=(0,-7,0)`, normal `w[0xdd..0xdf]=(0,-1,0)`, `w[0xe5]=300`, `w[0xe6]=550`; two static root bodies from `0x4f7d60(0,…)` + `PhysicsWorld__addBody` 0x4f7830, `w[0x21]` (+0x84, parent of all dynamic bodies) and `w[0x22]` (+0x88), each with def flag `|= 0x100`. `DAT_0060250c/10/14 = &w[0xec]/&w[0xe8]/&w[0xea]`.
**`PhysicsWorld__addBody` 0x4f7830**: alloc 0x218 body (ctor 0x4f72e0), `0x501f30(body, def, a, b)`, `world+0x54 += 1`; if root `w[0x21]` exists and `def+0xb4 & 4 == 0`: **head insert** into root's child list (`body+0x28 = root+0x24; root+0x24 = body; body+0x20 = root`). So step order = reverse creation order (src `World::add_body` appends: differs). Callers: `Vehicle__createCar` 0x4992e0 (no direct callers: reached through a vtable), `PhysicsRef` attach 0x4e8a50 (`addBody` then 0x524620 binds the body to the script resource), and 0x4fffb0.
**`PhysicsWorld__step` 0x502060 prologue/epilogue** (ECX = world): `t = dt + w[1]; w[1] = t; if (t < 0.0001) return 0;` (dt accumulates, nothing runs); if `DAT_006052ac` (set by 0x4f8450 at the start of every 0x47e050 tick) clear `+0xb8/+0xbc/+0xc0` of every node in contact list `w[0x76]` and clear the flag; `w[1] -= t` (back to 0); `w[3] = t`; body of the step as above with dt = `t`; on exit `w[0] += t`, `DAT_006028e4 += (iters − DAT_006028e4)·0.1`, return 1. Between, 0x47be30(t) (ECX = game) walks the game mode-9 callback list (`registerCallback(h, 9)` 0x47d670, count `game+0x218`) and calls 0x47be90 = native `vf+0x30(obj+0x4c, t)` of the bound object or script method `"drive"`; dead nodes (`+0x2c == 0`) are deleted.
**`Game__physicsTick` 0x47e050** (ECX = game, called by the main loop every >15 ms control step):
```
step = min(game+0x27c * frameDt[0x5f0c88], 0.1f [0x5da468])
ctrlAcc[0x5f0c8c] += step
if (ctrlAcc > 0.0001f [0x5938c4]) { 0x47bc20(ctrlAcc); ctrlAcc = 0 }   // mode-8 list (count game+0x214): 0x47bc80 = native vf+0x2c or script "control"(dt)
perfStart(0)                                                         // 0x56a230
done = 0; nominal = world->w[2]                                       // 0.02
if (!quit[0x5f0c74]) { DAT_006052ac = 1 /*0x4f8450*/;
  while (step > 0) { if (quit) break;
    sub = step;
    if (step >= nominal) sub = (step - nominal > nominal) ? nominal : step * 0.5f;  // last split halved
    if (!world->step(sub)) break;          // 0x502060
    step -= sub; done += sub;
    Game__runTimers();                      // 0x47b700
    Game__flushDeferred();                  // 0x47d070
    DAT_005f0c70 = 0; }
}
if (!(done > 0)) Game__flushDeferred();     // exactly one flush when nothing stepped
frameCounter[0x5f1e40] = (frameCounter + 1) % 1000 (cmovge at 999+1)
if (done > 0) world->w[8] += (perfMs(0)/done - w[8]) * 0.1   // 0x56a260, double 0.1 @0x59a460
camera follow snapshots: for i = DAT_005f1e44-1 .. 0 (entries 0x58 @0x5fe120): dead handle -> drop entry; else body = GameRef__getPhysBody 0x4eabc0, copy active-state pos (+0x64), vel (+0x70), rotation rows (+0xa4/+0xb4/+0xc4)
```
Consequences for src: with a 0.1 s cap and 0.02 nominal step 0.1 splits as 0.02, 0.02, 0.02 (remaining-nominal > nominal), then 0.04 → halved 0.02, then 0.02 → halved 0.01, then 0.01 (< nominal) = 6 sub-steps (float rounding can shift the boundary cases). Timers and deferred flush run after **each** sub-step and see the world time `w[0]` that sub-step produced. `System.simTime` = `w[0]` (float), not a separate clock. src today: `natives::advance_sim` adds the whole capped step to a double `sim_seconds`, `FrameLoop::frame` calls flush, advance, run_timers once (boot.cpp:194-198). Not reached by vmboot so far and not ported by 138/139: the mode-8/9 callback consumers (0x47bc20/0x47bc80, 0x47be30/0x47be90; shipped sl scripts never call `registerCallback`, natives register the car), perf timers, camera snapshots, car body creation (`Vehicle__createCar` via vtable), native ground (`type_ground__vf3` 0x4bade0).

## Car body creation, sleep/wake (2026-10-24, cards 140/141)

**Vtable slot.** `_type_car` vtable 0x59dcd8 (18 slots): vf0 0x493da0, vf1 0x49ab20, vf2 0x49c6b0, **vf3 0x4992e0 `Vehicle__createCar`**, vf4 0x49beb0, …, vf11 0x498300 (`Vehicle__updateControls`), vf12 0x49c8e0 (`Vehicle__update`), vf13 0x494910, vf14 0x4a2e60 (command/message handler). vf3 = "create native object", same slot as `type_ground__vf3` 0x4bade0.

**Call path.** Script `Vehicle(GameRef parent, int id, …)` (Vehicle.java:111, also CarMarket.java:491/792, Garage.java:1964, EventList.java:507) → `create(parent, new GameRef(id), "0,0,0,0,0,0", "Vehicle")` → `GameRef.create` 0x4da620 → `GameObject__createScripted` 0x523c80 (`create_native` 0x4da7e0 → `GameObject__createNative` 0x5240e0, same native part): instantiate the game instance under parent, `typed = inst->vf3(1.0)`, `T = 0x468b00(0x80000000)` = the type record's typed data; **`if (T+0xc) typed+0x4c = (T+0xc)->vf[0xc/4=3](&ctx16, params)`** (native create, before the script `<init>(I)` THRD-CREATE thread), then `0x47d560(&ctx)`, `(T+0xc)->vf[0x40/4=16](typed+0x4c)`. `T+0xc` is the native type manager of the record's `native <kind> <cfg>` line: the Chassis record (parts.rdb 0x640 rsd: `script java.game.parts.bodypart.Chassis` / `native car parts\scripts\Chassis.cfg`) → `_type_car` manager `M` → createCar. Other `native part` records → `type_part` manager (vf3 not traced here).

**`Vehicle__createCar` 0x4992e0 (ECX = M, (ctx, params))**, body-relevant part only:
```
car = Part__initInstance 0x4c1f90(ctx, params, new Vehicle(0x2284) /*ctor 0x490100*/)
b = GameRef__getPhysBody 0x4eabc0(car+0x28)          // typed(0xa0000000)+0xb4 of the part instance
if (b) car+0x13ec = ref(b->+0xb4)                    // chassis body (refcounted link at +0x13ec/+0x13f0)
... thermal/engine/wheel-array init (notes/09, notes/13) ...
b = getPhysBody(...); if (b) {
  root = b->+0xb4; while (root+0x58 & 4) root = root+0x20       // PhysBody__root 0x480700
  if (root+0x58 & 0x80) PhysBody__wake 0x507060(root)
  def  = Physics__createBox 0x4f7d60(1.0f, 5 /*box*/, 1.0f, 1.0f, 1.0f, 0)
  cr   = PhysicsWorld__addBody 0x4f7830(def, root+0x64+i*0xd4 /*pos*/, root+0xa4+i*0xd4 /*rot*/)  // i = root+0x20c active state
  GameObject__createPhysInstance 0x524620(car, 0, cr, "car_root")
  PhysBody__unlink 0x501d00(root); PhysBody__attachChild 0x4f7b20(root, cr)   // chassis body becomes child of car_root
  if (M+0x1f34 <= 0) M+0x1f34 = 1/(*(car+0x13ec)->+0x5c->+0x2c)              // spring weight from inverse mass
  per wheel: addChildBody 0x4f78d0(cr, …) "wheelbone" / "a_arm" / … + suspension object (notes/11)
  0x4ebd90(car, 0x20, 0)
}
if (car+0x13ec) 0x5032d0(car+0x13ec)                 // compound mass/COM refresh (not traced)
r = root of car+0x13ec; if (!(r+0x58 & 0x80)) PhysBody__sleep 0x506d90(r)   // car_root goes to sleep
... slot/part list, "car_add %d %d" event, typed+0xc |= 0x4000 ...
```
**`Part__initInstance` 0x4c1f90 (ECX = M, (ctx, params, P))**: `Str__scan(params, "%f,%f,%f,%f,%f,%f,%d", &x,&y,&z, &ya,&p,&r, P+0x58)`; ypr → matrix 0x5666d0; `P+0x12c = pos`. If `M+0xff0 >= 0` (first body index: type init 0x4c3940 sets -1, the cfg `body` line handler 0x4bf4e0 sets it to the record count at the first body line; Chassis.cfg has none, but every shipped car type record (`native car cars\racers\Axis_Data\scripts\_main_200S.cfg`, Axis.rdb) has `body` lines, so real cars take this path): `def = Physics__createBox(1.0f, 6, 0,0,0, 0)` (type 6 without mesh = Poly box template), `Physics__attachToResource 0x4e8a50(P, def, &pos, &mat, "part_comp")` = `addBody(def, pos, mat)` + `createPhysInstance(P, 0, body, "part_comp")` (+ 0x518e90 when the resource flag 0x100); `body = getPhysBody(P)->+0xb4`; `PhysBody__sleep(root(body))`; `body+0x58 |= 8`; for each M bone record `i < M+0xfa8` with kind 1/2: `addChildBody 0x4f78d0(body, rec shape, world pos, mat)` ("bbone"/"steer" names, flags 0x4000/0x20/0x11040); `0x5032d0(body)`. Else (`M+0xff0 < 0`, `M+0x1040` click shape) attaches a "click" body instead.

**`GameObject__createPhysInstance` 0x524620(parentHandle, 0, body, name)**: creates a type-2 resource instance (`_physinst` default name) under the part's resource, `typed+0xb4 = ref(body)`, copies the body's active pos to the instance. This is how `GameRef__getPhysBody` later finds the body.

**Sleep/wake (world roots `w+0x84` active, `w+0x88` sleeping).**
- `PhysBody__sleep` 0x506d90 (ECX body): if `!(+0x58 & 0x80)`: if parent == `w+0x84`: `unlink`, `attachChild(body, w+0x88)`; `+0x58 |= 0x80`; active state (`+0x64 + i*0xd4`, 0x35 dwords): zero `[3..5]` vel, `[0x1c..0x21]`, `[0xa..0xc]`, `[0x22..0x24]`, `[0xf] &= ~2`; copy active → other state; recurse into children (`+0x24` first, `+0x28` next).
- `PhysBody__wake` 0x507060: if `+0x58 & 0x80`: clear it; if `!(flags & 4)`: `+0xc = 0, +0x10 = 100000.0f (0x47c35000), +0x14 = 0`; if parent == `w+0x88`: `unlink`, `attachChild(body, w+0x84)`; recurse children; then re-queue every contact/listener in `w+0x134` whose `vf[5](body)` hits (moves them to list `w+0xbc`) — listener part not needed for stepping.
- `PhysBody__unlink` 0x501d00: remove from parent's child list (`+0x24`/`+0x28`), compound cleanup if `+0x1c` (child-shape record), `+0x20 = 0`. `PhysBody__attachChild` 0x4f7b20(b, p): head insert `b+0x28 = p+0x24; p+0x24 = b; b+0x20 = p`; if `!(p->def+0xb4 & 0x100)` → 0x4f7990(b) (compound attach, not traced).
- `PhysBody__root` 0x480700: walk `+0x20` while `+0x58 & 4`. `PhysBody__wakeRoot` 0x48ca50: if asleep, wake root. `PhysBody__sleepRoot` 0x4ae5a0: if awake, sleep root.

**Who wakes the car.** `type_car__vf14` 0x4a2e60 `(car, sender, code, msg)`, code 0x10 (EVENT_COMMAND), first word of msg: `"brake"` (0x4a3f6d) `car+0x70 |= 0x40000`; `"start"` (0x4a3fa6) `car+0x70 &= 0xfffbfffb`; `"suspend"` (0x4a3fdf) `PhysBody__sleepRoot(root(car+0x13ec))`; `"wakeup"` (0x4a4023) `PhysBody__wakeRoot(root(car+0x13ec))`; `"reset"` (0x4a4067) zeroes the root's active vel/angular state and the car inputs (`+0x1ddc..+0x1df4`, gear `+0x1e5c = 0`, `+0x1e7c/+0x1e78`), it does **not** wake. Reached from `Game__dispatchEvent` 0x47c760 step 2 (native handler `vtbl+0x38(typed+0x4c, sender, code, param)` before the script). Script senders: `Vehicle.wakeUp()` = `queueEvent(null, EVENT_COMMAND, "wakeup")` (Vehicle.java:185): Garage.java:289/473 (then `"suspend"`), CarMarket.java:758/885, Track.applyStartGrid only `if(!game)` (Track.java:2909; freeride has `game` set), `Gamemode.wakeUpCars` (call commented out in Track.java:1056). Engine wakers: `Vehicle__updateControls` 0x498300 at 0x498599 (a control axis value changed vs. its stored `+0x2c` and `car+0x70 & 1`), `Vehicle__update` 0x49c8e0, contact resolve 0x4f0dd0, 0x4f2d70, `type_camera__vf12` 0x48a430, 0x4be600. So in freeride the player's car is woken by player input (updateControls) or a collision, not by a script call. UNRESOLVED: whether `GameRef.setMatrix` 0x4db950 (formats `"ffffff"` and calls 0x4eb3f0) wakes or sleeps bodies (Vehicle.java:128 comment says `setMatrix(null,null)` stops the physics).

**Ground.** Not needed for the car to be stepped: `PhysicsWorld__step` integrates every awake root under `w+0x84`. Without the native ground (`type_ground__vf3` 0x4bade0, notes/19) the car just falls (gravity -9.81); the ground is needed for contacts only (later card).

**src today.** `Runtime::instantiate` (src/game/runtime.cpp) ignores the type's native line (no vf3 dispatch); `Runtime::queue_event` has no native handler (shortcut comment). `game::CarData` (src/game/car.*) is a lazy per-resource payload with no body; `vehicle::Car` (src/vehicle/vehicle.*) owns a `RigidBody body` but nothing instantiates it in the game layer. `physics::World` has only the active root list (`bodies`, head insert), no sleeping root, no body counter (`w+0x54`), no parent/child links (single non-compound bodies).
