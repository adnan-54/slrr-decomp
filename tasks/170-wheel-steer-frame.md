---
id: 170
status: todo
branch: card/170
depends: [168]
---
# Wheel steer frame (0x4aa830 / W+0xe4): steered wheels turn the car

## Why (RE 2026-10-24, notes/09 "Wheel steer frame")
Card 169 (branch card/169) measured mean yaw **0.001 rad/s** at `steer_input` 1.0, 8.5 m/s: `vehicle::Car`
writes `w.steer_angle` (src/vehicle/vehicle.cpp:140) but nothing reads it; the tyre pass, the slip velocity and
the spring Gram-Schmidt all use the chassis rotation. In the exe all three use the wheel's sensor-body matrix,
whose local offset is `W+0x110 = W+0x90 · Ry(W+0xe4)`, written by 0x4aa830.
Exe provides: the steer angle (with Ackermann) and the wheel frame. Script provides: `max_steer`
(Chassis.setMaxSteer) and `ackermann` (setAckermann), both already in `CarData`.

## Exe code (rea/Ghidra 12.1.4, read 2026-10-24)
`0x4aa830(car)` per wheel W with S (`W+0x30`):
```
d0 = e+0xa20 * car+0x1ddc                          // max_steer * steer
if (|d0| <= 0.001)          W+0xe4 = 0
else if (e+0xa30 == 0)      W+0xe4 = W+0xc8 * d0    // ackermann 0
else { R = e+0xa30 / (sin(d0)/cos(d0))
       if (R > 0) W+0xe4 =  atan2(e+0xa30,  W+0x78 + R) * W+0xc8     // W+0x78 = hub x (Wheel::mount.x)
       else       W+0xe4 = -atan2(e+0xa30, -R - W+0x78) * W+0xc8 }
0x5666d0(Euler{W+0xe4, 0, 0}) -> Ry rows (c,0,s),(0,1,0),(-s,0,c)
W+0x110 = W+0x90 * Ry          // 0x566a70; W+0x90 = WheelData::rot (setYpr / template Ypr)
0x4f4380(S, 0, W+0x110)        // sensor body local matrix = W+0x110
```
Convention (0x49c8e0 W+0x140 transform `local_j = sum_r M[r][j] v_r`): columns are the local axes, so
`Ry` column 2 `(sin d, 0, cos d)` is the wheel forward; `d > 0` turns +z toward +x.
`0x49c8e0` consumers (all `R = matrix of S+0x94` = `R_car * W+0x110`):
- §2 slip velocity: `W+0x140 = R^T (groundVel - pointVel(B, S+0xd8))`, `W+0x148 += W+0xc0 * r`
- §6 tyre pass: `v = R^T (...)`, `n_body = R^T S+0xc4`, force basis `a/b` mapped back with `R`
- spring 0x4f2d70 step 10: Gram-Schmidt forward `R (0,0,1)` (up `R (0,1,0)` is the same as the chassis').

## Files
- `src/vehicle/vehicle.h`, `src/vehicle/vehicle.cpp` (Wheel: `base_rot` = W+0x90; Car: `ackermann` = e+0xa30)
- `src/game/car.cpp` (bridge: `sim.ackermann = c->ackermann`, `sw.base_rot = w.rot` next to `steer_factor`)
- `src/tools/vmboot.cpp` (check below)

## What to do
1. Replace vehicle.cpp:140 with the 0x4aa830 formula above (per tick is fine: same result as on-change).
2. Per wheel `Mat34 rw = r * base_rot * Ry(steer_angle)` using the existing `euler_to_mat`/Mat34 helpers in the
   exe's column-axis convention (check against how `mat_tvec(r, ..)` already mirrors the W+0x140 transform).
3. Use `rw` instead of `r` in: the §2 slip velocity (vehicle.cpp ~345), `update_tyre_forces` (`v`, `n_body`,
   `f_world`; the patch-offset `point` stays chassis-local as now), and the spring Gram-Schmidt `fwd`
   (vehicle.cpp ~231). Nothing else.

## vmboot check (shipped data only)
In the card-148 "Up + Left" block (steer_input 1, ~8 m/s): over its last 30 frames record mean `omega_y`
(`root->angular_velocity().y`) and mean forward speed `vf` (velocity · car forward). From the car's own data:
wheelbase `Lwb` = |mean front hub z - mean rear hub z| (front = wheels with `steer_factor != 0`),
`d_f` = mean `steer_angle` of the front wheels, `r_kin = vf * tan(d_f) / Lwb` (kinematic bicycle model).
**Hard check**: `sign(omega_y) == sign(r_kin)` and `0.3 <= omega_y / r_kin <= 1.1`
(`INVENTED:` bounds; below 1 = tyre slip/understeer, never faster than kinematic). Print Lwb, d_f, vf, omega_y, r_kin.

## Done when
`make -C src -j4 check` green; `ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck` PASSED with the
hard check; all existing checks still pass (list any that moved, with numbers). Result lists the printed numbers.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No tuning of tyre/steer
constants; no unit tests; gap-filling marked `INVENTED:`.

## Not this card
Steer help in 0x498300 (flag 0x200 / e+0xa34, notes/09:418), flag 0x2000 assist, render wheel pose 0x4a7fe0,
steering-wheel bone, the card 169 spline driver.
