---
id: 149
status: merged
branch: card/149
depends: [147]
---
# CarData -> vehicle::Car bridge in drive_cars

## Why
The exe keeps one car struct; src splits it into CarData (natives, cfg, forceUpdate results) and
vehicle::Car (simulation). `vehicle::Car` never sees CarData's inputs, engine block or ratios, so
throttle could never move it. INVENTED glue (no exe function): copy across once per sub-step.
Claude ran exactly this hunk on top of the card-147 changes: it compiles and card-146/147 checks
still pass (car rests with the engine flag on, gear 0).

## Change (src/game/car.cpp `drive_cars` only; do not change src/vehicle)
Take the `drive_cars` hunk (`@@ -658`) of
`/tmp/claude-0/-root-slrr-re/a4e760fe-f689-48e3-a3bd-ff9290332a02/scratchpad/card147-partial.patch`
verbatim if the file exists; otherwise write:
```
Car &sim = *c->sim;
// INVENTED glue: one exe struct, two src structs (notes/09 "Control path").
sim.engine.running = (c->flags & 0x4000u) != 0;
sim.throttle = c->throttle; sim.brake = c->brake_input; sim.handbrake = c->handbrake_input;
sim.steer = c->steer_input; sim.clutch = c->clutch_input; sim.nitro = c->nitro_input;
sim.gear = c->gear; sim.max_steer = c->max_steer; sim.final_drive = c->rearend_ratio;
for i < kMaxGears: sim.ratios[i] = c->ratio[i];
Engine &e = sim.engine;
e.idle = idle_omega; e.inertia; e.starter_torque; e.friction_fwd/rev; e.max_rpm; e.rpm_limit;
e.torque_health = c->torque_health;
e.torque_curve = {(torque_rpm[i] or 0, torque_nm[i])} for i < torque_nm.size()
sim.ground = ground; sim.update(dt);
c->rpm_raw = e.omega; c->rpm_smoothed = e.smoothed_omega; c->shaft_speed = sim.shaft_speed;
c->gear_ratio = 0 <= gear < 8 ? c->ratio[gear] : 0;
```

## vmboot check (after the card-147 checks)
- `sim->engine.running == ((flags & 0x4000) != 0)`.
- `sim->engine.torque_curve.size() == car_payload->torque_nm.size()` and every point equals
  CarData's (rpm, nm). Do NOT require max > 0: no engine part is installed yet (card 150); today
  the curve is 1 point of 0 Nm. Print size and max.
- `sim->ratios[i] == car_payload->ratio[i]` for i 0..7 and `ratio[7] < 0`
  (shipped run prints `0 3.5 2.7 1.9 1.3 0 0 -4`).
- card-146 checks (car rests) still pass.

## Done when
`make -C src -j4 check` green; capped vmboot `selfcheck: PASSED`.

## Stop rule
Max 2 build+vmboot runs. If the car no longer rests or a check fails, report numbers and stop;
do not tune vehicle code.
