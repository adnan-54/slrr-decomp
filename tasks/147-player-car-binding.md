---
id: 147
status: todo
branch:
depends: [146]
---
# Player controller binds the car; engine on at create; car inputs/engine block reach vehicle::Car

## Why
Track.enter (Track.java:814) sends `player.controller.command("controllable " + car.id())`
(:872) and `car.command("transmission " + Config.player_transmission)` (Vehicle.java:197-205).
src drops both (only `game::car_command` runs in `Runtime::queue_event`), so the car has no
driving player. createCar also turns the engine on for cars with a driver slot (card 146 left it
off as INVENTED). And `vehicle::Car` never sees CarData's inputs or engine block: its
`Engine::torque_curve` is empty and `ratios[]` are placeholders, so throttle could not move the
car even once card 148 writes the inputs. notes/09 "Control path" has the full trace.

## Exe functions (behaviour to match)
- `type_player__vf14` 0x4d2d20, code 0x10 `"controllable %d"` -> 0x4d2ab0:
```
id = scan("%*s %d"); if id already in player+0x2310 list -> return 1
Game__queueEvent(car(id), 0, 0x10, "activate 0x%x 0x%x 0x%x" % (player id, player+0x218c, player+0x219c))
Game__queueEvent(car, 0, 0x10, "damage_multiplier %.3f" % player+0x21c4)
Game__queueEvent(player, 0, 0x10, "control 0x%x" % car id)
return 1            // vf14 returns 1 for EVERY code: the Controller script never gets events
```
- `type_car__vf14` 0x4a2e60, `"activate"` (0x4a3360) with slot search 0x4a1470:
```
pid = scan("%*s %d %d %d")[0]     // custom scanner: "0x..." ids parse (use strtol base 0)
s = first slot in [0, M+0x17ec) with R_s.player_id == pid, else first with id 0, else return
R_s (car+0x150 + s*0x4a8).player = ref(pid)     // +0x158 = id
```
- `"transmission %d"` (0x4a37f0): `flags &= ~0x30; if (tr&1) { flags = (tr&2) ? flags&~0x40 :
  flags|0x40; flags |= (tr&4) ? 0x20 : 0x10; car+0x1fa8 = clutch }` (tr&1 == 0: manual branch;
  `INVENTED:` only `flags &= ~0x30` is ported there, rest not read).
- car cfg `controller`/`driver` lines -> 0x48dee0 (`M+0x120` taken as 0, UNRESOLVED):
  `if (slots < 4) { if (slots == 0) driver_slot = 0; if (key == "driver") driver_slot = slots-1; slots++ }`;
  `nodriver` 0x48e070: `driver_slot = -1`. Defaults (0x49b34e): `slots = 0, driver_slot = -1`.
- createCar 0x4992e0 at 0x49a4ef: `if (driver_slot >= 0) { if (!steam_neutralized /*0x7a75df*/)
  flags |= 0x4000 else flags &= ~0x4000 }`; `car+0x1e6c = -1.0` (0x499511).

## Files (3 + check)
- src/game/car.h / src/game/car.cpp:
  - CarData: `int control_slots = 0, driver_slot = -1;` `Runtime::Resource *slot_player[4] = {};`
    `float starter_timer = -1.0f; // +0x1e6c`, `float prev_clutch = 0; // +0x1fa8`.
  - fill_car: count `controller`/`driver`/`nodriver` cfg lines as above (Axis `_main_*.cfg:105`
    `controller 0 1 2 3 4 5` -> 1 slot, driver 0). Camera records M+0x1858 not parsed.
  - create_car_bodies: replace the INVENTED `engine.running = false` with the 0x49a4ef rule
    (`extern bool steam_neutralized` from natives.cpp:560) on `c->flags`; `sim->gear = 0` stays.
  - car_command: `"activate"` (slot rule above, resolve the id with `rt.resolve`) and
    `"transmission"`; `"damage_multiplier"` -> return 1, no effect (shortcut).
  - `game::player_command(rt, res, code, param)`: returns 1 for every code when `res`'s record
    chain has the `native player` ref (same walk as `load_native_cfg`, `DataRef::Native`,
    `native == "player"`, no cfg), else 0. `"controllable"` queues `"activate 0x<pid> 0x0 0x0"`
    to the car resource (`INVENTED:` camera/render ids 0, player+0x2310 list and `"control"`
    reply not kept: no consumer in src). Other commands: no-op (shortcut: osd, viewport,
    camera, renderinstance, device, leave, control).
  - bridge in `drive_cars` (INVENTED glue: the exe has one struct; src has CarData + Car):
    before `sim->update`: `sim->engine.running = flags & 0x4000`; inputs throttle/brake_input/
    handbrake_input/steer_input/clutch_input/nitro_input/gear -> sim; engine block
    `torque_rpm/torque_nm` -> `engine.torque_curve`, idle_omega, inertia, starter_torque,
    friction_fwd/rev, max_rpm, rpm_limit, `torque_health` (+0x1e04, notes/09 §1 T_max),
    `ratio[0..7]` -> `ratios`, `rearend_ratio` -> `final_drive`, `max_steer`.
    After: `rpm_raw = engine.omega`, `rpm_smoothed = engine.smoothed_omega`, `shaft_speed`,
    `gear_ratio = ratio[gear]`. Do not change src/vehicle.
- src/game/runtime.cpp `queue_event`: try `game::player_command` after `car_command`.
  `INVENTED:` the Controller resource needs `event_mask |= 0x10` like card 141's car (default
  mask not traced): set it where the native player is created (or in player_command's caller
  path, whichever keeps the mask gate of 0x47c760 step 2).
- src/tools/vmboot.cpp: checks below, after the card-146 resting checks.

## vmboot check (real data, hard)
On the freeride Track after the existing flow:
- `car_payload->control_slots == 1`, `driver_slot == 0` (Axis cfg) and
  `slot_player[0]` == the resource of `GameLogic.player.controller` (script object -> resource).
- `flags & 0x4000` set; `flags & 0x70` == the 0x4a37f0 bits for the VM's
  `java.util.Config.player_transmission` (read the static, compute 1 -> 0x50, 5 -> 0x60, ...).
- `sim->engine.torque_curve` has `torque_nm.size()` points equal to CarData's, max > 0;
  `sim->ratios[i] == CarData.ratio[i]` for i 0..7 and `ratio[7] < 0` (reverse).
- card 146 checks still pass (car rests; engine idles in neutral). Print the slot, flags,
  transmission, curve max, ratios.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`; report
  the printed numbers. If a check fails on shipped data, report it, do not bend the check.

## Not this card
Reading the axes / `Vehicle__updateControls` 0x498300 (card 148), auto gearbox/clutch
(0x497930/0x4977a0), camera/HUD slot records (0x4ad770, 0x49c800), `"steerhelp"`.
