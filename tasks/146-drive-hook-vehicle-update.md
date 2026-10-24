---
id: 146
status: todo
branch: card/146
depends: [145]
---
# Drive callback: run Vehicle::update for the woken car each physics sub-step

## Why
`vehicle::Car` (src/vehicle/vehicle.*) ports `Vehicle__update` 0x49c8e0 incl. the suspension
raycast/spring and tyre forces, but nothing calls it: the woken freeride `car_root` rests on its
1x1x1 box. In the exe every car is registered in the game's mode-9 drive list at create and
`PhysicsWorld__step` calls `Vehicle__update(car, dt)` once per sub-step (notes/09 "Drive path").

## Exe functions (4; behaviour to match)
- `Game__registerByInfo` 0x47d560 (from `GameObject__createNative` 0x5240e0 after vf3):
  `if (getInfo(obj, 9)) tail-append to game+0x74 drive list` — `type_car__vf15` 0x4a15c0 returns 1
  for 9 (also 8 and 0x1c), so every native car is a drive object.
- `PhysicsWorld__step` 0x502060 order: applyForces 0x4f8460 -> runControllers 0x4ffca0 (springs)
  -> `Game__runDriveCallbacks` 0x47be30(dt) -> integrate 0x5001c0 -> contacts.
- `Game__driveCallback` 0x47be90: `typed+0x4c` (car) set -> `M->vf[12](car, dt)` = `Vehicle__update`.
- `c_triggerspring__onSensorContact` 0x4f3d70 (reached via `type_car__vf14` event 0x20): nearest
  sensor hit along the spring; src replaces it with `raycast_body` in `Car::update_suspension`.
```
advance_sim sub-step:  if (accumulator + sub >= 1e-4) drive_cars(sub); world.step(sub)
drive_cars(dt): for each CarData with sim: root = car_root; if (root asleep) continue
                sim->ground = map groundbody (world.statics, SHAPE_MESH); sim->update(dt)
```

## Files (3 + check)
- src/game/car.h + src/game/car.cpp:
  - `CarData` gets `std::unique_ptr<Car> sim` (vehicle::Car). In `create_car_bodies` the car_root
    body IS `sim->body` (replace `root_body_storage`; keep `root_body` pointing at it) so Car's
    forces land on the world's root without touching src/vehicle. Copy 145's mass/inertia into it.
  - Map each `WheelData` to a `Wheel`: `mount = hub - com` (car_root local), `radius`,
    `rest_len/max_len/min_len`, `spring/bump/rebound` (145), `friction_eff`, `roll_res`, `pacejka`,
    `drive_eff -> set_drive`, `brake_eff/hbrake_eff`, `drive_ratio`, `steer`, cpatch fields.
  - Engine off and neutral: `sim->engine.running = false; sim->gear = 0;` `INVENTED:` createCar sets
    car+0x70 |= 0x4000 only when M+0x17f0 >= 0 and DAT_007a75df == 0 (not traced); no throttle yet.
  - `void drive_cars(float dt)` as above. `INVENTED:` the call runs before World::step's force pass
    instead of between runControllers and integrate (src integrates from the pre-force snapshot, so
    the impulses must land first); sleeping roots are skipped (the exe's sensors never touch the
    ground while the root sleeps, so the spring retires and no force is applied, 0x4f2d70 step 1).
  - `reset()` already frees payloads: free `sim` with them.
- src/script/natives.cpp `advance_sim`: one call to `game::drive_cars(sub)` before `world.step(sub)`
  (guarded like the step's 1e-4 accumulator test).
- src/tools/vmboot.cpp: replace the wakeup resting checks.

## vmboot check (real data, hard)
After the queued wakeup + 300 frames on the freeride map, for the player car `sim`:
- every wheel `contact == true`, `load > 0`, compression `rest_ray() - ray_len > 0`;
- `|sum(load) - m*9.81| / (m*9.81) < 0.10` (the car is carried by its springs);
- no world contact has the car_root or part_comp as a side (the box no longer touches the soup);
- `|vel.y| < 0.1`. Print per-wheel ray_len/load, car_root pos.y and floor.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`; report the
  per-wheel numbers. If the car bounces or sinks, report the numbers, do not tune constants.

## Not this card
Player input / `Vehicle__updateControls` 0x498300 (mode 8, next card), the sensor-capsule bodies and
trigger event path (0x4f3f10/0x502d60/0x4f3d70), wheelbone/a_arm bodies, per-face ground materials,
traffic controller `car+0x2054`.
