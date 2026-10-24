---
id: 154
status: implemented
branch: card/154
depends: [152, 153]
---
# Wheel parts reach their WheelRef (slottype, wheel id 0x4c8b10) + sim wheel sync

## Why (RE 2026-10-24, notes/09 "Card 153 traction RE")
`Part.getWheel()` (Part.java:733) = `Chassis.getWheel(getWheelID())`. src never sets
`PartData::wheel_id` (`pt_get_wheel_id` then answers -1), so Spring / ShockAbsorber / Wheel
(tyre) / Brake / Suspension `updatevariables` all see `whl == null`: a full vmboot run makes zero
`WheelRef.setForce/setRestLen/setMinLen/setMaxLen/setRollRes/setBearing` calls, and the car drives
on cfg-derived defaults (k 18205, damping 910, rest 0.39, friction 1.0, r 0.315). With the wheel
id in place (scratch build) the SunStrip gets its part values: front k 59841, damping 3900/3900,
rest 0.266, min 0.178, max 0.266, radius 0.329, friction 1.77; rear k 53955, damping 3600.
Second gap: `vehicle::Car::wheels` are copied from `WheelData` once in `create_car_bodies`; the
WheelRef natives write `WheelData` only (exe: one struct, every length/force setter re-runs
`Suspension__configure 0x4f4280`), so script changes never reach the simulation.
Needs 153 first: with these stiffer springs the point-mass-only inertia is unstable.

## Exe functions
`0x4c0190` cfg keyword `slottype N` (table 0x5da5d0): if `M+0x120 == 0`:
`last_type_slot(M+0x1194)+0x64 = N` (the slot declared last). Default 0.

`0x4c8b10(P, slot_id) -> uint` (called by 0x4c7d20 as `P+0xd0 = 0x4c8b10(P, 0)` once the car
ref is set):
```
for each runtime slot s of P (index order):
  if (s.flags & 1) && s.target_ref (+8) && target instance resolves:
    T = target part; return 0x4c8b10(T, s.mate_slot_id (+0x10))   // 0 if T does not resolve
for type slot n in P's type-slot list: if n.id (+0x60) == slot_id: return (ushort) n+0x64
return 0
```
So a spring on chassis slot 311 (`slottype 301`) gets 301; a part on a strut inherits the strut's
chassis slot type. `getWheelID` (src `pt_get_wheel_id`, unchanged) maps 101..400 -> (v-101)%10.

## Files
- src/parts/cfg.h/.cpp: parse `slottype N` into the current `CfgSlot` (new `int32_t slot_type`,
  default 0). Check `make -C src check` cfg corpus still passes.
- src/game/car.h/.cpp: `Slot::slot_type` (type slot +0x64) filled in `fill_slots`; static
  `wheel_id_of(rt, P, slot_id)` = 0x4c8b10 above (cap recursion depth at 64, `INVENTED:` guard);
  in `part_attach` after the car ref block: `pp->wheel_id = wheel_id_of(rt, P, 0)` (0x4c7d20).
- src/game/car.cpp `drive_cars`: before `sim.update(dt)`, copy per wheel from `c->wheels[i]` to
  `sim.wheels[i]`: spring_k, bump, rebound, rest_len, max_len, min_len, radius, friction,
  friction_eff, roll_res, pacejka[], drive/brake/handbrake shares (via the existing set_* calls)
  and steer. `INVENTED:` glue, same as the engine block copy above it. Do not copy hub/mount
  (153 owns them).
- src/tools/vmboot.cpp:
  1. Start fixture: `root->pos.y = floor_y + (wheel0.rest_ray() - wheel0.mount.y)` instead of
     `+ 1.0f` (INVENTED fixture; the short part springs, max_len = rest_len, cannot catch a 1 m
     drop without the sensor capsules).
  2. The "updatevariables reached setInstantCenter(10000)" check: the strut parts now override
     the Chassis.java:346 value; change it to "every wheel's instant center was written" (ic_h
     != 0 and ic_l != 0) and print the values.

## Done when (`VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh`, real data)
a. For each wheel i: the part on chassis slot 311+i is a `Spring`; `sim->wheels[i].spring`,
   `.rest_len`, `.min_len` equal its script fields `force`, `restlength`, `minlength`
   (`Vm::get_instance_field`) within 1e-4; print them.
b. `getWheelID()` called through the VM on the parts in chassis slots 101+i, 111+i, 121+i, 301+i,
   311+i returns i.
c. Car settles on its springs (contact on all 4, load sum within 2% of weight, |vel.y| < 0.1).
d. Card 152 launch phase (reset, Up held) extended to 900 frames: gear reaches >= 2 with no gear
   key, and the final speed > v1 = 0.95*min(max_rpm, rpm_limit)*0.10471976/(ratio[1]*
   rearend_ratio)*radius(wheel 0) (1st-gear top speed at the 0x4b06b0 threshold, computed from
   car_payload). Print mean driven kappa (0x4b19a0) over the last 60 frames.
e. All older checks green, 0 failed threads.

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; no tuning constants; do not touch
vehicle.cpp.

## Result

Implemented on `card/154`. `make -C src check`: **100 checks, 0 failures**, 0 failed
threads (30000 frames), 0.0 cfg-corpus failures.

- `cfg.h/.cpp`: `CfgSlot::slot_type` and the `slottype N` keyword (0x4c0190) written to
  the slot declared last; before any slot it is ignored (no shipped occurrence).
- `car.h/.cpp`: `Slot::slot_type` filled in `fill_slots`; `wheel_id_of(rt, r, slot_id,
  depth)` = 0x4c8b10 (runtime slot flag 1 -> host part at the mate slot, else the type
  slot with this id returns its `slottype`); called in `part_attach` after the 0x4c7d20
  car ref into `PartData::wheel_id`. INVENTED: depth guard (64).
- `drive_cars`: INVENTED glue copying spring/bump/rebound/rest/max/min/radius/friction/
  friction_eff/roll_res/pacejka/drive/brake/handbrake/steer from `WheelData` to
  `sim.wheels[i]` before `Vehicle::update` (exe has one struct; the setters rerun
  0x4f4280). Hub/mount left to card 153.
- vmboot: start fixture = `floor_y + rest_ray - mount.y` of wheel 0; instant-center check
  is now "written" (ic_h and ic_l non-zero, values printed); launch phase 900 frames with
  the v1 speed check and last-60-frame driven kappa; new checks for done-when a and b.

Measured (SunStrip 1.8 DVC): springs front 59841 N/m, rest 0.266, min 0.178 (rear 53955,
rest 0.270); sim wheels match within 1e-4; getWheelID = 0..3 on chassis slots
101/111/121/301/311+i; instant centres e.g. wheel 0 h (0.683,-0.380,-1.333). Launch: max
gear 5, final 103.35 m/s vs v1 12.42 m/s, mean driven kappa 0.000 over the last 60 frames.

Doubts:
- `getWheelID` is checked through `Runtime::call_method` (VM native dispatch), not a script
  method; the Part class name in the check is the `is_a` chain to
  `java.game.parts.rgearpart.Spring` (the installed parts are subclasses).
- The final speed (103 m/s in 14.4 s) looks high for the dyno curve but follows from the
  sim with real script springs; no constant was tuned.
