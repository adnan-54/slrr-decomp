---
id: 143
status: merged
branch: card/143
depends: [142]
---
# Native ground create: static kind-0 `groundbody` in the process world (statics list)

## Why
`type_ground__vf3` 0x4bade0 gives every map a static `groundbody` (side B of every map
contact in the exe). src has no ground body and `World::add_body` head-inserts every body
into the active root list, while the exe skips the root insert for defs with flag 4
(static / no integration). This card adds the body exactly as the exe builds it; card 144
gives it the PHYS soup so the car stops falling. RE: notes/19 §7 (`type_ground__vf3` body
part), notes/08 (addBody), notes/11 §1 (createBox kinds).

## Exe functions (4)
- `type_ground__vf3` 0x4bade0, body part:
```
v = Config("gravity") (0x47c080 = static float field of java.util.Config, default 9.81)
if (-v <= 0) world gravity = (0, -v, 0)
f = Config("ground_friction") (default 1.0); if (f <= 0) f = 1.0          // g+0x94
def = Physics__createBox(0 /*mass*/, 0 /*kind none*/, 0,0,0, 0)
def+0xb4 |= 4                                                             // static, no integration
shape: friction(+0x50) = f, +0x54 = 0.01 (rolling res), restitution(+0x58) = 0.2
body = Physics__attachToResource(g, def, &(x,y,z) /*params %f 1..3*/, mat, "groundbody")
```
- `Physics__createBox` 0x4f7d60, mass 0 / kind 0 branch: `inv_mass = 0`, drags 0, `flags |= 4`,
  shape kind 0, radius 0 (`+0xc`), AABB 0.
- `Physics__attachToResource` 0x4e8a50 = `PhysicsWorld__addBody(def, pos, mat)` +
  `GameObject__createPhysInstance(g, 0, body, "groundbody")`.
- `PhysicsWorld__addBody` 0x4f7830: `world+0x54 += 1` always; head insert under the active root
  only `if (!(def+0xb4 & 4))`.

## Files (3)
- src/physics/physics.h + src/physics/world.cpp: `std::vector<RigidBody *> statics;` on `World`
  (INVENTED: the exe keeps flag-4 bodies unlinked; src needs a list to own/pair them).
  `add_body`: `body_count++`; `def.flags & 4` -> `statics.insert(begin)`, else `bodies` as now.
  `update_contacts`: also pair each active root with each static (same filters: shape 0/7 skipped,
  so this card adds no contacts). `step` never integrates `statics`.
- src/game/runtime.cpp: create the ground for a mounted top-level type-1 record whose RSD
  `gametype` resolves to `native ground` (read the gametype/params from the RSD chunk via card 142's
  `parse_instance`, and switch `load_ground` to that text too). INVENTED timing: in the exe the map
  node loads through streaming (`Collide__groundTree` 0x4fe6f0 requestLoad near a body) and then runs
  vf3; src creates it once, at `mount_package`. Keep the body on the Resource (`ground_body`).
  Config floats: read `java.util.Config` statics `gravity` / `ground_friction` through the VM when
  the class is loaded, else the Config.java defaults 9.81 / 1.0 (mark the fallback INVENTED).
  Replace the runtime.cpp:528 shortcut text so it no longer lists ground.

## vmboot check (src/tools/vmboot.cpp)
- Update `world body_count counts both bodies of every car` to `2 * cars + grounds`.
- New hard checks once `actualState` is `java.game.Track`: the Track.map resource has a ground body;
  it is in `process_world().statics`, not in `bodies`; `def.flags & 4`, `inv_mass == 0`, shape kind
  0, friction 1.0, pos = the record's params xyz (0,0,0 for the shipped maps).
- The existing `woken car_root falls under gravity` check stays true (kind 0 makes no contacts).

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, new checks ok.

## Not this card
The soup collider (card 144); materials table (`Cfg__parseKeyTable` 0x4f6240); particles/sound
precache, `r_ground` render instance, `IntervalMgr__addNode` 0x4ee1a0; whether every embedded area
child (inherits the ground type) builds its own groundbody (UNRESOLVED).
