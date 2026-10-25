# 22 — Str__vscan integers, RK4 angular velocity, Chassis geometry natives

Traced 2026-10-25 (card-196 follow-ups + draft 198). Cards: 199 (integrator), 200 (scan ints), 201/202 (chassis natives).

## 1. Str__vscan 0x527140 integer conversions (`Str__scan` 0x569380 is the varargs wrapper)
Format loop: whitespace in the format skips whitespace in the input; a literal must match (else return count);
`%#` sets "return consumed length"; `%*` = no store; width digits (default 0x400), `.prec` (default 0x400),
`h/l/L` skipped. Per conversion, a failed reader returns the count so far (later fields untouched).
- `%d` (0x64) / `%u` (0x75, unsigned flag): skip ws; **if the input starts with `0x`/`0X` -> hex reader 0x527490**,
  else decimal reader 0x527420.
- `%x`: skip ws, always the hex reader 0x527490.
- `%c`: copies one byte (no ws skip), count++. `%s`: ws skip, copy until ws / NUL / width / next format char.
- `%f`: ws skip, 0x527530 (precision arg = `.prec`).
### 0x527490 hex reader
```
p = *cursor
if !(p[0]=='0' && (p[1]=='x'||p[1]=='X')): return 0     // prefix REQUIRED, bare "FF" fails
p += 2; if !isxdigit(*p): return 0                       // "0x" alone fails
v = 0 (int32); while isxdigit(*p): v = v*16 + digit(*p); p++   // wraps mod 2^32, no clamp
if out: *out = v; *cursor = p; return 1
```
### 0x527420 decimal reader (arg2 = unsigned flag)
```
p = *cursor; neg = (*p == '-'); if neg: p++              // no '+' accepted
if !('0'<=*p<='9'): return 0
v = 0 (int32); while '0'<=*p<='9': v = v*10 + (*p-'0'); p++     // wraps, no clamp; leading 0 is decimal
if out: *out = unsigned ? v : (neg ? -v : v)              // %u ignores the '-'
*cursor = p; return 1
```
Differences to `strtoul(s, 0, 0)` (src car.cpp fill_osd): octal for a leading `0`, `+` sign, `-0x10` (exe reads
`-0` then stops at `x`, value 0), overflow saturation, empty `0x` (strtoul -> 0 and continues; exe fails and stops
the scan). Every shipped `osd_*` value is `0x%08X` (295+180+134+70+... lines), so the corpus agrees; the port is for
exactness. Also src runtime.cpp `scan_hex` (type_ground params) accepts hex without the `0x` prefix; the exe fails.

## 2. RK4 and the stored angular velocity (0x506530 / 0x505c40 / 0x504ca0 / 0x504b30 / 0x505450)
State s (stride 0xd4): `s[0x1c..0x1e]` (+0x70) = **stored ω**, `s[0x1f..0x21]` L, `s[0x22..0x24]` torque,
`s[0x25..0x28]` quaternion, `s[0x29..0x33]` world inverse inertia (3x4).
- Derivative 0x505c40(y, h, k) writes only: `k[0..2] = y.vel*h`, `k[3..5] = y.force(s[10..12]) * invMass(def+0x2c) * h`,
  `k.q = quat_mul((y.ω, 0), y.q) * (h*0.5)` (0x568190 builds (ω,0) in 0x605d10, 0x567b40 ECX=k+0x94, 0x568170 scale;
  0x593590 = 0.5), `k[0x1f..0x21] = y.torque * h`. **k.ω (k[0x1c..0x1e]) is never written** (static k buffers
  0x6059c0/0x605a94/0x605b68/0x605c3c, no xref to their +0x70) -> stays 0.
- Stage states 0x504ca0 (y + k*w) / 0x504b30 (y + k) copy ω as `y.ω + 0` -> **all four stages use the start
  state's stored ω** for the quaternion derivative; the stage L is integrated but never turned into ω.
- End of 0x506530 (non-kinematic): sum with 1/6,1/3,1/3,1/6; `pos += s[6..8]`; normalise q (0x5680d0), q -> matrix
  (0x5665c0); **angular damping first** (`def+0x9c < 0`: `L *= 1 + invMass*coef*h*100`, no clamp at 0); then
  `Iw = R * Ibody_inv(def+0x30) * R^T` into s[0x29..] (0x567380/0x566c10 x2); `ω = Iw*L`;
  `e = |ω|^2 * 0.0025; if e > 1: L *= 1/e; ω = Iw*L` (|ω|_new = 400/|ω|, NOT 20); store ω in s[0x1c];
  then `|v|^2 > 62500 -> v *= 250/|v|`.
- Impulse 0x505450(J, p) on the active state: `v += J*invMass`; if p: `L += (R p) x J`, `ω = s[0x29..]*L` (stored Iw).
- Chassis.getAngvel 0x4b24f0: car+0x13ec walked up `+0x20` while `+0x58 & 4` (car_root), returns active s[0x1c..0x1e];
  instance lookup 0x4ecc70 failing -> returns **null**; no car -> Vector3 of uninitialised stack (src (0,0,0) is fine).
So stored ω == `Iw(current R) * L` == src `RigidBody::angular_velocity()` at every read point, but src
`integrate_body` (world.cpp:371) diverges: per-stage ω from stage L/q (evaluate), clamp to 20 instead of 400/|ω|,
clamp before damping, damping factor clamped at 0 (INVENTED). Card 199.

## 3. Chassis geometry natives (CarInfo.java:255-257; getWheelPos only in a comment)
Common prologue: native ref -> 0x4ecc70 typed P (0xa0000000), car = P+0x4c, M = (P+0x44 typed 0x20000000)+0xc
(type record), `pb = GameRef__getPhysBody 0x4eabc0(ECX = car+0x2c)->+0xb4` = the car's own phys body = **part_comp
(car+0x13ec)**, created by Part__initInstance 0x4c1f90 as `createBox(1.0, 6, 0,0,0, 0)` (mesh-less type 6).
Any failure -> Vector3 (0,0,0) (locals pre-zeroed).
- Chain walk (0x4b3220/0x4b2dc0/0x4b3e40): `p = M.center (M+0x1f1c..0x1f24); b = pb; while (b+0x58 & 4) &&
  b != car+0x13ec: p = X.t + X.R p (X = b+0x1c), b = b+0x20`. Since pb == car+0x13ec the loop exits at once:
  **p = M.center, b = part_comp**. No child chain to port.
- part_comp's Poly: Shape ctor 0x4ba8c0 zeroes nverts (+0x20) and com (+0x40..0x48); Phys__shapeInit 0x507ce0
  type 6 with nverts < 1 -> Poly__buildBoxFaces 0x50e5e0 (feature table only, 26 features, no vertices) -> 0 vertices.
- getMin 0x4b3220: `lo = 0; for v in b.poly verts: lo = min(lo, v) (comiss: v <= lo takes v)`; return `p + lo + 0.1`
  (0x5935d4 = 0.1). For part_comp: **center + 0.1**.
- getMax 0x4b2dc0: same with max, `p + hi - 0.1` -> **center - 0.1** (min > max: CarInfo prints -200 mm).
- getWheelPos(i) 0x4b3e40: `p + *(car+0x1414)[i*0x2e4 + 0x78..0x80]` (wheel hub W+0x78), no bounds check on i.
- getCM 0x4b2680: `q = PhysBody__localToWorld 0x5060d0(pb, -center)` (= R_pb*(-c) + pos_pb, active state rows
  +0xa4/+0xb4/+0xc4, pos +0x64); `d = q - pos(car+0x13ec)` (same body, NOT walked to root);
  `out = -(R^T d) - pb.shape.com(+0x40..0x48)` with `R^T d = (a4*dx + b4*dy + c4*dz, a8.., ac..)` -> ≈ center.
- `center` cfg keyword 0x48de60 (gated M+0x120 == 0): `Str__scan(values, "%f %f %f")` into zero-initialised
  locals, then all three stored (missing values -> 0, not kept). No shipped cfg uses it.

## 4. Correction (2026-10-25): pb of the geometry natives is the first `body` child, not part_comp
§3's premise "pb == car+0x13ec" is wrong. Asm: 0x4b3336/0x4b2ed6/0x4b3f61 `LEA ECX,[car+0x2c]; CALL 0x4eabc0`
(getCM: `ADD ECX,0x2c` at 0x4b27a2). Two different ref nodes on P (16 bytes {prev,next,id,RI}):
- `P+0x1c` = "part_comp" resource (0x4c1f90 at 0x4c21e8 `0x4e8a50(ECX=P+0x1c, ..., "part_comp")`);
  createCar 0x499362 `car+0x13ec = getPhysBody(P+0x1c)->+0xb4` = part_comp.
- `P+0x2c` = the "bbone" child: in the 0x4c1f90 body-record loop, for record index `== M+0xff0`
  (first `body` line) `0x4c23e3 bind(P+0x2c, child+0x54)`; only if still unbound (`P+0x34 == 0`)
  at the end `0x4c261e bind(P+0x2c, P+0x28)` (= part_comp). Cars with body lines -> first body child.
Child link record `X = child+0x1c` (0x50 bytes, 0x4f7990 at attach): `X+4..0xc t = R_pc^T (pos_child -
pos_pc)`, `X+0x10` relative quat, `X+0x20/0x30/0x40` rows of `R_pc^T R_child` (0x5665c0). Child world
pose from 0x4c1f90: `pos = P.pos + R_P (rec.pos + rec.R * shape.com(+0x40))`, `R = R_P rec.R`, so
`X.t = rec.pos + rec.R*com`, `X.R = rec.R` (exact up to float rounding).
Effect: getMin/getMax/getWheelPos walk one step: `p = X.t + X.R*center`, then stop at part_comp (its
Poly has 0 vertices, so lo/hi stay 0). getCM: `pb = bbone`: `out = -(R_pc^T (R_bb(-c) + pos_bb - pos_pc))
- com_bb = X.R*c - X.t - com_bb`. Shipped data: the first body line is a 0.001 kg sphere/box at the
origin with rot 0 on all but 5 car cfgs, so results only change for Peterbild/_2/_3 (sphere at y 0.9),
Focer_WRC (`_phys_chassis_water.scx`) and Police_V3 (`_phys_chassis_cockpit.scx`, com != 0). Card 205.
