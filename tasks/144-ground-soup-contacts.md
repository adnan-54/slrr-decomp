---
id: 144
status: implemented
branch: card/144
depends: [143]
---
# Map triangle soup as the groundbody collider: the woken car stops falling

## Why
After 142/143 the map's PHYS soup is parsed and a static `groundbody` exists, but nothing
collides with the soup, so the woken freeride `car_root` still falls (~112 m in 300 frames).
In the exe, `Collide__groundTree` 0x4fe6f0 walks the resident map nodes near a body and
`Collide__groundSoup` 0x4ff6b0 -> `Collide__bodyVsTriangles` 0x4feb20 -> `Collide__candidatesTOI`
0x4fc7e0 adds c_collision contacts with B = groundbody (notes/19 §7). src already has a
triangle-soup collider (`CollisionMesh` + `mesh_contacts`, box/sphere/capsule vs soup).

## Exe functions (3, behaviour to match; narrow phase is INVENTED)
- `Collide__groundTree` 0x4fe6f0: node AABB (+0xb4 centre, +0xc0 half) vs body AABB; hit -> load,
  `if (T+0x18 /*nf*/) Collide__groundSoup(obj, node, T, groundBody)`, recurse type-1 children.
- `Collide__groundSoup` 0x4ff6b0: for each physics instance of the car, unless body/def flag 0x24:
  `Collide__bodyVsTriangles(body, T+0x14 verts, T+0x1c faces, groundBody, node bounds, nf, 0)`.
- `PhysicsWorld__addContact` 0x502830 side B = groundbody: μ = min(μA, μB), e = (eA+eB)/2.
```
contact(body, tri) -> c_collision{A = body, B = groundbody, n = from soup toward body, d}
```

## Files (3)
- src/physics/mesh.h + src/physics/mesh.cpp: `build_collision_mesh(const std::vector<Vec3> &verts,
  const std::vector<CollisionMesh::Triangle> &tris)` (metres, no scale; reuse the existing AABB tree
  builder; same null return when no triangle survives).
- src/game/runtime.cpp (ground create from 143): gather the PHYS soups of the map record and of every
  embedded type-1 child recursively (card 142 parser), one merged `CollisionMesh`, give the groundbody
  shape kind `SHAPE_MESH` with that mesh. Keep `inv_mass 0`, flag 4, friction/restitution from 143.
  `INVENTED:` one merged collider built at create instead of the exe's per-node streaming walk and its
  shape-0 groundbody + 0x4feb20 TOI narrow phase; mark it with `shortcut:` (upgrade when 0x4feb20 /
  0x4fd110 / 0x4fc7e0 are traced). World pairing of roots vs statics is already done by 143.

## vmboot check (src/tools/vmboot.cpp)
Replace `woken car_root falls under gravity (no ground yet)` with: after the wakeup + 300 frames,
`car_root pos.y` stays above `floor - 1.5` where floor = highest soup triangle under the car's start
(x,z) (compute from the parsed soup, e.g. city 1.00, test_track 6.37, ROC_track 0.003 at (0,0)), and
`|vel.y| < 1.0` at the end. Print floor, pos.y before/after and the contact count.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`; the new
  check shows the car resting on the map (report the numbers). Memory: city soup ~197k faces must fit
  the 4 GB cap; report peak RSS if it grows.

## Not this card
Per-face materials (`mat` -> `.scm` names -> friction), body flag 0x800/0x40 sweep paths (0x4fd110),
wheels/suspension raycast against the soup, streaming/unload of map nodes.
