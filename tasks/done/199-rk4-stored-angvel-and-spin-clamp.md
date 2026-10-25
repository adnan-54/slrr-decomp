---
id: 199
status: merged
branch: card/199
depends: []
---
# physics: RK4 uses the step-start angular velocity; exe spin clamp and damping order

## Goal
Make `integrate_body` (src/physics/world.cpp:371) match the exe integrator 0x506530 in three points
found while checking Chassis.getAngvel (notes/22 §2):
1. All four RK4 stages use the **step-start** angular velocity for the quaternion derivative (the exe
   never writes k.ω, so stage states keep y.ω). src `evaluate` recomputes ω per stage.
2. The spin clamp scales L by `1/e` with `e = |ω|^2*0.0025` (|ω| becomes 400/|ω|), not to |ω| = 20.
3. Angular damping runs **before** the clamp, and its factor is not clamped at 0.
`RigidBody::angular_velocity()` itself is already right (stored ω == Iw(R)*L at every read point), so
Chassis.getAngvel (natives.cpp ch_get_angvel) needs no change.

## Exe evidence (cleaned)
### 0x505c40 derivative(y, h, k)   (ECX = body; def = body+0x5c)
```
k.pos = y.vel * h
k.vel = y.force * def.inv_mass * h            // y.force = s[10..12], def+0x2c
k.q   = quat_mul((y.omega, 0), y.q) * (h*0.5) // 0x568190 (ω,0), 0x567b40, 0x568170 (0x593590 = 0.5)
k.L   = y.torque * h                          // s[0x22..0x24]
// k.omega (s[0x1c..0x1e]) is NEVER written: static k buffers, no xref -> 0
```
### 0x504ca0 stage(y, k, w, out) / 0x504b30 (w = 1)
```
out.{pos,vel,force} = y + k*w; out.q = y.q + k.q*w; out.omega = y.omega + k.omega*w (= y.omega);
out.L = y.L + k.L*w; out.torque = y.torque + k.torque*w
```
### 0x506530 tail (non-kinematic path), after the 1/6,1/3,1/3,1/6 sum
```
pos += s[6..8]                               // pos_correction (src already does this)
normalise q (0x5680d0); R = matrix(q) (0x5665c0)
if def.angular_drag(+0x9c) < 0:
    f = def.inv_mass(+0x2c) * def.angular_drag * h * 100 + 1      // no clamp at 0
    L *= f
Iw = R * def.inv_inertia * R^T              // s[0x29..0x33]
w = Iw * L
e = (w.x^2 + w.y^2 + w.z^2) * 0.0025
if 1 < e:
    L *= 1/e                                 // NOT 20/|w|
    w = Iw * L
store w in s[0x1c..0x1e]
if |v|^2 > 62500: v *= 250/sqrt(|v|^2)
```

## Port (src/physics/world.cpp)
- `evaluate(b, s, force, torque)` gets an extra `Vec3 omega` parameter and uses it instead of
  `mat_vec(iw, s.momentum)`; drop its now-unused matrix code.
- In `integrate_body`: `Vec3 omega0 = mat_vec(world inverse inertia of start.rot, start.momentum)`
  (build it like `RigidBody::world_inv_inertia` but from `start.rot`; comment
  `// 0x505c40: k.omega is never written, every stage uses the step-start omega`), pass it to all
  four `evaluate` calls.
- After the RK4 sum and `quat_normalize(&b.rot)`: damping block first (remove `if (factor < 0.0f)
  factor = 0.0f;`), then the clamp:
  `Vec3 w = b.angular_velocity(); float e = vec_dot(w, w) * 0.0025f;`
  `if (1.0f < e) b.angular_momentum = b.angular_momentum * (1.0f / e);`
  then the existing |v| clamp. Update the comment `|omega| <= 20` to `0x506530: L *= 1/(|w|^2*0.0025)`.
- Keep the existing "mass is read as inverse mass" comment (exe reads def+0x2c = inv mass, confirmed).

## Files
- edit: src/physics/world.cpp (evaluate, integrate_body)

## Done when
- `// 0x506530` / `// 0x505c40` comments on the changed lines
- `make -C src clean check` green (vmboot race numbers may change; that is expected, report them)
- no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
