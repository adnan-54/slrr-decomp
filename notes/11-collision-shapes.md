# Collision shapes, V-Clip narrow phase, contact constraints

Read from the exe (Ghidra decompile + assembly where ECX mattered). Companion to `08-physics.md`. New names: `analysis/names.physics.json`.
"state" = one of the two body state buffers (`body+0x64 + idx*0xd4`, see 08): `s+0x00` pos, `s+0x0c` linear vel, `s+0x18` position-correction offset, `s+0x28` force accumulator, `s+0x40/+0x50/+0x60` rotation rows R (3×4), `s+0x70` ω, `s+0x7c` angular momentum L, `s+0x88` torque accumulator, `s+0xa4/+0xb4/+0xc4` world inverse inertia rows (3×4), `s+0x3c` flags (bit0 = "settled/valid"; cleared = wake). Local→world: `p_w = pos + R·p` (rows: `x = pos.x + R[0]·p` …). World→local: `p = Rᵀ(p_w − pos)`.

## 1. Shape object (`Shape__ctor` 0x4ba8c0, 0xc4 bytes; owned by the rigid-body definition at `bodyDef+0x90`)
| off | type | meaning |
|---|---|---|
| +0x00/+0x04 | ptr/int | ref-list head / refcount (shared between bodies) |
| +0x08 | int | **shape type** 0..7 (below) |
| +0x0c | float | bounding-sphere radius (+0.05 slack) — broad phase |
| +0x10 | float | **margin radius** (sphere radius / capsule radius; 0 for box, mesh) |
| +0x14..+0x1c | float3 | AABB half extents (+0.05) in body frame — broad phase |
| +0x20..+0x37 | Poly | V-Clip polyhedron (section 2); `+0x34` = Poly flags (bit0 = built) |
| +0x40..+0x48 | float3 | AABB centre (mesh) / centre of mass (0 for primitives) |
| +0x4c | float | volume (sphere `4/3·π·r³` = r³·4.1887903, box `8·hx·hy·hz`, mesh = polyhedron volume, capsules 0) |
| +0x50 | float | **friction** μ, default 0.6 (`0x3f19999a`) |
| +0x58 | float | **restitution** e, default 0.2 (`0x3e4ccccd`) |
| +0xb4.. | | parameters: sphere r; capsule half-length h (`+0x10` = r); box hx,hy,hz (+0xb4,+0xb8,+0xbc); mesh: `+0xb4` nverts, `+0xb8` float3* verts, `+0xbc` nfaces, `+0xc0` face table (0x20/face) |

Shape types (`Physics__createBox(mass, type, a, b, c, meshRes)` 0x4f7d60 sets them; `Phys__shapeInit` 0x507ce0 builds the polyhedron lazily, jump table 0x507e4c):
| type | shape | createBox params → fields | polyhedron (`shapeInit`) | bound radius `+0xc` |
|---|---|---|---|---|
| 0 | none | — | — (pairs skipped) | 0 |
| 1 | sphere | r → +0xb4, +0x10 | `Poly__buildPoint`: 1 vertex at origin | r+0.05 |
| 2/3/4 | capsule along x/y/z | a=half length h → +0xb4, b=radius → +0x10 | `Poly__buildSegment((−h,0,0),(h,0,0))` (y/z analogous): features V0,V1,E | h+r+0.05; AABB long axis h+r+0.05, others r+0.05 |
| 5 | box | a,b,c = half extents → +0xb4..+0xbc, margin 0 | `Poly__buildBox(hx,hy,hz)`: 8 verts, 26 features | √(hx²+hy²+hz²)+0.05 |
| 6 | convex mesh (.scx) | `Shape__loadScx(meshRes, shape)` | `Poly__buildMesh(nv, verts, nf, faces)` (nv<1 → box template) | max‖v‖+0.05 |
| 7 | mesh, no polyhedron | `.scx` loaded, Poly flag preset (never built) | none — skipped by `Collide__bodyPair` (type 0/7 excluded) | max‖v‖+0.05 |

Box vertex i = (±hx, ±hy, ±hz) with **+ when bit0/bit1/bit2 of i is set** (x,y,z). Face table `g_boxFaceTable` 0x5dae80 (6 faces × 0x20: `{count=4, i0..i3, pad}`): `0,2,3,1` · `4,5,7,6` · `0,1,5,4` · `7,3,2,6` · `1,3,7,5` · `0,4,6,2` (outward normal = (v[i1]−v[i0])×(v[i2]−v[i0])).

### Rigid-body definition (`rigidbody_type`, 0xb8 bytes, `body+0x5c`) — corrections to 08
`+0x2c` **inverse mass** (0 = static, then `+0xb4 |= 4`), `+0x30..+0x5c` inverse inertia in body frame (3×4), `+0x60..+0x8c` inertia (= inverse of +0x30), `+0x90` shape*, `+0x98` linear drag coef = −0.005·m, `+0x9c` angular drag coef = −0.01·m, `+0xa0` = shape+0x4c (volume), `+0xb4` flags (bit0 set always, bit2 static/no-integration, 0x100 kinematic, 0x400 primitive shape owned).
Mass properties `RigidBody__computeMassProps` 0x505760 (m = 1/invMass; writes inverse inertia diag then `+0x60 = inverse(+0x30)`):
- sphere (0/1): invI = 2.5/(r²·m) on the diagonal.
- capsule axis a (2=x,3=y,4=z), r = +0x10, L = 2h: invI_aa = 2/(r²·m); invI_⊥ = 1/((L²/12 + r²/4)·m) (cylinder formulas).
- box: invI_xx = 3/((hy²+hz²)·m), invI_yy = 3/((hx²+hz²)·m), invI_zz = 3/((hx²+hy²)·m).
- mesh: `Poly__massProps` 0x508d20 = Mirtich volume integrals (`Poly__volumeIntegrals` 0x508820, doubles in 0x605f90..0x605fd8): V=T0, COM c=T1/V, unit-density-per-volume inertia `Ixx=(T2y+T2z)/V−(cy²+cz²)`, `Ixy=−(TPxy/V − cx·cy)` …; **mesh vertices are shifted by −c** (COM to origin), matrix inverted, then scaled by invMass (`Math__mat3Scale`); shape+0x4c = V.
- `RigidBody__setSphereInertia(_, R)` 0x5056a0 (used for type 7 / generic): invI = **2.5/(m·R)** (R not squared — as in the exe), volume 4/3πR³.
- `Physics__createBody` (generic, from a resource with no primitive): invMass=1/m, invI diag = 2.5/m, drag as above.

## 2. Polyhedron + Voronoi features (V-Clip, Mirtich 1998)
Poly (at shape+0x20): `+0x00` nverts, `+0x04` float3* verts (stride 0xc), `+0x08` nfeatures, `+0x0c` capacity, `+0x10` Feature* array (stride 0x20), `+0x14` flags (bit0 built, bit1 owns features, bit2 owns verts). `Poly__clear` 0x50e240 frees per flags.
Feature (0x20):
| off | V (kind 1) | E (kind 2) | F (kind 3) |
|---|---|---|---|
| +0x00 | 1 | 2 | 3 |
| +0x04 | float* own vertex | float* tail vertex | float* first face vertex (point on plane) |
| +0x08..+0x10 | 0 | unit direction tail→head | unit outward normal |
| +0x14/+0x18 | plane count / capacity (init 6) | count / cap (init 4) | count / cap (= face vertex count) |
| +0x1c | VPlane* | VPlane* (plane[0]→tail V, plane[1]→head V, plane[2..]→adjacent faces) | VPlane* (one per edge) |
VPlane (0x14): `+0x00` neighbour Feature*, `+0x04` float* point on plane, `+0x08..+0x10` normal n. A point p (local) is inside the feature's Voronoi region iff `n·(p − point) ≥ 0` for every plane.
`Feature__addPlanePair(point, n, featB)` 0x50e110 (ECX = featA): appends to featB `{featA, point, −n}` and to featA `{featB, point, +n}` (grows arrays by 1).
`Poly__buildFromFaces(nfaces, table)` 0x50e6d0 (table entries 0x20: `{count, idx[≤7]}`): for each face: F feature with n = normalize((v1−v0)×(v2−v0)) and interior point q = v0 + (v1−v0)/4 + (v2−v0)/4; for each edge (vi → vi+1, wrapping): find/create V features, find existing E with the same vertex pair (plane count > 1) or create E (dir = normalize(vj−vi), planes: `addPlanePair(tail, dir)` between E and tail V, `addPlanePair(head, −dir)` with head V); then the E–F plane: m = dir × n_face, flipped so `m·(q − tail) ≥ 0`, `addPlanePair(tailOfEdge, m, E)` with ECX = F (F gets +m, E gets −m). Capacity for meshes: `nv + nf + nf·3/2` (special case nv=4,nf=2 → 5 edges). `Poly__buildSegment`: V0, V1, E(+planes). `Poly__buildPoint`: one V with no planes.

### `.scx` collision mesh (`Shape__loadScx` 0x4f5b90) — verified on `cars/racers/Axis_data/meshes/_phys_FL_door.scx`
Header `"INVO"`, u32 version. Coordinates are **centimetres** (×0.01 on load). Errors: "Fatal! Missing mesh file" / "Fatal! Invalid mesh file".
- version 1..3 (and 5 when flag `DAT_007a75e4` is set): repeated blocks until a zero dword: `u32 hdr`; v1: skip 0x38, stride 0x34; v2: skip 0x34, stride 0x34; v3: if hdr==0x38 skip 0x38 stride 0x2c else stride = u32 at hdr+0x3c, skip hdr bytes. Then `u32 nv`, nv × (3 floats xyz, stride bytes), `u32 nf`, nf × 3 u32 indices (each face → `{3, i0+base, i1+base, i2+base}`).
- version 4 (and 6 with the flag): `u32 nchunks`, nchunks×8 bytes table, then chunks `{u32 type, u32 size(incl. 8-byte header), …}`: type 4 vertices `{nv, flags, data…}` stride `(size−16)/nv`, xyz floats if flags&1; type 5 indices `{count, u16 triples}`.
- After load: per-face normal stored at face+0x14..0x1c, AABB → shape+0x14 (half) / +0x40 (centre), radius → +0xc.
Sample: FL_door.scx = v3, hdr 0x88, stride 64, **16 verts, 28 triangles** (closed hull: E = 42, features 86), AABB x −0.116..0.105, y −0.321..0.316, z −0.602..0.549 m. Link to data: the cfg line `body 0.003 0.001 0.023  0 0 0  36.000  cars\racers\Axis_data\meshes\_phys_FL_door.scx ;0x20` = body offset, rotation, mass 36 kg, shape file → type-9 rpk resource `0x20` whose `.rsd` holds `shape <path>.scx` / `mass` (see 01); the shape is a type-6 convex mesh, inertia from the hull, μ=0.6, e=0.2 defaults.

## 3. Distance queries
Globals: `g_vclipState[2]` 0x60617c (state A,B), `g_vclipPoly[2]` 0x606174, `g_vclipFeature[2]` 0x606184 (current features), `g_vclipSwap` 0x6060e0 (**persistent** 0/1 index; handlers address A/B as `[swap]`/`[swap^1]` and the result normal is negated when swap==1). Pair key = `kind[swap]*0x100 + kind[swap^1]`; mixed pairs are normalised so `[swap]` holds the lower-dimensional feature (V before E before F).
Outputs: points in each body's **local** frame, normal in world pointing **from B to A**, return = signed distance.

Distance primitives:
- `VClip__distVV` 0x50ce30: d = ‖pA_w − pB_w‖, n = (pA_w − pB_w)/d.
- `VClip__distVE` 0x50b870: vertex → edge frame, t = dir·(p − tail) clamped to [0, dir·(head−tail)], closest = tail + t·dir; then like VV.
- `VClip__distVF` 0x50c400: p in face frame, d = n_f·(p − point), closest = p − d·n_f, n = R_F·n_f; returns d (signed).
- `VClip__distEE2` 0x509130 → `Math__segSegClosest2` 0x509740 on world segments: n = dA×dB; non-parallel: line parameters by the perpendicular-plane method, **each clamped independently** to its segment; n flipped so `n·(q0−p0) ≤ 0`; returns the **squared** distance (caller takes √). Parallel: endpoint distances; normal = ((q1−p0)×dA)×dA.
- E–F (`VClip__stepEF` 0x50aa60 + inline): edge endpoints in face frame, d0,d1 = n_f·(p − point); crossing point x = p0 + (p1−p0)·(−d0/(n_f·(p1−p0))); n = R_F·n_f; returns d0 if d0 ≤ 0 else d1 (the depth of the end behind the plane).

Validity tests (return 0 = features valid, 1 = invalid/updated, 2 = penetration):
- `checkVV` 0x50d050: each vertex in the other's frame against the other's vertex planes; on violation sets that feature to the plane's neighbour, returns 1.
- `checkVE` 0x50bc90 / `checkVF` 0x50c660: vertex against the E/F planes → 1; VF: 0 if above the face plane else 2.
- `checkEE` 0x509f10: for both edges: closest point of the other edge's *line* inside this edge's two V–E planes (else 1); if behind all adjacent faces of this edge (planes 2..) → 2.
- EF: `VClip__clipEdge` 0x508fe0 (Mirtich clip of the segment against a plane range, λ∈[0,1], returns entering/leaving neighbours) then: λ-ends on opposite sides of the face → 2, else feature update → 1.

Two drivers:
- **`Phys__shapeDistance` 0x50d9d0 (single check, used every sub-iteration by `PhysicsWorld__updateContacts`)**: runs the test once for the cached feature pair (`pair+0x64/+0x68`); result 0/2 → distance (VV/VE only on 0; VF on 0 or 2; EE ±√; EF only on 2); result 1 → **returns 0.0, which makes the caller delete the contact** (feature updates are not written back except in EE). VE with 2 → −1.0. Unknown pairs → 0.
- **`VClip__closestFeatures` 0x50d230 (full V-Clip walk, used when creating contacts)**: starts from the passed features or the poly's first feature, loops `≤ nfeatA + 2·nfeatB` steps using the *stepping* versions `stepVE` 0x50be40, `stepVF` 0x50c830 (with `handleLocalMin` 0x50b7b0: if the vertex is below the face, picks the face of B with max signed distance; >0 → continue there, else penetration 2), `stepEE` 0x50a410, `stepEF`; FF → A := its first plane neighbour (an edge). Writes the final features back; on overrun prints "Voronoi error (infinite loop)" and returns 1e6.

## 4. Contact generation (`Collide__bodyPair` 0x4fb370, recursive over compound children)
1. Skip if either body has flag 0x20, a shape is missing/type 0/type 7. Body flag 0x800 → use a temporary shape built by `FUN_004faa40` (cached per body in 0x6054c0/0x605584).
2. Broad phase: `|Δpos|² ≤ (bA + bB + 0.05)²` (b = shape+0xc), then OBB-vs-sphere both ways: `|R_Aᵀ·Δ|_k − halfA_k ≤ bB` for k=x,y,z and vice versa.
3. Compound parents (child list `body+0x24`): the larger-radius side is descended first; children with flag 0x20 skipped.
4. Body flag 0x40 (sensor-type body; name of the flag inferred) on one side → `Collide__bodyVsStatic` 0x4fd910 (features start null; capsule special cases: edge–face with `n·dir<0` → `VClip__edgeCrossFace` 0x50b290 (edge of A crossing a face of B, gives crossing point and face normal); capsule vs. non-face: tilt the normal by the margin, `n' = (dir·√(R²−d²) + n·d)/R`); if d < rA+rB the margins are subtracted (`pB −= rB·n`, `d −= rB`) and `PhysicsWorld__addTriggerContact` 0x502d60 is called: **no physical contact** — without body flag 0x1000 it only queues script event 0x20 (with a temporary c_collision as payload); with 0x1000 it keeps a de-duplicated trigger record (list `world+0x2bc`) and sends event 0x20 to both scripts.
5. Normal pairs (neither 0x40 nor 0x400 primitive): `PairCache__find(A,B)` gives cached features (`+0x28/+0x2c`); **10-step time bisection** on the predicted motion (t=1.0, step 0.5, `PhysBody__interpolateState(prev, cur, tmp, t)`), each step `closestFeatures`; d > rA+rB → if d < rA+rB+0.02 and (first step or approaching) stop, else t += step; d ≤ rA+rB → t −= step (or stop if the cached pair is flagged `+0x44&1`). Features are written back to the pair cache. If final d < rA+rB+0.02: margins applied (`FUN_00503a60/00503b40`), deep penetration (d ≤ −(rA+rB)) is rejected, else `PhysicsWorld__addContact` 0x502830.
6. `PhysicsWorld__addContact(A, B, res, fA, fB, pA, pB, n, d, …, toi t, worldPt)`: per root body keeps only the **earliest-TOI** contact (`body+0x210` TOI, `+0x214` contact, flag 0x400); later TOI → rejected, earlier → the old one is removed; duplicates (same A,B,fA,fB) rejected. Allocates `c_collision` (0xcc), `c_collision__init`, `+0xc4 = t`, `+0x98 = worldPt`, appends to the contact list (`world+0x1d8`, count `world+0x60`), wakes both roots.

## 5. Contact constraint `c_collision` (vftable 0x59f914, base `constraint` 0x59db70)
Fields (`c_collision__init` 0x4f2770): `+0x2c` **mode** (6 = new impact, 7 = resting; `constraint__vf4_resolve` only acts on 6/7), `+0x34` rows = 1, `+0x38`/`+0x40` VecN {n, float*} (rhs / λ), `+0x48`/`+0x50` body A/B (ref links), `+0x58` optional resource instance (flag `+0xb4` bit0), `+0x64/+0x68` features, `+0x74` pA (A-local), `+0x80` pB (B-local), `+0x8c` normal (world, B→A), `+0x98` world point, `+0xa4` separation d (margins already subtracted), `+0xa8` extra, `+0xac` μ = min(μA, μB), `+0xb0` e = (eA+eB)/2 (0.5/0.5 if a body is missing), `+0xb8/+0xbc/+0xc0` accumulated normal impulse / bias force / friction force (reset when `DAT_006052ac`), `+0xc4` TOI, `+0xc8` = 1.0.
`updateContacts` keeps each pair alive: re-evaluates `Phys__shapeDistance` on the previous states, drops it if d == 0 (invalid features) or d > rA+rB+0.05, wakes bodies and clears `world+0x39c` (= "converged" flag `world[0xe7]`) if d < rA+rB+0.02, refines by 10-step bisection if penetrating, then subtracts margins: `pA −= rA·(R_Aᵀn)`, `pB += rB·(R_Bᵀn)`, `d −= rA+rB`, and stores the world point.

### Resolve `c_collision__resolve` 0x4f0dd0 (= vf4 for modes 6/7; called for every contact each sub-iteration)
Let dyn(X) = !(def.flags&4); skip if neither body is dynamic-and-not-0x80; d > 0.05 → 0. Compound children: pA/pB are lifted to the root frame through `child+0x1c` (offset +4, rows +0x20/+0x30/+0x40). Bodies with flag 0x20000: mode 6 only adds `1.1·body+0x18` to `+0xb8`, returns 0. dt = `g_physWorld+0xc`.
```
vrel = vA(pA) − vB(pB)          // PhysBody__pointVelocity: v + ω×(R pA), from the *other* state (idx^1)
vn   = n·vrel
mode 6: imp  = (vn < −0.2) ? −(1+e)(vn+0.2) : 0 ;  bias = 0
mode 7: imp  = 0 ;  bias = (both bodies' other-state bit0 set || d ≥ 0.03) ? 0 : (0.03 − d)·10
        if (vn < −0.2) bias −= (1+e)(vn+0.2)
if imp+bias > 0:
  K = n·( (I⁻¹A (rA×n))×rA + (I⁻¹B (rB×n))×rB ) + invmA + invmB      // r = R·p (current state idx), I⁻¹ = world inverse inertia
  if imp  > 0: P = imp/K;  applyImpulseAt(A, +P n, pA); applyImpulseAt(B, −P n, pB); +0xb8 += P
  if bias > 0: F = (bias/K)/dt; applyForceAt(A, +F n, pA); applyForceAt(B, −F n, pB); +0xbc += F
  N = (imp+bias)/K/dt
  if N > 0.001 and μ > 0:  vt = vrel − (vrel·n) n;  if |vt|² > 0.001:
      t = vt/|vt|;  Kt = same formula with t;  f = (|vt|/Kt)/dt;  if f > μN: f = μN (return 2) else return 1
      applyForceAt(A, −f t, pA); applyForceAt(B, +f t, pB); +0xc0 += f
if d < 0.021: c = (0.021 − d) − (n·offA − n·offB)       // off = state+0x18 position offset (added after integration)
  if c > 0: A static → offB −= c n; B static → offA += c n; else ±c/2;  return 1
on nonzero return: clear other-state bit0 of both (wake), world+0x14++.
```
`PhysBody__applyImpulseAt(J, pLocal)` 0x505450: `v += J·invm`; `L += (R p)×J`; `ω = I⁻¹_w L` (state idx). `PhysBody__applyForceAt(F, p)` 0x5051e0: `force += F`, `torque += (R p)×F` (consumed by the next integration). Since `World::step` re-integrates every root after each sub-iteration, impulses (velocity) and forces (bias, friction) both act through the RK4 re-integration from the start-of-step state.

### Impact pass `PhysicsWorld__resolveImpacts` 0x4ffd20
For every contact in mode 6: call vf4 (sum returned), clear both roots' flag 0x400/`+0x214`, queue script event type 1 (`Game__queueEvent(body+0x38, 0, 1, contact)`) for each body when its threshold `body+0x18` < accumulated impulse `+0xb8` or body flag 0x80 (also to the compound root), then **switch the contact to mode 7**.

### `PhysicsWorld__solvePass` 0x502410 (the "joint solver")
Walks `world+0xe8` (constraints; count `world+0x5c`, pruned by vf6 `isValid` in `pruneConstraints`). For each with rows `+0x34 ≠ 0`: vf7 (rhs → `*(+0x3c)`), vf8 (λ → `*(+0x44)`); N = Σrows; `world+0x3a0`, `+0x3a8` VecN(N) and `world+0x3b0` MatN(N×N) are sized; the per-constraint vectors are gathered into the world vectors (`VecN__copyBlock`) and scattered back; vf9 applies λ. **No global matrix solve happens** (the N×N matrix is allocated but unused) — each row is solved independently with the velocities from before the pass (Jacobi style).
c_collision rows: vf7 `rhs`: both immovable → 0; mode 6: `vn < −0.2 → −(1+e)(vn+0.2)` (otherwise rhs left unchanged); other modes: `b = d`, if (a body's other-state bit0 clear) and d < 0.03 → `b = (0.03−d)·10`; if vn < −0.2: `b −= (vn+0.4)(1+e)` (sic). vf8: `λ = b/K` (K as above, 0 if both immovable). vf9: mode 6 → `applyImpulseAt(±λ n)`, else `applyForceAt(±(λ/dt) n)`.
`constraint` base and `c_triggerspring` have vf7–vf9 = `guard_check_icall` (empty), so in practice only c_collision-type rows are solved here. **Open**: which objects are inserted into `world+0xe8` (no writer found yet; contacts go to `+0x1d8`); joint types from `typ_physics_constraint` resources are not identified.

## 6. Other
- `Collide__triggerTest` 0x4fbda0: bodies flagged 0x48 (triggers) → `closestFeatures` vs. the trigger shape; flag 8 → returns `d < r+0.02`; capsule edge vs. face orientation filter; creates trigger contacts via `FUN_00502d60` (list `world+0x2bc`, count `world+0x64`, event 0x20 to both scripts). `PhysicsWorld__removeTrigger` 0x4fb070 sends event 0x40 on separation.
- `Collide__candidatesTOI` 0x4fc7e0: TOI search for `body_dummy_candidate` list `DAT_006028f8` (same 10-step bisection; if still penetrating picks the vertex with min distance along the normal) → `addContact`.
- `PhysicsWorld__init` 0x4fffb0: dt `world+8 = world+0xc = 0.02`, gravity `world+0x1c4 = (0, −9.81, 0)`, `world+0x364 = 1.2935` (air density?), `+0x368 = (0,−7,0)`, `+0x374 = (0,−1,0)` (default ground plane point/normal for the 0x800 plane contact), `+0x394 = 300`, `+0x398 = 550`, two static root bodies `world+0x84` (all dynamic bodies are children of it, `PhysicsWorld__addBody` 0x4f7830) and `world+0x88`. Global world pointer `g_physWorld` 0x5dadf0.
- `PhysicsWorld__purgeEvents` 0x5040f0 frees collision events whose body has flag 0x200000.
