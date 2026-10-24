---
id: 156
status: implemented
branch: card/156
depends: [144, 154]
---
# Map soup keeps the PHYS face normals + the exe sensor segment test (0x4fd110)

## Why (RE 2026-10-24, notes/19 §7 "Sensor segment vs PHYS faces", notes/09 "Who writes the ray hit")
In the exe the wheel "ray" is not a ray: the wheel sensor capsule (0x4f3f10) is tested against every
PHYS face of the map nodes near the car by `0x4fd110`, a one-sided segment-vs-triangle crossing that
uses the face normal stored in the PHYS chunk. src's `mesh_raycast` is two-sided and recomputes the
normal from the vertices (flipped toward the ray). This card adds the exe test and keeps the PHYS
normals in the merged collider; card 157 switches the wheels to it. No behaviour change in this card.

## Exe functions (pasted)
`0x4ff6b0` sensor branch (shape kind 3/4, body flag 0x40, not 0x1000): seg P0 = state*(0,-a,0),
P1 = state*(0,+a,0) (a = half length; kind 4 uses z), then `0x4fd110(body, verts, face, ground, seg)`
for every face of the node.

`0x4fd110(body, verts, face, ground, seg) -> t`:
```
v0 = verts[face.i0]; n = face.n            // PHYS +0x10, stored, not recomputed
d0 = n.(P0 - v0); d1 = n.(P1 - v0)
if !(d1 > 0 && d0 <= 0) return 0
if (|d0| > 0.02 || |d1| > 0.02) {
  t = |d0/(d0-d1)|; q = (P0-v0) + t*(P1-P0)
  (i,j) = 0x5043d0(n)
  a = v1-v0; b = v2-v0; inv = 1/(b_j*a_i - b_i*a_j)
  s = (b_j*q_i - b_i*q_j)*inv; u = (a_i*q_j - a_j*q_i)*inv
  if (0<=s && s<=1 && u>=0 && s+u<=1) { X = v0+q; addTriggerContact(body, ground, face, X, n); return t }
  return 0
}
// in-plane: for k in 0..2 (edge v_k -> v_{k+1 mod 3}), A = (v1-v0)_i*(v2-v0)_j - (v2-v0)_i*(v1-v0)_j,
// return 0 if |A| < 1e-5:
//   ex = (v_{k+1}-v_k)_i*A; ey = -((v_{k+1}-v_k)_j*A)
//   f0 = (P0_j - v_k_j)*ex + (P0_i - v_k_i)*ey; f1 = same with P1
//   if f0 >= 0 { te = f0/(f0-f1); t_in = max(t_in, te) } else { if f1 < 0 return 0; t_out = min(t_out, f0/(f0-f1)) }
//   if t_out <= t_in return 0          (t_in starts 0, t_out 1)
// X = P0 + t_in*(P0-P1)   (sign as decompiled, keep it); addTriggerContact(...X, n); return t_in
```
`0x5043d0(face) -> (i,j)` (drop the dominant normal axis):
```
if |ny| < |nx|: (|nz| <= |nx|) ? (1,2) : (0,1)
else:           (|nz| <= |ny|) ? (0,2) : (0,1)
```

## Files (2 + vmboot)
- src/physics/mesh.h + mesh.cpp:
  - `CollisionMesh::face_normals` (`std::vector<Vec3>`, one per triangle, empty for SCX meshes) and a
    `build_collision_mesh(verts, tris, normals)` overload (the existing 2-arg one stays for callers).
  - `struct SegmentCross { Vec3 point; Vec3 normal; float t; uint32_t triangle; };`
    `int mesh_segment_cross(const CollisionMesh &m, const Pose &pose, Vec3 p0, Vec3 p1,
    std::vector<SegmentCross> *out)`: walk the AABB tree with the segment's AABB (INVENTED
    broadphase: a crossing face always overlaps it, so the face set equals the exe's all-faces loop)
    and run 0x4fd110 above on each candidate in mesh-local space; push every hit (point, normal back
    in world frame, t). Requires `face_normals` (return 0 otherwise). Port both branches literally,
    with the exe's float comparisons; cite 0x4fd110 / 0x5043d0.
- src/game/runtime.cpp `gather_ground_soup` / `create_ground_body`: also collect `RpkPhysFace::n`
  and pass it to the new overload (same order as `tris`).

## vmboot check (src/tools/vmboot.cpp, next to "car drive (0x47be30)")
After the 300 settle frames, for each sim wheel i: top = root pose * mount, P0 = top - up*max_ray(),
P1 = top (any segment longer than `ray_len` serves this check; the exe's createCar-time length is
card 157's `sensor_len`). Call `mesh_segment_cross` on the
groundbody mesh and print the hit count, the nearest |top - X| and its normal.
Hard checks (real data only):
1. Every hit's `normal` equals the `RpkPhysFace::n` of the same triangle re-read from the map rpk
   through `RpkArchive::parse_instance` (vmboot walks the records itself, same order as
   `gather_ground_soup`) exactly (bitwise floats).
2. Every wheel has >= 1 hit, and the nearest `|top - X|` equals the current `w.ray_len` within 1e-3
   (the map faces under the wheels face up, so the one-sided test and the src raycast agree here;
   if a wheel disagrees, print the face and stop: that is a finding, not a tolerance to widen).

## Done when
- `make -C src -j4 check` green.
- `VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh`: `selfcheck: PASSED`, the two new
  checks pass, every older check unchanged (rest loads ~3.1 kN per wheel, sum within 2% of weight,
  gear >= 2 and final speed > v1), 0 failed threads. Report the per-wheel numbers and peak RSS
  (normals add 12 B/face, city ~197k faces).

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; no tuning constants; do not touch
src/vehicle/ or src/game/car.cpp (card 157).

## Not this card
Using the test for the wheels (157), the 0x4f3d70 selection, node streaming (0x4fe6f0), per-face
materials (S+0xa8/+0xac), 0x4feb20 / 0x4fc7e0 body-vs-soup narrow phase.

## Result
Implemented on card/156 (2026-10-24).

- `physics/mesh`: `CollisionMesh::face_normals` + 3-arg `build_collision_mesh(verts, tris,
  normals)`; `SegmentCross` + `mesh_segment_cross` porting `0x4fd110` / `0x5043d0` literally
  (both branches, one-sided `d1 > 0 && d0 <= 0`, stored normal, segment-AABB broadphase).
- `game/runtime.cpp`: `gather_ground_soup` keeps `RpkPhysFace::n` in triangle order and the
  ground body is built with the normals.
- `vmboot`: independent record walk (`gather_phys_faces`) + two checks. `make -C src -j4 check`
  green. Capped run (4 GB, 280 s): `selfcheck: PASSED (102 checks, 0 failure(s))`, peak RSS
  581.6 MB. Per wheel nearest |top-X|: 0.5536 / 0.5504 / 0.5519 / 0.5512 m, each equal to
  `ray_len`; 1 hit each; normals bitwise equal to the independently re-read PHYS normals.
  Loads 3069 / 3260 / 3098 / 3134 N (sum 12560.4 vs weight 12559.7 N); older checks unchanged.
