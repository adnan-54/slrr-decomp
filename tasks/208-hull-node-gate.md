---
id: 208
status: todo
branch:
depends: [207]
---
# Hull vs ground soup: per-map-node gate of 0x4feb20 instead of the swept BVH gate

## Goal
`collect_hull_candidates` (src/physics/world.cpp ~899) walks the merged soup's BVH and keeps a node
when the child's SWEPT sphere (start -> integrated pose, radius br) touches the BVH box
("INVENTED: merged soup"). The exe instead walks map nodes and drops a whole node when the body's
STEP-START bounding sphere misses the node's record box (no sweep). In a fast corner-first approach
src gets a candidate one step before the exe (notes/24 "Consequences" 3). Replace the BVH walk with a
walk over `CollisionMesh::map_nodes` (card 207) with the exe gate; the per-face filters after it stay
unchanged. Meshes without map nodes keep the BVH walk.

## Exe evidence (cleaned)
### 0x4feb20 Collide__bodyVsTriangles, entry gate (B = one compound child body here; N = map node)
```
shape = B.def+0x90; br = shape+0xc                      // bounding radius
s = B state[+0x20c ^ 1]                                  // the step-start pose (prev state)
if (|s.pos.y - N.centre.y| <= N.half.y + br &&
    |s.pos.x - N.centre.x| <= N.half.x + br &&
    |s.pos.z - N.centre.z| <= N.half.z + br) {
    ... face filters over this node's faces (0x4f9860 swept sphere, 0x4fc450 OBB SAT; already in src) ...
}
else return 0                                            // the node's faces are never looked at
```
### 0x4fe6f0 Collide__groundTree order: node's own faces, then type-1 children in record order (DFS)
= the order card 207 stores in `map_nodes`; faces in a node in PHYS order (`first .. first+count`).

## Files
- edit: src/physics/world.cpp `collect_hull_candidates` (~899-985). Per collision child (`cc`, `cp0`
  = start pose, `br` as today), when `!gm.map_nodes.empty()`:
  ```
  // 0x4feb20 node gate: step-start sphere vs the map node's record box (no sweep).
  for (const CollisionMesh::MapNode &mn : gm.map_nodes) {
      const Vec3 c = gp.pos + mat_vec(gr, mn.centre);   // same transform as the face vertices
      if (fabsf(cp0.pos.y - c.y) > mn.half.y + br || fabsf(cp0.pos.x - c.x) > mn.half.x + br ||
          fabsf(cp0.pos.z - c.z) > mn.half.z + br)
          continue;
      for (uint32_t ti = mn.first; ti < mn.first + mn.count; ti++) { <existing per-face body, with ti> }
  }
  ```
  Move the existing per-face body (from `HullCandidate c;` to `out.push_back(c);`, using `ti`) into a
  small static helper or lambda so both walks share it; the BVH walk stays for `map_nodes.empty()`.
  Keep the card-164 side gate and its INVENTED comment as they are. Update the function comment:
  the merged-soup INVENTED note becomes "map nodes per 0x4feb20; streaming not modelled (no collision
  effect for cars, notes/24)". `INVENTED:` note that half is not rotated (shipped grounds have
  identity rotation).
- edit: src/physics/world.cpp ~1221 comment in `update_contacts`: same wording change.
- edit: src/game/runtime.cpp line ~271 comment: replace "INVENTED: one merged collider ... per-node
  streaming walk" with: faces merged in groundTree walk order with the node table (card 207/208);
  streaming/unload not modelled: for cars the exe loads nodes synchronously (0x518a80 prio 0,
  notes/24), so only residency differs. Keep the `shortcut:` about no unload.
- read only: src/physics/mesh.h (MapNode from card 207).

## Done when
- `// 0x4feb20 node gate` comment; deviations `INVENTED:`
- `make -C src clean check` green (vmboot drive/selfcheck runs unchanged or better; report any check
  whose numbers move); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
