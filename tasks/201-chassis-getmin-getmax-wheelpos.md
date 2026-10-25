---
id: 201
status: merged
branch: card/201
depends: [196]
---
# Chassis natives: `center` cfg keyword, getMin / getMax / getWheelPos

## Goal
Replace the explicit stubs `Chassis.getMin`, `Chassis.getMax`, `Chassis.getWheelPos` (natives.cpp
registry rows ~7815/7816/7871, `nullptr, true`) and parse the car type cfg keyword `center`
(M+0x1f1c..0x1f24). Caller: sl/Scripts/game/CarInfo.java:256-257 (dimensions = Max - Min).
Replaces draft 198 (deleted). RE: notes/22 §3.

Key fact: the exe walks a compound chain from `pb` to car+0x13ec, but `pb` IS car+0x13ec
(part_comp, the car's own phys body), so the walk never runs: `p = M.center` and the bounds body is
part_comp, whose Poly has **0 vertices** (mesh-less type 6: Shape ctor 0x4ba8c0 zeroes nverts,
Phys__shapeInit 0x507ce0 -> Poly__buildBoxFaces 0x50e5e0 builds features only). So no chain or
vertex loop is needed in src: src part_comp = `CarData::chassis_body`.

## Exe evidence (cleaned)
### Prologue (all three)
```
P = typed(native ref, 0xa0000000) (0x4ecc70); car = P+0x4c; M = typed(P+0x44, 0x20000000)+0xc
pb = GameRef__getPhysBody 0x4eabc0(ECX = car+0x2c) -> +0xb4      // = car+0x13ec (part_comp)
any step fails -> return new Vector3(0, 0, 0)
```
### chain (inlined in each): `p = M.center; b = pb; while (b+0x58 & 4) && b != car+0x13ec: {...}`
-> exits at once: p = M.center, b = part_comp.
### 0x4b3220 getMin
```
lo = (0,0,0); for v in b.shape.poly verts (count shape+0x20, verts +0x24): if v.k <= lo.k: lo.k = v.k
return Vector3(p.x + lo.x + 0.1, p.y + lo.y + 0.1, p.z + lo.z + 0.1)     // 0x5935d4 = 0.1
```
### 0x4b2dc0 getMax: same with max, `p + hi - 0.1`.
With 0 vertices: getMin = center + 0.1, getMax = center - 0.1 (yes, min > max; CarInfo prints -200 mm).
### 0x4b3e40 getWheelPos(int i)
```
W = *(car+0x1414) + i*0x2e4                 // wheel record, no bounds check on i
return Vector3(p.x + W+0x78, p.y + W+0x7c, p.z + W+0x80)   // hub = WheelData::hub
```
### 0x48de60 type cfg `center` (gated M+0x120 == 0, taken as 0 like the other keywords)
```
x = y = z = 0; Str__scan(values, "%f %f %f", &x, &y, &z); M+0x1f1c = (x, y, z)   // missing -> 0
```
No shipped cfg uses `center`.

## Port
- src/game/car.h: `float center[3] = {};  // M+0x1f1c, cfg `center` (0x48de60)` in CarData.
- src/game/car.cpp type-cfg keyword loop (next to `steerhelp`, ~line 315): `if (ln.key == "center")
  { for k<3: c.center[k] = k < values.size() ? num(values[k]) : 0.0f; continue; }`.
- src/script/natives.cpp, near ch_get_angvel: `ch_get_min`, `ch_get_max`, `ch_get_wheel_pos`
  (pattern of ch_get_angvel / ch_get_wheel: `car_of(a)`, `new_point(vm, "java.lang.Vector3", v, false, err)`).
  Gate: `c && c->chassis_body` else (0,0,0). getMin: `v[k] = c->center[k] + 0.0f + 0.1f` (lo = 0,
  comment `// part_comp's Poly has no vertices (0x50e5e0), lo stays 0`); getMax: `+ 0.0f - 0.1f`.
  getWheelPos: `get_i(a[1])`; `v[k] = c->center[k] + c->wheels[i].hub[k]`;
  `// INVENTED: exe has no bounds check; i out of range -> (0,0,0)`.
- Registry rows: set the three functions, last field `false`.
- Leave the setMatrix `center` shortcut comment (car.cpp ~1469) alone (separate behaviour).

## Files
- edit: src/game/car.h, src/game/car.cpp, src/script/natives.cpp

## Done when
- `// 0x4b3220` / `// 0x4b2dc0` / `// 0x4b3e40` / `// 0x48de60` comments
- `make -C src clean check` green; natives-vs-exe table check still passes (3 fewer stubs)
- no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
