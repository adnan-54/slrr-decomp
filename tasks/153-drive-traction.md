---
id: 153
status: todo
branch: card/153
depends: [152]
---
# Car compound: installed part masses + shape inertia (0x4c7d20 -> 0x5032d0, 0x508d20)

## Why (RE 2026-10-24, Ghidra + scratch debug build of 421e928; notes/09 "Card 153 traction RE")
The vmboot car is the **Duhen SunStrip 1.8 DVC** (cfg body lines 742.297 kg, car_root 782.337 kg),
not the Axis. At rest the springs already carry exactly the weight (7677 N vs 7674.7 N) and the
split matches the cfg compound COM (expected 1489 N driven front / 2349 N rear per wheel, measured
1461/1477/2375/2364). The old "11.8 kN" was the Axis mass. Two compound terms are missing:
1. `Part__commitInstall 0x4c7d20` joins every installed part's body into the car compound and
   re-runs `PhysBody__updateCompoundMass 0x5032d0`; src `part_attach` skips it. The SunStrip's 53
   installed parts with a body weigh **460.1 kg**, COM z ~ -0.49 (engine/gearbox/front
   suspension): the exe car is ~1242 kg with ~3.0 kN on each driven front wheel (src: 1.47 kN,
   so the front wheels spin, kappa ~1.8).
2. `0x5032d0` adds each child's own shape inertia; src sums point masses only, and mesh bodies
   have no mass properties (`compute_mass_props` TODO). Roll inertia is ~30 kg m^2; with the real
   spring parts of card 154 (k 59841, damping 3900) the explicit spring step goes unstable (car
   bounces off the soup in 20 frames). This card must land first.

## Exe functions
`0x4c7d20 Part__commitInstall(P)` physics tail (after the slot flag loop):
```
Pb = body(P)                                   // P's own compound body (P+0x1c ... +0xb4)
Tb = getInfo(target, 0x2f, type_slot(first committed slot)+0x60)  // body hosting the target slot
if (!(Tb+0x58 & 0x8000) && Tb.parent(+0x20) && Tb.offset_record(+0x1c)) Tb = Tb.parent
wake roots of Pb and Tb if asleep (0x507060)
0x501d00(Pb); 0x4f7b20(Pb, Tb); 0x5032d0(Tb)   // unlink, add as child, compound mass (recurses
                                               // into Tb's parent up to car_root)
if (Tb flags had 0x80 asleep) sleep root (0x506d90)
```
`0x4c1a10` pose of P (car space): `Ts = target runtime slot`, `Ms = my runtime slot`;
`R_P = R_T * Ts.rot * transpose(Ms.rot)`, `p_P = p_T + R_T*Ts.pos - R_P*Ms.pos`
(src `mat_mul`/`mat_vec`; world = R*local). The chassis (car resource) has `p = 0, R = I`.
INVENTED: whether the subtraction uses R_P or R_T*Ts.rot is hidden in 0x567020; use R_P.

`0x5032d0 (body)` (notes/09 line ~383), per child c with invMass != 0, in the body frame:
```
M = sum m_c;  com = sum m_c * r_c / M                 // r_c = child offset (+0x1c record)
I = sum ( R_c * I_c * R_c^T  +  m_c*(|r_c-com|^2 E - (r_c-com)(r_c-com)^T) )  // 0x566330
inv_mass = 1/M; inv_inertia = inverse(I) (0x566830); drag -0.005M / -0.01M
if |com|^2 > 1e-5: subtract com from every child offset, move the body pose by R*com
```
`I_c` = child inertia `def+0x60` (= inverse of its inv inertia). Primitives: the existing
`compute_mass_props` (sphere/box/capsule). Mesh: `Poly__massProps 0x508d20` (0x508820 = the
standard Mirtich polyhedral volume integrals T0, T1[3], T2[3], TP[3] over the hull triangles, in
doubles):
```
V = T0; c = T1/V
Ixx = (T2y+T2z)/V - (cy^2+cz^2); Iyy = (T2x+T2z)/V - (cx^2+cz^2); Izz = (T2x+T2y)/V - (cx^2+cy^2)
Ixy = Iyx = -TPxy/V + cx*cy;  Iyz = Izy = -TPyz/V + cy*cz;  Izx = Ixz = -TPzx/V + cz*cx
inv_inertia = inverse(I) * (1/m)      // caller scales by invMass; shape volume = V
vertices -= c                         // the exe shifts the hull to its COM and returns c
```
src keeps meshes shared/const: do not shift vertices; return `c` and add it to the child offset
(`INVENTED:` where the exe applies the returned c is not traced).

## Files
- src/physics/world.cpp (+ physics.h): `SHAPE_MESH` branch of `compute_mass_props` = the
  0x508d20 block above (Mirtich 0x508820 as a static helper, doubles), plus an out-param or
  BodyDef field for `c`. Cite both addresses.
- src/game/car.cpp: one static `rebuild_compound(Runtime&, Runtime::Resource *car)` used by
  `create_car_bodies` (replaces today's point-mass loop and its shortcut comment) and by
  `part_attach` (call it after the slot flags when `g_cars` has the part's car and its
  `root_body` exists). Children: the car cfg `body` lines (mass, xform, shape: primitive from
  `tail`, or the `.scx` hull loaded with `mesh::load` + `build_collision_mesh`, cache per
  resource), each wheel's bone child (`bone_mass` sphere at `hub`) and the 0.01 kg sensor, and
  every installed part (walk `car->children` recursively, `PartData` with an installed slot):
  its cfg `body` lines at `pose(P) * line.xform`, pose from the 0x4c1a10 block. Sum at car_root
  with the 0x5032d0 formula, set root `inv_mass`, `inv_inertia`, drags, `car->com`, re-centre
  `sim->wheels[i].mount = hub - com`, and move `root->pos` by `R*(com_new - com_old)`.
  INVENTED: all parts join car_root (getInfo 0x2f slot->body mapping not traced; the root totals
  are the same because 0x5032d0 recurses). Keep spring k as is: `M+0x1f34` is fixed at createCar
  from part_comp (cfg body lines only, notes/09 line 364).
- src/tools/vmboot.cpp: see Done when. Update the existing "car_root mass adds the wheelbone and
  sensor children" check to the new total.

## Done when (`VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh`, real data)
a. car_root mass == sum of the car cfg body masses + per wheel (bone_mass + 0.01) + sum of the
   cfg body masses of every installed part (vmboot reads the parts' cfgs itself), within 0.01 kg;
   print it (SunStrip ~1242 kg) and the part count.
b. At rest (after the 300 settle frames) every wheel load is within 10% of the static share from
   the root COM and the hub z (front each = W*(zr-c)/(zr-zf)/2, rear = W*(c-zf)/(zr-zf)/2,
   W = M*9.81, c = com.z) and the load sum within 2% of W; print loads and shares.
c. Car still settles (|vel.y| < 0.1) and every older check stays green, 0 failed threads.
d. Print only: root inertia diagonal, Up-held speed and driven kappa (traction is checked in 154).

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; no tuning constants; do not touch
vehicle.cpp.

## Result
