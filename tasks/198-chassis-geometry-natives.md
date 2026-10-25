---
id: 198
status: draft
branch:
depends: [196]
---
# Chassis natives: getCM / getMin / getMax / getWheelPos (CarInfo.java:255-266)

## Goal
Replace the four explicit stubs. Callers: sl/Scripts/game/CarInfo.java:255-257 (car info screen),
getWheelPos is commented out there. DRAFT: the exe walks the chassis part body's compound chain
(child -> parent local transforms); src keeps child shapes on the root (physics.h:122-131,
INVENTED storage), so the chain mapping below needs an analyst pass before Flash.
Also needs the type cfg keyword `center` (handler 0x48de60 -> M+0x1f1c..0x1f24, notes/09:516;
no shipped cfg uses it -> 0).

## Exe evidence (common prologue: chassis instance -> car T = inst+0x4c, M = type record,
## pb = GameRef__getPhysBody 0x4eabc0(chassis)+0xb4 = the chassis part body; no car -> (0,0,0))
### chain(p): local point p of pb -> car root frame
```
b = pb
while (b+0x58 & 4) and b != car+0x13ec:     // compound child, stop at the car's part_comp body
  X = b+0x1c (child transform: t = X+4..0xc, rows r0 = X+0x20.., r1 = X+0x30.., r2 = X+0x40..)
  p = (t.x + X[0x20]*p.x + X[0x24]*p.y + X[0x28]*p.z,
       t.y + X[0x30]*p.x + X[0x34]*p.y + X[0x38]*p.z,
       t.z + X[0x40]*p.x + X[0x44]*p.y + X[0x48]*p.z)
  b = b+0x20 (parent)
return (p, b)
```
### 0x4b3220 getMin
```
(p, b) = chain(M.center); geom = (b+0x5c)+0x90; verts = geom+0x24 (xyz floats), n = geom+0x20
lo = (0,0,0); for each vert: lo = componentwise min(lo, vert)      // starts at 0, not +inf
return (p.x + lo.x + 0.1, p.y + lo.y + 0.1, p.z + lo.z + 0.1)       // +0.1 on the MIN (exe)
```
### 0x4b2dc0 getMax: same with max and hi start 0, returns `p + hi - 0.1`.
### 0x4b3e40 getWheelPos(i): `(p, _) = chain(M.center); return p + W_i.hub (car+0x1414 + i*0x2e4 + 0x78..0x80)`
### 0x4b2680 getCM
```
q = -M.center; q = 0x5060d0(pb, q)           // pb local -> world
root = car+0x13ec root state S (pos +0x64, rows +0xa4/+0xb4/+0xc4)
d = q - S.pos
l = (S.r0 . d, S.r1 . d, S.r2 . d) using columns: x = a4*dx + b4*dy + c4*dz (R^T d)
g = (pb+0x5c)+0x90; return (-l.x - g+0x40, -l.y - g+0x44, -l.z - g+0x48)
```

## Open (analyst)
- src equivalent of `b+0x1c` child transforms and of `geom+0x20/+0x24` vertex list for the
  chassis part body; which src body is `pb` (car->chassis_body vs root_body).
- 0x5060d0 exact transform (rotation + translation of pb's active state).

## Files (when todo)
- edit: src/game/car.cpp (center keyword), src/script/natives.cpp (4 natives)
