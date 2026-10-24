---
id: 158
status: todo
branch: card/158
depends: [157]
---
# Vehicle::update inside World::step (after the force pass) with forces, not impulses (0x502060)

## Why (RE 2026-10-24, notes/08 "Exact pass order")
src runs `game::drive_cars` (Vehicle::update + spring solve) **before** `World::step` (natives.cpp
advance_sim) and `vehicle::Car::apply_contact_impulses` applies every tyre/spring force as an impulse
`F*dt` on the start velocity, because World::step's force pass zeroes `force`. The exe runs the drive
callbacks **inside** the step, after the force pass and the controllers and before integrate, and the
vehicle adds forces to the accumulators that RK4 holds constant. Position gets `dv*h` (impulse) instead of
`dv*h/2` (force). Both INVENTED markers (car.cpp drive_cars comment, vehicle.cpp apply) go away.

## Exe functions (pasted)
`PhysicsWorld__step` 0x502060 (start of the step body, after the accumulator/prologue):
```
for root in w[0x21] children: PhysBody__applyForces 0x4f8460(root)   // gravity/drag, clears other-state bit0
PhysicsWorld__runControllers 0x4ffca0()                               // c_triggerspring vf3 0x4f2d70 (spring force)
Game__runDriveCallbacks 0x47be30(t)                                   // -> 0x47be90 -> Vehicle__update 0x49c8e0
for root: PhysBody__integrateTree 0x5001c0(root, t)
PhysicsWorld__pruneConstraints 0x4fa4d0(); sub-iteration loop ...
```
`PhysBody__addLocalForceAtLocalPoint` 0x504e70(body, f, P) (tyre force, notes/09 §6): `F += R·f;
τ += (R·P) × (R·f)` on the active state; `PhysBody__applyForceAt` 0x5051e0(body, F, P) (spring 0x4f2d70
step 11, drag): `F += F; τ += (R·P) × F`. Neither touches velocity or momentum; integrate 0x5001c0 ->
0x506530 RK4 uses them as constant force/torque.

## Files (3 + vmboot)
- src/physics/physics.h + world.cpp: `World` gets `void (*drive)(float dt) = nullptr;` (0x47be30 slot).
  In `World::step`, call `if (drive) drive(h);` after the force-pass loop and the statics snapshot,
  before `update_contacts(*this, true)` and the first integrate. Comment: 0x502060 order A-D.
- src/vehicle/vehicle.cpp: `apply_contact_impulses` -> `body.apply_force_at(p.force, p.point)` (0x504e70 /
  0x5051e0; `apply_force_at` is world F at a local point, same as both). Rename to `apply_contact_forces`
  only if every caller is updated (vehicle.h + `Car::update`); otherwise keep the name. Delete the unused
  `Car::step` (it calls update then world.step, which would now zero the forces) if nothing calls it
  (grep first). The 157 `INVENTED:` hit-timing comment in update_suspension: replace with
  "hit on the previous step's final pose = the pose 0x4fc610's collide pass saw (notes/08 H); differs only
  after an in-loop resolveImpacts" (keep an `INVENTED:` tag for that remaining difference).
- src/script/natives.cpp advance_sim (~7669): drop the `game::drive_cars(sub)` call before `world.step`;
  set `world.drive = game::drive_cars` once (where the process world is first used there, or before the
  sub-step loop). Comment-only edit in src/game/car.cpp: the INVENTED block above `drive_cars`
  ("runs before World::step's force pass ...") becomes "called by World::step between the force pass and
  integrate (0x502060 C)"; keep the sleeping-root sentence.

## vmboot check (src/tools/vmboot.cpp)
Keep every existing check unchanged; they are the exe-derived expectations: every wheel rests on its
spring, each load within 10% of its static share (~3.1 kN), sum within 2% of weight, no world contact on
car_root/part_comp, |vel.y| < 0.1, Spring part values reach the sim wheels, sensor normals == PHYS normals,
gear >= 2 and final speed > v1 (0x4b06b0 threshold). Add:
1. Count drive-hook calls and world steps that ran with the car awake during the run; hard check equal
   (0x47be30 once per executed 0x502060 step).
2. Over the 300 settle frames: car_root `vel` and `angular_momentum` immediately after the hook equal the
   values immediately before it bitwise (0x504e70/0x5051e0 write accumulators only). Wrap the hook in
   vmboot (save/compare, then call `game::drive_cars`) rather than adding probes to src.
Print the new loads per wheel, sum vs weight, max gear, final speed vs v1 next to the old ones.

## Done when
- `make -C src -j4 check` green.
- `VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh` (or the same binary under
  `ulimit -v 1500000; timeout 300` if the script is blocked): `selfcheck: PASSED`, all older checks green
  with the numbers reported, both new checks pass, 0 failed threads.

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; no tuning constants (if an older check now fails,
that is a finding to report, not a reason to change its threshold).

## Not this card
New-contact generation moving from the start of the step into the event phase (0x4fc610, notes/08 H),
in-loop resolveImpacts, body-vs-soup narrow phase 0x4feb20/0x4fc7e0 (notes/19 §7, not carded: 6 more
callees, no vmboot scenario where the box touches the ground), control callbacks 0x47bc20, sleep check
0x502530, finishContacts 0x4fa900.

## Result
