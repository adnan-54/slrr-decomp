---
id: 152
status: todo
branch: card/152
depends: [151]
---
# Auto gearbox + auto clutch (transmission flags 0x10 / 0x40)

## Why (RE 2026-10-24, Ghidra + a scratch debug build)
Config.player_transmission = 1 gives the player's car flags 0x50 (0x4a37f0): 0x10 = auto gearbox
0x497930, 0x40 = auto clutch 0x4977a0. `game::control_cars` (src/game/car.cpp, the port of
`Vehicle__updateControls` 0x498300) already has the input read, the manual shift block, the
0x4b0190 timers (`shift_timers`) and 0x4b04a0; it skips both auto paths (`shortcut:` comment
"auto gearbox/semi-auto/auto clutch ... later cards"). So today the car only leaves neutral
when A is pressed, never shifts by itself, and the clutch stays at the key value (0 = closed).

Measured in a scratch build (vmboot `control Up held`): the printed "rpm 740" is `rpm_raw` in
**rad/s** (= 7066 rpm, on the 6750 limiter), not a bog. Driven wheels spin at 39.6 rad/s
(12.5 m/s surface) while the car does 2.2 m/s: kappa ~1.8 (wheelspin). Front/driven wheel
loads 1236 N vs 2601 N on the undriven pair. That traction problem is card 153; this card only
ports the two transmission helpers. Note: 0x497930 never upshifts while |avg kappa| >= 0.2,
so do NOT expect upshifts in vmboot until 153 is fixed.

Names below: `e` = engine block (CarData engine fields), `car` = CarData.
`now` = world time (`process_world().time`, PTR_005dadf0). Field map:
`+0x1de4` throttle, `+0x1de8` brake_input, `+0x1df0` clutch_input, `+0x1e0c` gear_ratio,
`+0x1e5c` gear, `+0x1e68` shift_end, `+0x1e70/74` shift_t70/t74, `+0x1e78` shaft_speed,
`+0x1e80` rpm_smoothed (rad/s), `+0x1fa8` prev_clutch, `+0xb4` tear (PartData, as in
control_cars), `+0x70` flags; `e+0x14` rpm_max_torque, `e+0x18` max_rpm, `e+0x1c` rpm_limit,
`e+0x20` gears, `e+0x24+4i` ratio[i] (so `e+0x28` = ratio[1], `e+0x28+4g` = ratio[g+1],
`e+0x20+4g` = ratio[g-1]), `e+0x44` rearend_ratio, `e+0xf0` idle_omega.
NEW CarData fields: `int32_t brake_latch` (+0x1e64, 0x497930), `float prev_rpm` (+0x1fac,
0x4977a0). `curve(rpm)` = `FUN_004a2dd0` = exactly `sample_curve` (vehicle.cpp:17) ->
use `c->sim->engine.torque_at(rpm)` (the bridge keeps it equal to `torque_rpm/torque_nm`).

## Exe functions (5)
`0x497930 (e, car)` auto gearbox (dt unused):
```
if (tear <= 0 || !(flags & 0x4000) || (flags & 0x40004) || shift_end > 0) return
lo = idle*1.2; hi = idle*3.0; s = shaft_speed; thr = throttle; brk = brake; d = thr - brk
if (|s| <= 1) {                                       // standstill
  if (brake_latch == 0) {
    if (d > 0.1)  { if (!gears || gear != 0) return; goto ENGAGE }
    if (d < -0.1) { if (!gears) return; brake = thr; throttle = brk;      // swap
                    if (gear != 0 || rpm_smoothed <= hi) return
                    gear = 7; 0x4b0190(up=1); return }
    if (gear == 0 || rpm_smoothed >= lo) return
  } else { if (d <= -0.5) return; brake_latch = 0 }
  gear = 0; 0x4b0190(up=0); return
}
g = gear
if (g != 7 && g != 0 && d < 0) {                      // braking in a forward gear
  brake_latch = 1; if (g == 1) return                 // exe: (-1 < g && g < 2)
  r = gear_ratio*s*rearend*9.549296; T = r < rpm_limit ? curve(r) : 0
  0x4b05a0(r, T); return
}
if (d <= 0.1) {
  if (d < -0.1) { if (!gears) return; brake = thr; throttle = brk
                  if (g == 7 || shift_t70 > 0 || rpm_smoothed <= hi) return
                  gear = 7; 0x4b0190(1); return }
  if (shift_t74 > 0) return; if (g >= 0 && (g < 2 || g == 7)) return
  goto MOVING
}
if (g == 0) { if (!gears || shift_t70 > 0) return; goto ENGAGE }
if (g == 7) { if (shift_t74 <= 0) { gear = 0; 0x4b0190(1); return }
              brake = thr; throttle = brk; return }
if (|0x4b19a0()| >= 0.2) return                       // driven-wheel slip too high
MOVING:
w = gear_ratio*shaft_speed*rearend; r = w*9.549296; T = rpm_limit <= r ? 0 : curve(r)
if (r < 0) { if (shift_t74 > 0) return; gear = 0; shift_end = now+0.2; 0x4b0190(0); return }
0x4b06b0(r, T, w); 0x4b05a0(r, T); return
ENGAGE: if (rpm_smoothed > hi) { gear = 1; 0x4b0190(1) }
```
`0x4b06b0 (e, car, r, T, w)` upshift -> int:
```
if (shift_t70 > 0) return 0
G = gear_ratio
if (|G| <= 0.01 || |rpm_smoothed - w| <= rpm_smoothed*0.1) {
  g = gear; if (g < gears) {
    Gn = ratio[g+1]; rn = r/G*Gn; Tn = rn < rpm_limit ? curve(rn) : 0
    th = min(max_rpm*0.95, rpm_limit*0.95)
    if (th < r || G*T < Gn*Tn) { gear = g+1; shift_end = now+0.3; 0x4b0190(1); return 1 } } }
return 0
```
`0x4b05a0 (e, car, r, T)` downshift -> int:
```
if (shift_t74 > 0 || gear <= 1) return 0
G = gear_ratio; Gp = ratio[gear-1]; rp = r/G*Gp; Tp = rp < rpm_limit ? curve(rp) : 0
th = min(max_rpm*0.95, rpm_limit*0.95)
if (rp < th && G*T*1.15 < Gp*Tp) { gear--; shift_end = now+0.2; 0x4b0190(0); return 1 }
return 0
```
`0x4b19a0 (M, car)` average driven-wheel slip: over wheels with a suspension (`W+0x30`),
drive base `W+0xcc != 0` (src `Wheel::drive`) and load `S+0xf4 > 0` (src `Wheel::load`):
mean of `W+0x14c` (src `Wheel::kappa_prev`), 0 when none. shortcut: the render-object veto
(`W+0x1e0` / instance `+0x50`) is not ported (same as the drive-wheel update in vehicle.cpp).
`0x4977a0 (e, car, dt)` auto clutch:
```
if (tear <= 0 || !(flags & 0x4000)) return
G = gear_ratio; if (|G| < 0.01) return
rpm = rpm_smoothed; pred = (rpm - prev_rpm) + rpm; prev_rpm = rpm
launch = (|G/ratio[1]|*throttle*0.56 + 0.24000001)*(rpm_max_torque*0.10471976 - idle) + idle
ws = shaft_speed*G                                   // times rearend below
lim = min(max_rpm, rpm_limit)
if (ws*rearend <= lim*0.12566371) {                  // 1.2*lim in rad/s
  c = clutch_input
  if (rpm < ws*rearend || launch <= pred) goto APPLY
  if (idle < pred) { c = 1 - (pred - idle)/(launch - idle); if (c <= clutch_input) c = clutch_input; goto APPLY }
}
c = 1.0
APPLY: if (clutch_input <= c) {
  p = prev_clutch; k = min(dt*(c <= p ? 33.333336 : 200.0), 1); clutch_input = k*(c - p) + p }
prev_clutch = clutch_input
```

## Files (3 + check)
- src/game/car.h: add `brake_latch` (+0x1e64) and `prev_rpm` (+0x1fac) to CarData with offset
  comments.
- src/game/car.cpp: static `auto_gearbox` (0x497930), `upshift` (0x4b06b0), `downshift`
  (0x4b05a0), `drive_slip` (0x4b19a0), `auto_clutch` (0x4977a0); reuse `shift_timers` for
  0x4b0190. In `control_cars`, call them right before the `// 0x4b04a0 shift timers.` block in
  exe order: `if (flags & 0x10) auto_gearbox; if (flags & 0x40) auto_clutch(dt)`. Keep a
  `shortcut:` for 0x20 semi-auto 0x4ad480 (not traced) and shrink the existing shortcut comment.
  Cite each address next to its port. No other behaviour changes.
- src/tools/vmboot.cpp: (1) fix the `report` label: print `rpm_raw*9.549296` as rpm (it is rad/s).
  (2) The existing "A (axis 50) shifts neutral -> 1st" check must still pass; if the auto box
  changes the gear before it is read, read it right after `frames(10)` of A held (inside the
  0.3 s shift window) and say so in a comment. (3) New phase after `Up + Right` (keys released):
  `game::car_command(..., "reset")` (0x4a4067, INVENTED fixture like the dealer browse), then
  up to 300 frames with no keys until `|shaft_speed| <= 1` (fail the check if never); then hold Up
  (200) for 300 frames, sampling every frame (gear, rpm_smoothed, clutch_input, shaft_speed,
  speed). Hard checks:
  a. "auto gearbox engages 1st from standstill (0x497930)": gear goes 0 -> 1 with no gear key,
     and `rpm_smoothed` of the frame before the change is > `3*idle_omega` (print both).
  b. "auto clutch slips at launch (0x4977a0)": max `clutch_input` over the 30 frames after the
     engage is > 0 (key C is never pressed; the exe value comes from the launch formula; print the
     max and `launch` computed from car_payload fields with the formula above).
  c. print only (no check, card 153): max gear, final speed, mean driven kappa (0x4b19a0).

## Done when
`VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh` passes checks a and b, all older
checks stay green, 0 failed threads, `make -C src` builds clean without warnings in car.cpp.

## Rules
- Stop and report after 2 failed vmboot runs on the same check: write what you saw into Result
  and set status `blocked`.
- No unit tests or synthetic inputs; cite the exe address next to the ported code. Do not tune
  any constant; do not touch vehicle.cpp (traction is card 153).

## Result
