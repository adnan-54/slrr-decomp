# Collision shapes, V-Clip narrow phase, contact constraints

Read from the exe (Ghidra decompile + assembly where ECX mattered). Companion to `08-physics.md`. New names: `analysis/names.physics.json`.
"state" = one of the two body state buffers (`body+0x64 + idx*0xd4`, see 08): `s+0x00` pos, `s+0x0c` linear vel, `s+0x18` position-correction offset, `s+0x28` force accumulator, `s+0x40/+0x50/+0x60` rotation rows R (3×4), `s+0x70` ω, `s+0x7c` angular momentum L, `s+0x88` torque accumulator, `s+0xa4/+0xb4/+0xc4` world inverse inertia rows (3×4), `s+0x3c` flags (bit0 = "integrated, untouched since": set by 0x5001c0, cleared by 0x4f8460 at step start and by contact resolve to force re-integration). Local→world: `p_w = pos + R·p` (rows: `x = pos.x + R[0]·p` …). World→local: `p = Rᵀ(p_w − pos)`.

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
- `RigidBody__setSphereInertia(_, R)` 0x5056a0 (only caller `Physics__createBox` 0x4f7d60: type 7 with R = max‖v‖+0.05 of the loaded .scx, or R = 1.0 without meshRes; type 6 **without meshRes** with R = 1.0, e.g. the `part_comp` body of 0x4c1f90): invI = **2.5/(m·R)** (R not squared — as in the exe), volume 4/3πR³.
  Re-checked 2026-10-24 (decompiled 0x505760, 0x5056a0, 0x4f7d60): type 1 spheres (createBox case 1, cfg `sphere`/`ball` shapes, every shipped wheel `type` line such as `10.000 sphere 0.315`) always go through 0x505760 = 2.5/(r²·m); only the two createBox cases above use 0x5056a0. So src `compute_mass_props` (world.cpp:75, r²) is right; there is no R-vs-r² conflict. Gap (no behaviour today): src leaves type 7 and mesh-less type 6 at inverse inertia 0; for `part_comp` 0x5032d0 overwrites the inertia from massive children (or makes it static), and src creates no type-7 body.
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
**Resolved: the "kinds" 1/2/3 are V-Clip Voronoi feature kinds, not shape types.** The shape object itself has type 0..7 (§1); the pair dispatcher works on the cached Voronoi features of the shapes' polyhedra (built by `Phys__shapeInit`/`Poly__build*`, §2):
- **1 = V (vertex)**, **2 = E (edge)**, **3 = F (face)**.

Globals: `g_vclipState[2]` 0x60617c (state A,B), `g_vclipPoly[2]` 0x606174, `g_vclipFeature[2]` 0x606184 (current features), `g_vclipSwap` 0x6060e0 (**persistent** 0/1 index; handlers address A/B as `[swap]`/`[swap^1]` and the result normal is negated when swap==1). Pair key = `kind[swap]*0x100 + kind[swap^1]`; mixed pairs are normalised so `[swap]` holds the lower-dimensional feature (V before E before F).
Outputs: points in each body's **local** frame, normal in world pointing **from B to A**, return = signed distance.

Pair handler map (feature-kind key → validity test / distance routine):
| key | test | distance | features |
|---|---|---|---|
| 0x101 | 0x50d050 | 0x50ce30 | V–V |
| 0x102, 0x201 | 0x50bc90 | 0x50b870 | V–E |
| 0x103, 0x301 | 0x50c660 | 0x50c400 | V–F |
| 0x202 | 0x509f10 | 0x509130 | E–E (→ `Math__segSegClosest2` 0x509740) |
| 0x203, 0x302 | 0x50aa60 + `VClip__clipEdge` | inline (0x50e2d0 frame transforms) | E–F |

Handler parameters are the local feature data from §2: V = one vertex `p`; E = tail/head vertices (`dir = normalize(head−tail)`, `len = dir·(head−tail)`); F = plane point `q` and unit outward normal `n_f`, plus the face rotation R_F of the owning body.

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

### Resolve `c_collision__resolve` 0x4f0dd0 (called for every contact each sub-iteration)
Vtable slot 4 of both `constraint` and `c_collision` is the wrapper `constraint__vf4_resolve` **0x4f0db0**: `if (mode − 6u < 2) return c_collision__resolve(); else return 0;` — so mode 6/7 reaches 0x4f0dd0, the body of the routine below. (The earlier note's 0x4f0dd0 is the real body; do not confuse it with the wrapper.)
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
on nonzero return: clear other-state bit0 of each dyn side, world+0x14++.
```
Details re-checked against the 0x4f0dd0 decompile:
- A side that is not dyn contributes nothing to K (its r, I⁻¹ term and invm are 0) and gets no impulse/force; r = R·p and I⁻¹ come from the **active** state (`PhysBody__rotateToWorld` 0x5061f0, state +0xa4), vrel from the **other** (integrated) state.
- Friction uses this call's `N` only (not the accumulated +0xb8) and `vt` from the vrel taken before this call's response. The position-correction branch returns 1 even when friction clamped (2).
- Mode 7 puts both the spring and the restitution term in `bias`, i.e. it acts as a **force**; only mode 6 applies an impulse.
- Other-state flags bit0 is not a sleep bit: `0x5001c0` sets it after integrating a body and skips bodies that still have it, `0x4f8460` clears it at step start, resolve clears it to force re-integration. So "both bit0 set" = neither body touched by a contact earlier in this pass.
- `off` is the active state's s[6..8]; the integrator `0x506530` adds it to the integrated position, so `d` (measured on the integrated state) already includes earlier corrections.
`PhysBody__applyImpulseAt(J, pLocal)` 0x505450: `v += J·invm`; `L += (R p)×J`; `ω = I⁻¹_w L` (state idx). `PhysBody__applyForceAt(F, p)` 0x5051e0: `force += F`, `torque += (R p)×F` (state idx, kept until the next step's force model). The active state idx is the start-of-step state that `0x5001c0` integrates into the other buffer, so impulses (velocity), forces (bias, friction) and the position offset all act through the RK4 re-integration after the pass; within a pass, later contacts do not see earlier impulses (vrel reads the other state).

### Impact pass `PhysicsWorld__resolveImpacts` 0x4ffd20
For every contact in mode 6: call vf4 (sum returned), clear both roots' flag 0x400/`+0x214`, queue script event type 1 (`Game__queueEvent(body+0x38, 0, 1, contact)`) for each body when its threshold `body+0x18` < accumulated impulse `+0xb8` or body flag 0x80 (also to the compound root), then **switch the contact to mode 7**.

### World intrusive-list layout (resolved)
All world lists are `TList` objects (`TList::vftable_TList` 0x59391c, dtor 0x493d00 → body 0x41fd90). A **`TListNode`** base is **0x24 bytes**: vtable +0, then **four independent doubly-linked link slots**, each `{next, prev}`: slot0 +0x04/+0x08, slot1 +0x0c/+0x10, slot2 +0x14/+0x18, slot3 +0x1c/+0x20. A **`TList`** is **0x4c bytes**: TList vtable +0; head sentinel TListNode at +0x04 (its vtable at +0x04, links +0x08..+0x27); tail sentinel TListNode at +0x28 (vtable +0x28, links +0x2c..+0x4b). Empty slot *k*: `head.next[k] = &tail`, `tail.prev[k] = &head`, `tail.next[k] = 0`, `head.prev[k] = 0`; the walk stops when a node's `next` is 0, i.e. the tail sentinel is never processed. `TList__dtor` 0x41fd90 unlinks each node from **all four** slots. World constructor `PhysicsWorld__ctor` 0x4f7050 builds nine such TLists; cleanup `FUN_004fabb0` drains some of them.

For a list at base `B`, slot0: first = `*(B+8)`, tail sentinel = `B+0x28`, last = `*(B+0x30)`. All lists below use slot0 unless noted.

| base | first / tail / last | counter | contents and operations |
|---|---|---|---|
| world+0x94 | +0x9c / +0xbc / +0xc4 | `+0x58` | **active controllers** (incl. `c_triggerspring`). Append `PhysicsWorld__addController` 0x4f7960 (`last->next=new; new->next=tail; new->prev=last; last=new; +0x58++`); ticked by `runControllers` 0x4ffca0 → `vf3`; if `vf3() < 0` unlink, `+0x58--`, `vf0(1)`. Cleanup drains it. |
| world+0x12c | +0x134 / +0x154 / +0x15c | `+0x58` (observed; shared active+retired total is inferred) | **retired controllers**. `c_triggerspring` `vf3` appends itself here when it becomes invalid/static. Cleanup drains it. |
| world+0xe0 | +0xe8 / +0x108 / +0x110 | `+0x5c` | **solver-pass list**. `PhysicsWorld__solvePass` 0x502410 walks slot0; `pruneConstraints` 0x4fa4d0 removes nodes whose `vf6` (vtable slot 6, offset +0x18) returns 0, `+0x5c--`, `vf0(1)`. **No producer found — see verdict below.** Cleanup does *not* drain it. |
| world+0x178 | +0x180 / — / — | — | unknown; cleanup does not drain it. |
| world+0x1d0 | +0x1d8 / +0x1f8 / +0x200 | `+0x60` (removed `+0x1c`) | **contact pairs** (`c_collision`). `addContact` 0x502830 appends; `updateContacts` 0x4f9af0 walks; cleanup drains (calls `vf+8`, unlinks, `vf0(1)`). |
| world+0x21c | +0x224 / — / — | `+0x60`/`+0x1c` same as contacts | drained exactly like the contact list; semantics unknown (second contact/cache list). |
| world+0x268 | +0x270 / — / — | `+0x68` | unknown; cleanup drains; each entry's `+0x3c` (= +0xf0) pointer has `+0x1c` cleared and `+0x40` (= +0x100) is passed to `FUN_004f6a80`. |
| world+0x2b4 | +0x2bc / +0x2dc / +0x2e4 | `+0x64` | **trigger contacts** (`PhysicsWorld__addTriggerContact` 0x502d60; `removeTrigger` 0x4fb070). |
| world+0x300 | +0x308 / +0x328 / +0x330 | — | unknown; cleanup drains via `FUN_004fb2a0` only if `world+0x70 > 0`. |

### `PhysicsWorld__solvePass` 0x502410 (the "joint solver") — exact pass
```
solvePass(world):                    // ECX=world, 274 bytes
  N = 0; off = 0
  for (node = *(world+0xe8); node != 0 && *(node+4) != 0; node = *(node+4)):
      if (node->rows (+0x34) == 0) continue
      vf7(node)                      // vtable slot 7 (offset +0x1c): compute rhs into node+0x38 VecN
      vf8(node)                      // slot 8 (offset +0x20): compute lambda into node+0x40 VecN
      N += node->rows
  if (N == 0) return
  VecN__setSize(world+0x3a0, N)      // 0x569ea0
  VecN__setSize(world+0x3a8, N)
  MatN__setSize(world+0x3b0, N, N)   // 0x569cc0 — allocated, never filled or solved
  off = 0
  for node in list:                  // gather
      if rows: copyBlock(dst=world+0x3a0, src=node+0x38, srcOff=0, count=rows, dstOff=off)
               copyBlock(dst=world+0x3a8, src=node+0x40, srcOff=0, count=rows, dstOff=off)
               off += rows
  off = 0
  for node in list:                  // scatter back + apply
      if rows: copyBlock(dst=node+0x40, src=world+0x3a8, srcOff=off, count=rows, dstOff=0)
               vf9(node)             // slot 9 (offset +0x24): apply lambda
               off += rows
```
`VecN__copyBlock` is 0x569db0 (ECX = destination VecN; the pushed stack args in order are `src, srcOff, count, dstOff`). Rows/vectors live on the constraint base: `+0x34` rows (int), `+0x38` rhs VecN `{n, float*}`, `+0x40` lambda VecN `{n, float*}` (the float pointers are at +0x3c / +0x44).

Ordering: **all** `vf7`/`vf8` (rhs and λ computation) run before **any** `vf9` (application), and each row computes against the velocities at pass start — a Jacobi / semi-implicit pass, not sequential impulses. The gathered rhs vector at `world+0x3a0` and the `N×N` matrix at `world+0x3b0` are never read; the λ vector at `+0x3a8` is copied out and back unchanged. **There is no global LCP/matrix solve.**

`c_collision` is the only class with non-empty row vfuncs (`vf7` 0x4f2180, `vf8` 0x4f23a0, `vf9` 0x4f1fa0):
```
vf7 rhs:
  A=+0x48, B=+0x50; if either is null: return (rhs unchanged)
  pA=+0x74 (A-local), pB=+0x80, d=+0xa4, e=+0xb0, n=+0x8c
  dyn(X) = !(defX.flags(+0xb4) & 4)
  if (!dyn(A) || A.flags&0x80) && (!dyn(B) || B.flags&0x80): rhs = 0; return
  vA = pointVelocity(pA); vB = pointVelocity(pB); vn = n·(vA − vB)
  if mode != 6:                                  // resting/other
      b = d
      if ((prevState(A).flags & 1)==0 || (prevState(B).flags & 1)==0) && d < 0.03:
          b = (0.03 − d) * 10
      if vn < −0.2: b -= (vn + 0.4) * (1 + e)     // observed (decompiled as ((vn+0.2)+0.2)); note the 0.4, unlike vf4)
      rhs = b
  else:                                          // mode 6 = impact
      if vn < −0.2: rhs = −(1 + e) * (vn + 0.2)
      // else no store: rhs keeps its previous value (uncertain: VecN init state)
vf8 lambda:
  if either body null: return
  if !(dyn(A) && !(A.flags&0x80)) && !(dyn(B) && !(B.flags&0x80)): lambda = 0; return
  rA = R_A·pA, rB = R_B·pB                     // active state idx (+0x20c)
  uA = IA⁻¹(rA×n), uB = IB⁻¹(rB×n)             // world inverse inertia at state+0xa4 = body+0x108... (rows +0x108/+0x118/+0x128)
  K = n·(uA×rA + uB×rB) + invMassA + invMassB  // invMass = def+0x2c
  lambda = rhs / K
vf9 apply:
  if either body null: return
  lambda = *(VecN data)
  dynA = !(defA.flags & 4), dynB = !(defB.flags & 4)   // body flag 0x80 is NOT tested here
  if mode == 6: dynA ? applyImpulseAt(+lambda·n, pA); dynB ? applyImpulseAt(−lambda·n, pB)
  else:         f = lambda / world.dt;
                dynA ? applyForceAt(+f·n, pA); dynB ? applyForceAt(−f·n, pB)
```
The `prevState` flags are at `body + (idx^1)*0xd4 + 0xa0` (state flag word `s+0x3c`, bit0). `PhysBody__pointVelocity` reads the **other** state (`idx^1`) in vf7; `PhysBody__rotateToWorld` and the inertia used by vf8 read the **active** state (`+0x20c`).

### Verdict: the joint solver is vestigial in 2.3.1
- Only `c_collision` implements vf7/vf8/vf9. The base `constraint` and `c_triggerspring` have vf7–vf10 = 0x52f990 (`guard_check_icall`, decompiles to an empty function) and vf11–vf12 = 0x4f0da0 (`return 0`).
- `c_collision__init` 0x4f2770 has only three callers: `addContact` 0x502830, `addTriggerContact` 0x502d60, `removeTrigger` 0x4fb070. `addContact` inserts the pair **only** into the contact list at world+0x1d0; trigger contacts use world+0x2b4.
- A node has one link pair per slot, so a pair in the contact list (slot0) cannot simultaneously be in the solver list (also slot0). No code writes the solver list's last pointer `world+0x110` (searched `89/8b ?? 10 01 00 00`, `8d ?? 08 01 00 00`) and no code increments its counter `world+0x5c` (searched `83 ?? 5c 01`, `ff ?? 5c`, `89 ?? 5c` over 0x4e0000–0x50a000; only `pruneConstraints` decrements it). `world+0x5c` starts at 0.
- Therefore **`solvePass` is a runtime no-op in this build**: the contact response actually used is the per-contact `vf4` → `c_collision__resolve` (0x4f0dd0) loop over the contact list `world+0x1d8` in `World::step`, and the spring/suspension behaviour is the `c_triggerspring` controller tick (below), not solver rows.
- No hinge/slider/ball/fixed joint class exists in the RTTI table: under `constraint` only `c_collision` and `c_triggerspring` appear. Resource type 10 exists (`RESTYPE_PHYSICS_CONSTRAINT`, `ResourceType_Physics_Constraint` vftable 0x5a85a0), but its identified vtable methods are generic resource plumbing (vf10 0x520c70 calls helper 0x520ce0 and stores a float at +0x98; vf7 0x520dc0 toggles a resource flag), and no code path creating a joint object was found. **Uncertain**: a writer through an unidentified generic list helper cannot be excluded absolutely, but all direct evidence (counters, link fields, callers, class table) says the list is never populated and the pass never contributes.

### `c_triggerspring` — spring/suspension controller (vftable 0x59dbb8; RTTI name; called `Suspension` by Ghidra)
Not a solver-row constraint: it derives `constraint`/`controller`, lives in the world controller list, and is ticked via `vf3` by `runControllers` 0x4ffca0.
- **ctor** 0x4915d0 (`RET 0x18`): zeroes dwords +4..+0x44, sets vftable 0x59dbb8, zeroes +0x94/+0x98, +0xac/+0xb0, +0xd0/+0xd4, then calls init 0x4f3f10. Object is **0x104 bytes** (allocated with `Mem__alloc(0x104)`).
- **init** 0x4f3f10: builds a world attach point from the passed attach vector/local offset `(0, −L/2, 0)` transformed by the parent body's active state; creates a **sensor capsule child body** with `Physics__createBox(mass 0.01, type 3 = capsule along Y, a = L/2, b = 0.05, c = 0, mesh 0)` (constants 0x3c23d70a, 0x3d4ccccd), sets body flags `|= 0x40` (sensor/trigger), and adds it via `PhysicsWorld__addChildBody` 0x4f78d0. The sensor body is referenced from this+0x94 (unlink old via `RefLink__unlink` 0x501ca0, refcount++); `ResourceRef__set(body+0x38, *(this+0xc))` binds the resource instance.
  Field values written (observed; names inferred): `+0x48 = L`; `+0x4c = 0.9`; `+0x50 = L` else 0.9 if `L ≤ 0`; `+0x54 = 0`; `+0x58 = L` else 0; `+0x5c = 800.0`; `+0x60 = 200.0`; `+0x64 = 140.0`; `+0x68 = L` else 0.9; `+0x70 = L/2`; `+0x6c`/`+0x74..+0x8c`/`+0x90` = 0; `+0xb4 = (+0x50)² + 0.1`; `+0xf0 = +0x50`; `+0x9c..+0xa4`, `+0xa8`, `+0xd8..+0xe0`, `+0xe4..+0xec`, `+0xf4`, `+0xf8` = 0. Finally registers itself: `addController(g_physWorld, this)` (world+0x94 slot0, world+0x58++).
  Creation site: **0x4992e0** (car/wheel setup, one spring per create call): allocates 0x104, calls the ctor with the parent body, an attach context, a world attach point and direction `(0,−1,0)`, plus two floats read from the car record (`+0x5c`, `+0x64` of the wheel/part record). Function/field names there are inferred.
- **tick `vf3`** 0x4f2d70: sensor = `[this+0x94]`; if null return 0. Other body = `[this+0xd0]`. If both bodies are invalid/static, unlink this from its current controller list and append it to the **retired** list at world+0x12c (tail `world+0x15c`), return 0. It maintains the current length against `+0xb4` (previous length²+0.1; `sqrt` gives the previous length), updating `+0xf0` (current length) and `+0xb4 = clamp_len² + 0.1`. It computes end points from the attach point `+0x6c/+0x70/+0x74` and a direction (globals `DAT_005dadd0/d4/d8`), with the lower end at `+0xd8..+0xe0 = attach − length·dir`, evaluates point velocities at both ends, and applies spring (`+0x5c` = 800, `+0x60` = 200), damper (`+0x64` = 140, `+0x4c` = 0.9) and bump-stop terms; a bump-stop branch triggers when current length `< +0x54` with negative compression velocity, computes an effective inverse mass from `def+0x2c` and applies `impulse/dt`. Forces go through `PhysBody__applyForceAt` at `+0xd8` on the sensor body and the opposite at `+0xb8` on the other body; the other state's bit0 is cleared (wake) and the roots woken. Outputs stored: `+0xf4` spring force, `+0xf8` relative velocity, `+0xfc` damper force, `+0x84..+0x8c` force vector, `+0xe4..+0xec`. **Uncertain**: exact per-term algebra and the physical names of `+0x48/+0x4c/+0x50/+0x54/+0x58/+0x60/+0x64/+0x68`; the vf3 body has only been read in assembly/summary form, not fully re-decompiled.

## 6. Other
- `Collide__triggerTest` 0x4fbda0: bodies flagged 0x48 (triggers) → `closestFeatures` vs. the trigger shape; flag 8 → returns `d < r+0.02`; capsule edge vs. face orientation filter; creates trigger contacts via `FUN_00502d60` (list `world+0x2bc`, count `world+0x64`, event 0x20 to both scripts). `PhysicsWorld__removeTrigger` 0x4fb070 sends event 0x40 on separation.
- `Collide__candidatesTOI` 0x4fc7e0: TOI search for `body_dummy_candidate` list `DAT_006028f8` (same 10-step bisection; if still penetrating picks the vertex with min distance along the normal) → `addContact`.
- `PhysicsWorld__init` 0x4fffb0: dt `world+8 = world+0xc = 0.02`, gravity `world+0x1c4 = (0, −9.81, 0)`, `world+0x364 = 1.2935` (air density?), `+0x368 = (0,−7,0)`, `+0x374 = (0,−1,0)` (default ground plane point/normal for the 0x800 plane contact), `+0x394 = 300`, `+0x398 = 550`, two static root bodies `world+0x84` (all dynamic bodies are children of it, `PhysicsWorld__addBody` 0x4f7830) and `world+0x88`. Global world pointer `g_physWorld` 0x5dadf0.
- `PhysicsWorld__purgeEvents` 0x5040f0 frees collision events whose body has flag 0x200000.

## Voronoi error in vmboot (RE 2026-10-24, cards 165/166)
- String + condition are the exe's: 0x50d230 loops `<= nfeatA + 2*nfeatB` steps, then prints
  "Voronoi error (infinite loop)" (0x564ca0), writes the features back, returns 1e6. src `vclip_closest` matches.
- vmboot (HEAD 42a210b, SunStrip pick): 248 lines, all from `consider_hull_pair` (car hull child, closed,
  38..194 features, vs two-sided soup triangle, 8 features), all inside the INVENTED forced-drag run after
  the car crashes near z -450..-497 at ~44 m/s. 0 during normal driving. Scratch walk trace: 2-cycles
  `F(hull)/E(tri) <-> E(hull)/E(tri)` with the hull centre 0..0.6 m from the face plane (deep penetration).
- Cause: src creates hull-vs-soup contacts only at step start pose (static filter, cold features), so a
  fast hull is deep inside before V-Clip sees it. Exe: event phase after integrate, swept filters
  (0x4feb20), 10-step TOI bisection prev->cur (0x4fc7e0, 0x506c00 interp: t=1 integrated, t=0 start),
  features warm-started from the pair cache (0x4f6ea0). Card 165.
- Exe walk stepping (src uses INVENTED Mirtich-like rules + "largest violated plane"):
  `clipEdge 0x508fe0(seg[6], feat, &N[2], &lam[2], from, to)`: N cleared; per plane i: dt,dh = n.(p-pt);
  dt>=0,dh<0: l=dt/(dt-dh), if l<hi {hi=l; Nhi=nb; if lo>hi return 0}; dt<0: dh<0 -> Nlo=Nhi=plane[from].nb, return 0;
  else l, if l>lo {lo=l; Nlo=nb; if lo>hi return 0}; return 1.
  `stepEE 0x50a410`: for each side s (2 passes): other edge in s frame; clip planes 0..2 (V-E): Nlo==Nhi!=0 -> step;
  Nlo && (x(lo)-V).u > 0 -> step Nlo; Nhi && (x(hi)-V).u < 0 -> step Nhi; clip planes 2..n (E-F, lo/hi carried on):
  same with D_F(x)*(n_F.u) (>0 lo, <0 hi); both passes clean -> 0 (valid; 0x50d230 returns sqrt(0x509130)).
  `stepEF 0x50aa60`: edge in face frame; clip all face planes; ok: D(lo)*D(hi)<0 -> 2; D(lo)*(n.u)<0 -> Nhi or head V,
  else Nlo or tail V; clip failed: walk the face boundary edges from the excluding plane (clip vs plane i, then vs
  edge i's V-E planes, move to the neighbour edge sharing the clipped vertex) -> face side := that edge. Never 0.
  Scratch port of stepEE+stepEF alone raised the count to 2552; the full 1:1 port (card 165 follow-up) gives 0.
- Full walk (ported in src/physics/vclip.cpp): 0x50d230 dispatches kind[side]*0x100+kind[side^1] with
  side = global DAT_006060e0 (never reset; persists between queries). 0x102/0x201 stepVE, 0x103/0x301 stepVF,
  0x203/0x302 stepEF (side flipped so it names V resp. E), 0x202 stepEE, 0x101 checkVV, 0x303 A := A.planes[0].nb.
  stepVE: V vs E planes in order (first violated -> E steps), then clip E vs V planes (lo/hi derivative test).
  stepVF: F planes min (start 1.0) < 0 -> F steps; else for each V plane edge, far endpoint e:
  |d_e| < |d_V| or d_e*d_V < 0 -> V steps to that edge; else d_V <= 0 -> handleLocalMin (max face over all
  faces from -1e8, > 0 step else penetration 2). checkVV: both sides, first violated plane. stepEF fail path walks
  face planes i -> i+1 / i-1 by which neighbour edge holds the clip vertex.
- Determinism: Math.randomize 0x4dee90 = srand((int)0x569730()) (QPC ms); src reads steady_clock -> dealer roll
  varies with load. Card 166: vmboot-only switch to the scheduler's frame clock (INVENTED).

## Script triggers: `native trigger` (system 0x34) and EVENT_TRIGGER_ON/OFF (RE 2026-10-24, cards 167/168)
Race flow first (vmboot probe, scratch build): by the drive start the career race already ran its script
countdown: `Gamemode.launch` `addTimer(0,3)` -> `CircuitRacing.handleEvent` p3 -> THOR method 4 -> `run()`
`addTimer(1,4)` -> four EVENT_TIME p4 ticks (countDown 3..0, `lockCars` "start"+"stop"+"idle") -> `beginRace`
-> `unlockCars` -> `unlockPlayerCar` = car "start" (0x4a3fa6 clears 0x40000|4). Probe: countDown 0,
raceActive 1, car still asleep. Nothing in the shipped flow sends "wakeup" (`Gamemode.wakeUpCars` call is
commented out, Track.java:1056); the exe wakes the placed car on the first control input (0x498599, notes/09),
so vmboot's queued "wakeup" stays an INVENTED settle fixture. FIRST real blocker: src has no trigger type at all
(no body, no event 0x20/0x40), so `handleCheckpoint`/`handleRacePos`/`handlePitStop` never run and no lap,
position or finish can happen.

Script side (game/Trigger.java, Gamemode.setupCheckpoints/setupRacePosCalc, CircuitRacing.applyTriggers):
`new GameRef(gTrack.map, system:0x34, pos.toString()+",0,0,0,box,"+x+","+y+","+z, alias)`; Norisring (mod)
makes 4 checkpoint boxes 30/30/5, 3 pit boxes 2/2/5, 39 race-position boxes 40/40/5, then
`addNotification(trigger, EVENT_TRIGGER_ON(0x20), EVENT_SAME, null, "handleX")`. Handlers read
`param.token(0).intValue()` and compare with `GameLogic.player.car.id()`.

`type_trigger` (vtable 0x5a00e0) vf3 create 0x4d4700(type, params):
```
  T = alloc 0x38; ResRefLink__set(type+0xc);
  sx = 10.0, sy = sz = 0; pos = ypr = 0; kind[0] = 0;
  Str__scan(params, "%f,%f,%f,%f,%f,%f,%64s,%f,%f,%f", pos3, ypr3, kind, sx, sy, sz);
  if (kind == "" || kind == "sphere") def = Physics__createBox(0, 1 /*sphere*/, sx /*radius*/, 0, 0, 0);
  else if (kind == "box") def = createBox(0, 5, sx, sy, sz, 0);     // half extents (notes/11 §1)
  else if (kind == "rod") def = createBox(0, 4, sx, sy, sz, 0);     // capsule along z
  else                    def = createBox(0, 6, 1, 1, 1, kind);     // named .scx hull
  def+0xb4 |= 0x20; def+0xb4 &= ~4;                                  // NOT the flag-4 static kind
  R = Math__eulerToMat(ypr);
  Physics__attachToResource 0x4e8a50(T, def, pos, R): World add 0x4f7830(def, pos, R) + bind to the instance
  body = GameRef__getPhysBody(); body+0x58 = (body+0x58 & ~1) | 0x15040;   // 0x40 sensor, 0x1000 keep record
  (render/LOD flag work on type+0xc follows; no physics effect)
```
mass 0 -> never integrated. vf4 0x4d4a60 destroy: 0x4e9450 detach + 0x4fb2a0 release the body.

Pair test: `Collide__bodyPair` 0x4fb370 step 4 (flag 0x40 on one side) -> 0x4fd910 V-Clip, no physical
contact; `d < rA+rB` -> `PhysicsWorld__addTriggerContact` 0x502d60(A = sensor body, B, ...):
```
  if (!(A+0x58 & 0x1000)) { c_collision tmp(A,B); queueEvent(A.res, 0, 0x20, tmp); return; }
  B = root(B) (follow +0x20 while +0x58 & 4);
  for rec in world+0x2bc: if (rec.A == A && rec.B == B && rec+0x50 == B+0x40) { rec+0x34 = world.time; return; }
  rec = new (0x58): A, B, +0x34 = world.time; append world+0x2bc, world+0x64++;
  queueEvent(A.res, 0, 0x20, rec); queueEvent(B.res, 0, 0x20, rec) (+ root's res if different)
```
Sweep `0x4fa900` (only caller PhysicsWorld__step 0x502060), every record:
`if (!A) remove; else if (!B || rec+0x34 < world.time - 2*world.dt) { if (A && B && |posA-posB| < bA+bB+0.05) keep; else remove }`
(pos = other-state +0x64, b = shape bound radius +0xc). Remove 0x4fb070: `queueEvent(A.res,0,0x40,rec)`,
`queueEvent(B.res,0,0x40,rec)` (+root), unlink, world+0x64--, delete.
Param string (dispatchScript 0x47c960 case 0x20/0x40): `sprintf("%d", other)` where `other = rec.A+0x40` (the
body's resource id) unless that equals the dispatch object's id (sender ? sender : target), then `rec.B+0x40`.
A notification forwards with sender = the trigger, so `handleCheckpoint(trigger, 0x20, "<car id>")`.

Script caveat: Trigger.pos for race-position triggers is the shared raceposData Vector3, and
`Gamemode.getSplineDistance` flattens it in place (`Vector3.to2D`: `y = 0; return this;`), so after the first
position update rp[k].pos.y is 0.0 while the sensor body keeps the creation y. Every script assignment converts
to the target's declared type (JVM::assignmentConversion 0x5552e0 from assign 0x55e020), fields included,
so that 0 is a float.

## Contact refresh `PhysicsWorld__updateContacts` 0x4f9af0 for hull-vs-soup pairs (RE 2026-10-24, card 171)
Probe (scratch vmboot, spline driver run on past cp0, 30000 frames): the car reaches cp1 (nt 2, f5182), then at
f6011 (-38.9,-3.9,-101.9, ~7 m/s into a wall) one hull-vs-soup contact (child 10, tri 28594, n (0,0,-1)) refreshes
with sep -0.53 and keeps the SAME world point (-39.98,-3.66,-99.64) for 4 frames while the car moves 8 m: |v| 6 ->
50 -> 94 -> 116 m/s, |w| at the 19.68 cap, car launched 40 m high, lands 86 m off the route. Second one at f8801
(stationary at a wall, child 10 tri 65385 sep -0.78, n (0.99,-0.12,0)): 0 -> 118 m/s, then contacts with n (0,-1,0)
and the car falls through the map (y -81780 at f30000). Cause in src `refresh_hull_contacts` (world.cpp ~1112):
`Contact c = old` keeps the old points/normal and only replaces the separation, and the INVENTED signed fallback
`hull_plane_distance` measures against the INFINITE face plane (stays -0.5 m wherever the car is along the wall).
Exe 0x4f9af0, per pair (A = +0x48, B = +0x50, shape B = override +0x58 (+0x2c) when set; states idx^1 = integrated):
```
d = 0x50d9d0(stA, stB, shA, shB, &+0x64, &+0x68, &+0x74 ptA, &+0x80 ptB, &+0x8c n)  // writes points/normal/features
if d == 0 or d > rA + rB + 0.05: delete pair
if d < rA + rB + 0.02: wake both roots (state flag bit0), w+0x39c = 0
if (d > rA+rB) or (d > -(rA+rB)) or (old +0xa4 >= rA+rB):        // for a mesh pair (r = 0): d > 0 or old sep >= 0
    if d <= rA + rB: 10-step bisection t = 0.5, step 0.25 (0x506c00 both bodies, same rule as 0x4fc7e0:
        d > r: if d < r+0.02 and n.(pA - pB) < 0 (0x505d60) break; t += step;  else t -= step); still <= r: 0x50d9d0 again
        at the integrated states
    margins: +0x74 -= rA * R_A^T n, +0x80 += rB * R_B^T n, +0xa4 = d - rA - rB
    +0x98 world point = R_A * +0x74 + posA  (state used for the last 0x50d9d0)
else (d <= -(rA+rB) and old sep < rA+rB, i.e. already deep last time): margins as above, +0xa4 = d, +0x98 NOT updated
```
So: no plane fallback, the cached features/points/normal come from 0x50d9d0 every refresh, a 0.0 result deletes the
contact. Script-side race state at that point is fine (laps 2, `fines` null = event has no useFines, 4 checkpoints).
