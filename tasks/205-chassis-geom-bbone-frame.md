---
id: 205
status: implemented
branch: card/205
depends: []
---
# Chassis getMin/getMax/getWheelPos/getCM: use the first `body` child frame (fix cards 201/202)

## Goal
Cards 201/202 assumed the body these natives read (`getPhysBody(car+0x2c)->+0xb4`) is part_comp
(car+0x13ec). It is not: `car+0x2c` is bound to the FIRST cfg `body` line's child body ("bbone").
The natives walk one link (bbone -> part_comp) and so transform `center` by that child's frame
relative to part_comp. Port that frame. Results change only for car cfgs whose first body line is
not an origin primitive (Peterbild/_2/_3 sphere at y 0.9, Focer_WRC and Police_V3 .scx hulls);
all others stay identical (X = identity). RE: notes/22 §4 (corrects §3).

## Exe evidence (cleaned)
### Which body (asm)
`0x4b3336 / 0x4b2ed6 / 0x4b3f61: LEA ECX,[car+0x2c]; CALL 0x4eabc0` (getMin/getMax/getWheelPos),
getCM `ADD ECX,0x2c` 0x4b27a2. 0x4c1f90 Part__initInstance: part_comp is bound at P+0x1c
(0x4c21e8); in the body-record loop, record index `== M+0xff0` (first `body` line) does
`bind(P+0x2c, child.resource)` (0x4c23e3); only if still unbound, `bind(P+0x2c, part_comp)` (0x4c261e).
### Child link X = child+0x1c (0x4f7990 at attach)
```
X.t   = R_pc^T (pos_child - pos_pc)          // +4..+0xc
X.R   = R_pc^T R_child (rows +0x20/+0x30/+0x40)
child pos  = P.pos + R_P*(rec.pos + rec.R*com)   // 0x4c1f90, com = child shape +0x40
child rot  = R_P*rec.R ; part_comp at (P.pos, R_P)
=> X.t = rec.pos + rec.R*com, X.R = rec.R
```
### 0x4b3220 getMin (0x4b2dc0 getMax identical with max and -0.1; 0x4b3e40 getWheelPos uses `+ hub`)
```
p = M.center (car->center)
b = bbone
while ((b+0x58 & 4) && b != car+0x13ec) { p = X.t + X.R*p; b = parent }   // one step: b = part_comp
lo = 0 over b's Poly vertices (part_comp: 0 vertices)
return p + lo + 0.1      // getMax: p + hi - 0.1 ; getWheelPos(i): p + wheel[i].hub
```
### 0x4b2680 getCM
```
q = localToWorld(bbone, -center); d = q - pos(part_comp)
out = -(R_pc^T d) - bbone.shape.com    ==  X.R*center - X.t - com
```

## Files
- edit: src/game/car.h: add to CarData next to `center` (line ~255):
  `Mat34 bbone_rot = <identity>; float bbone_pos[3] = {}; float bbone_com[3] = {};`
  (comment: `// 0x4c1f90 P+0x2c: first body line child frame X (car+0x2c), notes/22 §4`).
  Use whatever identity initialiser car.cpp's `identity_rot()` uses (m[0]=m[5]=m[10]=1).
- edit: src/game/car.cpp `create_car_bodies` (~1355, right after `cfg.bodies.empty()` gate and
  `car_data`): from `const CfgBody &b = cfg.bodies[0]` compute, same as `add_body_lines` /
  `add_compound_child` (same file, ~760-820):
  `float ang[3] = {b.xform.rx, b.xform.ry, b.xform.rz}; euler_to_mat(&car->bbone_rot, ang);`
  `Shape sh; body_line_shape(b, &sh); BodyDef def; def.shape = sh; Vec3 com{0,0,0};`
  `compute_mass_props(&def, b.mass > 0 ? b.mass : 1.0f, &com);` (INVENTED: the mass only scales
  inertia; com is mass-independent) ; `bbone_com = com`;
  `bbone_pos = (b.xform.px,py,pz) + mat_vec(bbone_rot, com)`.
- edit: src/script/natives.cpp `ch_get_min`/`ch_get_max`/`ch_get_wheel_pos`/`ch_get_cm` (~3664-3740):
  `Vec3 p = bbone_pos + mat_vec(bbone_rot, center)`; getMin `p + 0.1`, getMax `p - 0.1`,
  getWheelPos `p + hub`; getCM `mat_vec(bbone_rot, center) - bbone_pos - bbone_com`
  (drop the live-state R^T computation: rigid compound, X is constant; say so in an `INVENTED:`
  comment: exe uses live child/part_comp states, equal up to float rounding). Update the
  comments that say "part_comp bounds" to "bbone frame (notes/22 §4)".

## Done when
- each changed function keeps its `// 0xADDR` comment; untraced logic `INVENTED:`
- `make -C src clean check` green (vmboot CarInfo numbers unchanged for the vmboot car: its first
  body line is an origin sphere); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
