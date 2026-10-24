---
id: 159
status: implemented
branch: card/159
depends: [158]
---
# V-Clip polyhedra for kind-6 hulls and soup triangles (0x50e6d0, 0x50f000, 0x50d230, 0x50d9d0)

## Why (RE 2026-10-24, notes/09 "Top speed 103.85 m/s ... free fall")
vmboot's 103.85 m/s is a free fall: at the bottom of the freeride slope (f242, flat face y -26.05) the
leading wheel mounts pass the face, the sensor segments stop crossing it, and nothing else of the car
collides with the soup (`car_root` is `SHAPE_NONE`, the cfg `body` hulls are mass-only compound children).
In the exe the hulls collide through V-Clip (card 160 wires the narrow phase). src has no polyhedron and no
V-Clip (shape.cpp: primitives only; `pair_unsupported_mesh` drops mesh pairs). This card adds them, no
behaviour change yet.

## Exe functions (pasted / referenced)
`Poly__buildFromFaces(nfaces, table)` 0x50e6d0 (notes/11 §2, table entries 0x20 `{count, idx[<=7]}`):
```
per face: F feature, n = normalize((v1-v0)x(v2-v0)), interior q = v0 + (v1-v0)/4 + (v2-v0)/4
  per edge vi->vi+1 (wrap): find/create V(vi), V(vi+1); reuse E with the same vertex pair, else new E
     dir = normalize(vj - vi); addPlanePair(tail V, dir) ; addPlanePair(head V, -dir)
  E-F plane: m = dir x n, flipped so m.(q - tail) >= 0; addPlanePair(tail, m, E) with F: F gets +m, E gets -m
capacity nv + nf + nf*3/2 (nv=4, nf=2 -> 5 edges). Feature kinds 1 V, 2 E, 3 F; stride 0x20.
```
Kind-6 shapes (`Shape__loadScx` 0x4f5b90, notes/11 §2): every .scx face is `{3, i0, i1, i2}` (triangles,
coplanar ones are not merged), so a hull Poly = buildFromFaces over the CollisionMesh triangles in file
order. FL_door.scx: 16 V, 42 E, 28 F = 86 features (notes/11).

Soup triangle Poly `0x50f000(verts, table)` built for a PHYS face by 0x4feb20 (decompiled 2026-10-24):
```
table = { {3, i0, i1, i2, pad, n}, {3, i0, i2, i1, pad, -n} }   // face and reversed twin, n = PHYS +0x10
first call (Poly+8 == 0): capacity 8, Poly__buildFromFaces(2, table)   // 3 V, 3 E, 2 F
later calls: same features, vertex pointers re-pointed and the E/F plane normals recomputed in place
             (e_k = normalize(v_{k+1} - v_k), planes n x e_k / e_k x n, F normals +-n): same values as a rebuild
```
`VClip__closestFeatures` 0x50d230 (notes/11 §3, full walk; step functions stepVE 0x50be40, stepVF 0x50c830
+ handleLocalMin 0x50b7b0, stepEE 0x50a410, stepEF 0x50aa60, distVF 0x50c400, distEE2 0x509130, clipEdge
0x508fe0: all described there): `(stateA, stateB, shapeA, polyB, &fA, &fB, &pA, &pB, &n) -> d`; starts at the
passed features or each poly's first feature, <= nfeatA + 2*nfeatB steps, writes the features back; on
overrun "Voronoi error (infinite loop)", returns 1e6; penetration returns the VF/EF depth (negative).
`Phys__shapeDistance` 0x50d9d0 (notes/11 §3, single check on cached features, used by updateContacts):
0/2 -> distance, 1 -> 0.0 (caller drops the contact).

## Files (<= 3 + checks)
- new `src/physics/vclip.h` / `src/physics/vclip.cpp`: `Poly` (verts, features with kind, vertex refs and
  plane lists), `poly_from_triangles(const CollisionMesh&)` (0x50e6d0), `poly_triangle(v0, v1, v2, n)`
  (0x50f000 layout), `vclip_closest(...)` (0x50d230) and `vclip_check(...)` (0x50d9d0) on two poses.
- `src/Makefile`: add `physics/vclip.cpp` to LIB_SRC.
- Cache the hull Poly next to the CollisionMesh (e.g. `std::shared_ptr<const Poly>` built once in
  `body_hull`, car.cpp) only if needed by the vmboot check; otherwise build on demand in the check.

## Checks (real data only)
1. `scxcheck` (or vmboot selfcheck) corpus: every kind-6 hull referenced by a shipped car/part cfg `body`
   line builds a closed Poly: each E has exactly 2 adjacent F, V - E + F = 2; print the count; FL_door.scx
   gives 16/42/28 (= 86, notes/11). Report hulls that are not closed instead of "fixing" them.
2. vmboot, after the 300 settle frames (car at rest on the flat start face): for each SunStrip cfg body-line
   hull (car frame pose from the compound, card 153) and the soup face under its lowest vertex
   (`mesh_raycast` down from that vertex): `vclip_closest` terminates (no 1e6) and its distance equals the
   minimum signed distance of the hull's vertices to that face's plane when the returned pair is (V, F)
   (within 1e-4 m); print both and the feature kinds. Two independent computations on shipped geometry.
3. Existing checks unchanged, `selfcheck: PASSED`.

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
PASSED with checks 1-2 printed and passing. No behaviour change (auto drive line identical to 158:
`max gear 5, final speed 103.85 m/s`).

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, what you saw in Result). No unit tests or
synthetic shapes; cite exe addresses; `INVENTED:` for anything not from the exe (e.g. storage layout).

## Not this card
Wiring into World (card 160), the 0x4feb20 filters, TOI, addContact, pair cache 0x4f6570/0x4f6ea0.

## Result
Implemented `physics/vclip.{h,cpp}`: `Poly`/`PolyFeature`/`PolyPlane`, `poly_from_triangles` (0x50e6d0),
`poly_triangle` (0x50f000), `vclip_closest` (0x50d230, incl. stepEF/clipEdge 0x50aa60/0x508fe0 and
checkEE 0x509f10 stepping) and `vclip_check` (0x50d9d0). No hull Poly cache was needed; vmboot builds it
on demand. `make -C src clean check` green:
- vmboot: 11/11 cfg hulls evaluated, 10 V-F, every V-F distance == min vertex-face distance (1e-4);
  auto drive unchanged (`max gear 5, final speed 103.85 m/s`).
- scxcheck: 1888 unique cfg `body` hulls, 1847 closed; Axis `_phys_FL_door.scx` = 16 V / 42 E / 28 F
  (86 features). 41 shipped hulls are open (windows/water/garage props) and 5 cfg hulls have no mesh on
  disk; both are reported, not failed, and not repaired.
`INVENTED:` C++ storage layout, max-violation plane choice in the region test, and the exact branch
order inside stepEF/vclip_check (notes/11 §3 gives the exe addresses and result codes, not every branch).