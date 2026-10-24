---
id: 153
status: todo
branch: card/153
depends: [152]
owner: Claude (RE/debug; not a Flash card until the root cause is known)
---
# Drive traction: driven wheels spin, front loads half the rear

## Why (scratch debug build, 2026-10-24, vmboot freeride Axis)
With Up held in 1st (ratio 3.23, rearend 5.5, r 0.315, D18V 159 Nm, idle 94.2 rad/s):
engine 740 rad/s (limiter), shaft 41.6 rad/s, driven wheels 0/1 spin 39.6 rad/s (12.5 m/s
surface) while the car does 2.2 m/s; kappa 1.8. Near rest (A held, speed 0.36 m/s) the wheel
loads are 1223/1239 N (driven pair) and 2634/2625 N (undriven): sum 7.7 kN vs 1200.042 kg*9.81 =
11.8 kN. Card 145 springs are k = 9.81*M/wheels/travel (equal static share 2943 N per wheel), so
the uneven split and the missing 4 kN point at the COM / pose / contact (body resting on the
soup?), not at the tyre model. 0x497930 refuses to upshift while |mean driven kappa| >= 0.2
(0x4b19a0), so the auto gearbox of card 152 cannot shift until this is fixed.

## To trace (<= 3 exe functions)
- Static wheel load: Vehicle__update spring force path (notes/09 §5, S+0xf4) vs the compound COM
  0x5032d0 (card 146, `CarData::com`) and the wheel hub positions: what split does the exe give
  for the Axis cfg (compute it from the shipped cfg/compound COM, not by hand)?
- Is the chassis box touching the ground soup (contact count on car_root at rest)?

## Done when (vmboot hard checks, real data)
- At rest each wheel's load is within 10% of the exe-derived static share from COM + hub positions
  and the sum is within 5% of M*9.81 (M = car_root compound mass).
- Card 152 phase, Up held 900 frames: gear reaches >= 2 via 0x4b06b0 (no gear key) and speed >
  v1 = 0.95*min(max_rpm, rpm_limit)*0.10471976/(ratio[1]*rearend_ratio)*radius of a driven wheel
  (the 1st-gear top speed at the 0x4b06b0 threshold; ~11.8 m/s for the Axis, computed in vmboot).

## Rules
Stop and report after 2 failed vmboot runs. No tuning constants; cite exe addresses.

## Result
