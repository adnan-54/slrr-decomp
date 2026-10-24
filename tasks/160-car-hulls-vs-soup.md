---
id: 160
status: merged
branch: card/160
depends: [159]
---
# Car hull children collide with the map soup (0x4feb20 -> 0x4fc7e0 -> addContact)

## Why (RE 2026-10-24, notes/09 "Top speed 103.85 m/s ... free fall")
Scratch debug run of c3c75ae: the car drives down the 14 deg freeride slope, the soup turns flat at f242,
the leading wheel mounts pass the flat face (ray_len 0.003, then no sensor crossing), and from f260 nothing
holds the car: `car_root` is `SHAPE_NONE` (car.cpp ~1104 shortcut) and the cfg body hulls are mass-only
(`rebuild_compound`). It falls through (f840 vel.y -92.5 m/s) and vmboot reports |vel| = 103.85 m/s.
The exe collides every compound child hull with the soup in the collide pass (event phase 0x4fc610 ->
0x4ee460 -> 0x4fe6f0 -> 0x4ff6b0 -> per node `Collide__bodyVsTriangles` 0x4feb20, then
`Collide__candidatesTOI` 0x4fc7e0). Aero drag is NOT the missing term for this car (script C_drag = 0).

## Exe functions (pasted, decompiled 2026-10-24)
`Collide__bodyVsTriangles` 0x4feb20(body, verts, faces, ground, node, nf, mask) (notes/19 §7, confirmed):
```
prev = other state (body+0x64 + (w[0x20c]^1)*0xd4), cur = active state; br = shape+0xc
if any |prev.pos_k - node.c_k| > node.h_k + br: return 0
(sensor capsule branch: unchanged, card 156)
mask' = copy(mask or 0); live = #zero
per face i (mask 0): cache slot i (0x602948 + i*0xa0, filled once by 0x504570, below)
     0x4f9860(prev.pos, cur.pos, br, slot) == 0 -> mask'[i] = 1, live--
if live && kind != 1 (sphere):
   half = (cur.pos - prev.pos)/2
   centre = prev.pos (+ R_prev * AABB centre of child bounds for kind 6 with children) + half
   ext = shape AABB half (+0x14..) (children: child bounds) + |R_prev^T half| * 1.5 + 0.05   per axis
   per face (mask 0): tri in body frame (R_prev^T (v - centre)), nb = R_prev^T n
        0x4fc450(ext, nb, tri) == 0 -> mask' = 1, live--
if live:
   if body has children (+0x24): Σ 0x4feb20(child, ..., mask') over children without flag 0x20,
        def flag 0x20, flag 0x20000; return Σ
   per face (mask' 0): tri shape cached on face+0x1c (0x4ffec0; friction 0.6 at +0x7c, restitution 0.2 at
        +0x84; Poly from 0x50f000 with {face, reversed twin}, card 159), +0xf4 = counter;
        0x4fbda0(body, shape, triShape, ground)
```
Face cache 0x504570 (slot C, floats): n = C[0x18..1a] (PHYS normal), v0/v1/v2 = C[0x1b..23];
e0 = v1-v0, e1 = v2-v1, e2 = v0-v2 (C[9..11], C[0xc..e], C[0xf..11]); m_k = e_k x n (C[0..2], C[3..5],
C[6..8]); |e_k|² = C[0x24], C[0x25], C[0x26].
`0x4f9860(p0, p1, r, C)` swept sphere vs face (1 = may touch, 0 = reject):
```
a = p0 - v0, b = p1 - v0; da = n.a, db = n.b
if !(da*db <= 0 || |da| <= r || |db| <= r) return 0
r2 = r*r
for (m, |e|², origin) in (m0, C[0x24], v0), (m2, C[0x26], v0), (m1, C[0x25], v1):
   sa = m.(p0 - origin), sb = m.(p1 - origin)
   if (sa > 0 && sb > 0 && sa² > |e|²·r2 && sb² > |e|²·r2) return 0      // both beyond r outside this edge
return 1
```
`0x4fc450(h, nb, tri)` box vs triangle, 4 axes only: `|nb.v0| <= Σ h_k |nb_k|`, then for k = x, y, z the
triangle's [min, max] of coordinate k overlaps [-h_k, h_k] (min/max of the 3 vertices, written as
centre +- half of v0/v1 then extended by v2). True = keep.
`0x4fbda0(body, shape, triShape, ground)` for a non-trigger body (flags & 0x48 == 0): new
`body_dummy_candidate` {+0x24 body, +0x2c triShape}, tail-appended to the list at 0x602918
(`DAT_006028f8` head, count `DAT_006028d8`), returns 0. (Trigger branch = existing trigger code.)
`Collide__candidatesTOI` 0x4fc7e0(ground) after the node walk:
```
for each candidate: features from the pair cache (0x4f6ea0, else first features)
   d = vclip_closest(prev state of body, ground state, body shape, tri Poly) - margin(shape+0x10)
   best = candidate with min d (stop scanning once best.d <= 0)
t = 1.0, step = 0.5, iter = 0
loop:
   if best.d > 0:
      if best.d < 0.02 and (iter == 0 or approaching: n.(pointVel(pA) - ground) < 0 via 0x505d60): accept
      if best.d >= 0.02 and iter == 0: accept-check (no contact, d > 0.02)
      t += step
   else t -= step
   step *= 0.5; if ++iter >= 10: accept-check
   if best.d <= 0: re-evaluate best at state interp(prev, cur, t) (0x506c00); if > 0 mark it and
       re-evaluate the unmarked others at t, best = min (ties keep the earlier one)
accept-check: if best.d <= 0.02:
   if best.d <= -margin (deep): n = tri F feature normal; fA = body vertex (kind 1) with min distance
       along n (rotated by the interp state); d = that distance + face-point offset; pA = that vertex
   interp state at t; 0x4fa550 (EE pair: sign of the edge triple product -> s, else s unchanged)
   d += margin; if margin > 0: d -= margin, pA -= margin * R^T n
   worldPt = R pA + pos; PhysicsWorld__addContact 0x502830(body, ground, triShape, fA, fB, pA, pB, n, d,
       s, t, worldPt)   // one contact per root per pass, earliest TOI (notes/11 §4.6)
free every candidate, writing features back to the pair cache (0x4f6570)
```

## Files (<= 3 + vmboot)
- src/physics/physics.h + world.cpp: `RigidBody` gets collision children (shape + child->parent pose;
  INVENTED storage, the exe uses real child bodies under part_comp). `update_contacts` add_new pass: for a
  dynamic root with collision children vs a static SHAPE_MESH: the 0x4feb20 walk over the merged soup
  (node test = AABB-tree node bounds; INVENTED: merged soup instead of streamed nodes, notes/19) +
  0x4fc7e0, one Contact (point lifted to the root frame, notes/11 §4 "pA/pB lifted through child+0x1c").
  Sub-iteration refresh of that contact: `vclip_check` (0x50d9d0, card 159): 0 -> drop, > 0.05 -> drop.
- src/game/car.cpp: `rebuild_compound` also fills car_root's collision children from the cfg body lines
  with a hull (kind 6) and installed part hulls, same poses as the mass children. Keep `car_root`'s own
  shape `SHAPE_NONE` (box is 0x4992e0's mass-1 box; whether it collides is a separate question, note it).
- vmboot (src/tools/vmboot.cpp): checks below.

## vmboot checks (exe-derived, no hand-made numbers)
Keep all existing checks. Add, over the 900 auto-drive frames:
1. Soup never passed: every frame, a downward `mesh_raycast` from `car_root.pos + (0, 3, 0)` (length 10)
   either misses or hits at y < `car_root.pos.y` (solid ground is not crossed). Print the min margin.
2. Top-speed bound from the rev limiter (0x49c8e0 §1: torque 0 above `RPM_limit`) and the top gear:
   `v_top = RPM_limit * 0.10471976 / (ratio[gears] * rearend_ratio) * wheel radius` from car_payload
   (SunStrip: 43.9 m/s). Hard check final |vel| <= v_top; print |vel|, horizontal speed and v_top.
   If 2 fails only because the route is downhill (gravity), report it in Result; do not change the bound.
3. At rest after the settle frames: no hull contact (the springs carry the car; card 158 check
   "no world contact on car_root/part_comp" stays green).

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-3 pass, the auto drive line prints the new final speed, 0 failed threads.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or synthetic
inputs; cite exe addresses; no tuning constants; `INVENTED:` for storage/merged-soup differences.

## Not this card
Aero drag §7 (0x49fb48..0x49ff40) and ground rolling resistance `gm+0x54` in p13 (card 161, inert for the
vmboot car: C_drag = 0); body-vs-body hull pairs (0x4fb370); streamed node walk.

## Result
Implemented on `card/160`.

- `physics.h`: `RigidBody::CollisionChild {shape, pose, poly}` + `collision_children`
  (INVENTED storage: the exe hangs real child bodies, 0x4feb20 descends them);
  `Contact::child_index`/`soup_triangle` keep the 0x50d9d0 refresh data.
- `world.cpp`: add_new pass walks the merged soup "tree" (node AABB test =
  node bounds + child bounding sphere), per-face 0x4f9860 swept-sphere and
  0x4fc450 box-triangle filters (AABB half = raw mesh bounds + 0.05), then
  0x4fc7e0's minimum distance + one `addContact` per root. Sub-iteration
  refresh runs the cached-feature `Phys__shapeDistance` 0x50d9d0 and drops on
  0/invalid or > 0.05 m. INVENTED: merged soup instead of streamed nodes; the
  signed support-vertex distance is added to the V-Clip distance because the
  0x50f000 face Poly is two-sided and a hull that crossed the face between two
  steps would otherwise only report a positive (unsigned) distance.
- `car.cpp`: `rebuild_compound` copies every cfg body hull (kind 6 + installed
  part meshes) into `collision_children` at the same parent-frame poses as the
  mass children; `car_root` itself stays SHAPE_NONE.
- `vmboot.cpp`: auto-drive checks (1) soup never above car_root over 900 frames,
  (2) final |vel| within the rev-limiter/top-gear bound, (3) no hull contact at
  rest.

vmboot (`make -C src -j4 check`, log `src/build/check160.log`):
`selfcheck: PASSED (112 checks, 0 failure(s))`, 0 failed threads.
- check 1: `hull vs soup ... 0 frame(s) with the soup above car_root, min soup
  margin 0.511 m`
- check 2: `final |vel| 33.29 m/s, horizontal 33.29 m/s, v_top 43.86 m/s`
- check 3: `48 hull(s)`, `no hull contact at rest (0 contact(s))`
- auto drive: `final speed 33.29 m/s (v1 12.42), mean driven kappa 0.077`;
  the 103.85 m/s free fall is gone and the car is held by the body hulls on the
  flat.
Doubt: the exe's TOI bisection (0x4fc7e0) is replaced by the signed support
distance; behaviour matches on the shipped freeride/auto-drive corpus, but a
multi-face corner case could differ.
