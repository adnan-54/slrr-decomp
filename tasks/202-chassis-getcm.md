---
id: 202
status: todo
branch:
depends: [201]
---
# Chassis native: getCM

## Goal
Replace the explicit stub `Chassis.getCM` (natives.cpp registry row ~7814, `nullptr, true`).
Caller: sl/Scripts/game/CarInfo.java:255 (value unused, "ToDo: display"). Uses `CarData::center`
from card 201. RE: notes/22 §3.

## Exe evidence (cleaned)
### 0x4b2680 getCM
```
prologue as card 201 (P, car, M, pb = part_comp = car+0x13ec); failure -> Vector3(0,0,0)
q = PhysBody__localToWorld 0x5060d0(pb, -M.center)
B = car+0x13ec                               // same body as pb, NOT walked to the root
d = q - B.pos
l.x = B.R[0][0]*d.x + B.R[1][0]*d.y + B.R[2][0]*d.z      // R^T d (rows +0xa4/+0xb4/+0xc4)
l.y = B.R[0][1]*d.x + B.R[1][1]*d.y + B.R[2][1]*d.z
l.z = B.R[0][2]*d.x + B.R[1][2]*d.y + B.R[2][2]*d.z
com = pb.shape+0x40..0x48                    // 0 for part_comp (Shape ctor 0x4ba8c0, mesh-less)
return Vector3(-l.x - com.x, -l.y - com.y, -l.z - com.z)
```
### 0x5060d0 localToWorld(body, p, out)   (active state)
```
if p == null: out = pos
else out = R*p + pos     // out.x = R[0][0]*p.x + R[0][1]*p.y + R[0][2]*p.z + pos.x, ...
```
Net result ~ M.center (exactly, up to float rounding), but port the op sequence.

## Port
- src/script/natives.cpp `ch_get_cm` next to the card-201 natives: gate `c && c->chassis_body`;
  `RigidBody *b = c->chassis_body; Mat34 R = b->rotation(); Vec3 m{-center...};`
  `Vec3 q = mat_vec(R, m) + b->pos; Vec3 d = q - b->pos;`
  R^T d with the explicit three sums above (use `mat_transpose` + `mat_vec` only if it keeps the same
  multiply-add order: x = r00*dx + r10*dy + r20*dz); out = `-l - 0` per component
  (`// part_comp shape com +0x40 is 0 (0x4ba8c0)`).
  `// INVENTED: src part_comp pose (child of car_root) may lag the exe's state; the result cancels it`.
- Registry row: set `ch_get_cm`, last field `false`.

## Files
- edit: src/script/natives.cpp

## Done when
- `// 0x4b2680` / `// 0x5060d0` comments
- `make -C src clean check` green; natives table check passes
- no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
