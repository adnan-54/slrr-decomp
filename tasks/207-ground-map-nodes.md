---
id: 207
status: implemented
branch: card/207
depends: []
---
# Ground soup: keep the map node table (bounds + face range) on the merged mesh

## Goal
The exe never collides with one merged soup; it gates every map node (type-1 record) by its bounds
box before testing that node's PHYS faces (notes/24). src merges all faces in
`gather_ground_soup` (src/game/runtime.cpp:199) and loses the nodes. Keep them: each node's box
(from the record's 48-byte extra block) and the range of its faces in the built mesh's `triangles`.
No collision change in this card (card 208 uses the table). Also add the rpkdump corpus check that
makes the merged-soup equivalence hold (faces inside node box, child box inside parent box).

## Exe evidence (cleaned)
### 0x51e6b0 Rpk__readRecord (record bounds)
```
if (rec.flags & 1) {                       // 48-byte extra block after the name
    bounds = { kind = 1,
               centre = { f32 extra[0], f32 extra[1], f32 extra[2] },     // bytes 0..11
               half   = { f32 extra[3], f32 extra[4], f32 extra[5] } };   // bytes 12..23
    Resource__createInstance 0x518410(..., &bounds)   // -> node +0xb0 kind, +0xb4 centre, +0xc0 half
} else  Resource__createInstance(..., null)
```
### 0x4fe6f0 Collide__groundTree walk order (what the node order must match)
```
n = node has faces ? Collide__groundSoup(obj, node, T, ground) : 0     // node's own PHYS faces first
for child C of node (record order): if C.type == 1: n += groundTree(obj, C, ...)   // then children, DFS
```
### Corpus facts (notes/24, scratch parse): every PHYS node has the extra block; every PHYS face vertex
lies within centre +- half (1 mm); every type-1 child box lies inside its parent box; <= 20 faces/node.

## Files
- edit: src/physics/mesh.h: in `CollisionMesh` add
  ```
  // One map node (type-1 record) of a ground soup: record bounds box (0x51e6b0) and its
  // faces' range in `triangles`, in Collide__groundTree 0x4fe6f0 walk order. Empty for SCX.
  struct MapNode { Vec3 centre{0,0,0}, half{0,0,0}; uint32_t first = 0, count = 0; };
  std::vector<MapNode> map_nodes;
  ```
  and a 4th overload `build_collision_mesh(verts, tris, normals, const std::vector<CollisionMesh::MapNode> &map_nodes)`
  where `first/count` index the INPUT `tris`.
- edit: src/physics/mesh.cpp: the 3-arg overload calls the new one with an empty node list. In the new
  one, while filtering triangles (the existing out-of-range / degenerate `continue`s), map each input
  node range to the output range: `out.first = triangles.size()` before the node's first input tri,
  `out.count` = how many of its tris survive. Push one MapNode per input node (count may be 0).
  The BVH build stays as is.
- edit: src/game/runtime.cpp `gather_ground_soup` (line 199): add a `std::vector<CollisionMesh::MapNode> *nodes`
  parameter. Per record: `MapNode m; m.first = tris->size()` before pushing its faces, `m.count = faces`
  after; centre/half from `rec.extra` (memcpy 6 floats, little-endian like the rest of rpk.cpp) when
  `rec.has_extra`; push it (before recursing, so the order is node, then its type-1 children in record
  order). No extra block: `INVENTED: half = 1e30 (never culled); no shipped PHYS node lacks the block`.
  `create_ground_body` (line ~281) passes the node list to the new `build_collision_mesh`.
  src/tools/vmboot.cpp:2152 `gather_phys_faces` stays as is (it compares faces only).
- edit: src/tools/rpkdump.cpp `walk_map_instance` (line ~361) / `MapTally` / `selfcheck_maps`: add
  counters and checks per map row: (a) every record with faces has `has_extra`; (b) every vertex used by
  a face of the record is within `centre +- half + 1e-3` on each axis; (c) every type-1 child with
  `has_extra` has its box (centre +- half) inside the parent's box (+ 1e-3); (d) max faces per record
  <= 64 (the exe's face cache at 0x6029e4 has 64 slots). Print the counts on the map line.
- read only: src/resource/rpk.h (RpkRecord.extra/has_extra, RpkInstanceData).

## Done when
- `// 0x51e6b0` / `// 0x4fe6f0` comments on the new code; deviations `INVENTED:`
- `make -C src clean check` green with the four new rpkdump checks passing on all four maps; no new unit
  tests or hand-made expected values
- card frontmatter: status implemented, branch name
