---
id: 147
status: merged
branch: card/147
depends: [146]
---
# Player controller binds the car; engine on at create; getWheel loads WheelRef

Rewritten 2026-10-24 after the first Flash run hit max turns. Every step below was run by Claude in
a scratch worktree on top of cf43df7: with exactly these changes vmboot binds the controller and
the checks below pass. Do NOT port the CarData -> vehicle::Car bridge here (card 149) and do NOT
check the torque curve (empty until parts are installed, card 150).

## Root causes found (do not re-investigate)
1. `Chassis.updatevariables line 340: Chassis.getWheel: java.game.parts.WheelRef is not loaded`
   is PRE-EXISTING on main (forceUpdate ignores the error, so every updatevariables aborted at its
   first getWheel). Exe `Chassis.getWheel` 0x4b4150 calls `JVM__getClass("java.game.parts.WheelRef")`
   (0x55add0, loads on miss); src calls `vm.find_class` (no load). Fix: `vm.get_class`.
2. `player_command` never fired because `g_players.insert(inst)` ran while `g_rt` was still
   unset; the next `car_data()` call saw `g_rt != &rt` and `reset()` cleared `g_players`.
   Fix: `if (g_rt != &rt) reset(&rt);` before the insert.
3. `Class not found: ...` lines are import probes (packages.cpp:272). Ignore them.

## Changes (copy; exe citations in notes/09 "Control path")
Base: the hunks of `/tmp/claude-0/-root-slrr-re/a4e760fe-f689-48e3-a3bd-ff9290332a02/scratchpad/card147-partial.patch`
if it still exists, MINUS every `DBG` fprintf and MINUS the whole `drive_cars` hunk (car.cpp
`@@ -658`) and MINUS the vmboot hunk (rewrite per below). If the file is gone, write it from this list:

- src/script/natives.cpp
  - `ch_get_wheel`: `vm.find_class("java.game.parts.WheelRef")` -> `vm.get_class(...)`; comment
    `// 0x4b4150: JVM__getClass loads the class on a miss`.
  - move `bool steam_neutralized = false;` (0x7a75df) out of the anonymous namespace to file scope
    (car.cpp declares `extern bool steam_neutralized;`).
- src/game/car.h: CarData `int control_slots = 0; int driver_slot = -1;` (M+0x17ec / M+0x17f0),
  `Runtime::Resource *slot_player[4] = {};` (R_s player ref, car+0x150+s*0x4a8),
  `float starter_timer = -1.0f; // +0x1e6c (0x499511)`, `float prev_clutch = 0.0f; // +0x1fa8`.
  Declare `int player_command(Runtime &, Runtime::Resource *, uint32_t code, const Value &param);`
- src/game/car.cpp
  - `std::set<Runtime::Resource *> g_players;` cleared in `reset()`.
  - `has_native_ref(rt, r, native)`: same parent walk as `load_native_cfg`, true when a ref has
    `kind == DataRef::Native && native == name && !is_cfg`.
  - `fill_car`: 0x48dee0 / 0x48e070:
    ```
    if key == "controller" || key == "driver":
        if control_slots < 4: { if control_slots == 0: driver_slot = 0
                                if key == "driver": driver_slot = control_slots - 1
                                control_slots++ }
        continue
    if key == "nodriver": driver_slot = -1; continue
    ```
  - `scan_id(cmd, word, &id)`: skip word + blanks, `strtol(p, &end, 0)` (both "0x.." and negative
    decimal ids occur: Track sends `controllable -2147482363`), fail if no digits.
  - `create_car_bodies`, first lines:
    ```
    if (type && has_native_ref(rt, type, "player")) {   // 0x523c80 -> 0x5240e0 native player
        inst->event_mask |= 0x10u;    // INVENTED: default mask not traced (card 040)
        if (g_rt != &rt) reset(&rt);  // else the first car_data() wipes g_players
        g_players.insert(inst);
    }
    ```
    and replace the INVENTED `sim->engine.running = false` with 0x49a4ef:
    `if (car->driver_slot >= 0) car->flags = steam_neutralized ? flags & ~0x4000 : flags | 0x4000;`
    (keep `sim->gear = 0`). Do not touch `sim->engine.running` (card 149 copies the flag).
  - `car_command`: `"activate "` (0x4a3360, slot search 0x4a1470: first slot whose player id ==
    pid, else first empty slot, both within `control_slots`; `rt.resolve(pid)`; return 1 when
    bound, 0 if no player/slot), `"transmission "` (0x4a37f0: `flags &= ~0x30; if (tr&1) {
    tr&2 ? flags &= ~0x40 : flags |= 0x40; flags |= tr&4 ? 0x20 : 0x10; prev_clutch =
    clutch_input; }`, INVENTED comment for the tr&1==0 branch), `"damage_multiplier "` -> return 1.
  - `player_command` (0x4d2d20 -> 0x4d2ab0): `if (!g_players.count(player)) return 0;` on code 0x10
    with `"controllable <id>"`: `rt.queue_event(car, nullptr, 0x10, "activate 0x<player->id> 0x0 0x0")`
    (INVENTED: camera/render ids 0, +0x2310 list and "control" reply not kept). Return 1 for every
    code (the Controller script never sees commands).
- src/game/runtime.cpp
  - `Runtime::resolve`: before the registry lookup, ids with high word 0x8000 or 0xffff are looked
    up in `instances_` by `id` (from the patch; keep its comment).
  - `queue_event`: `if (!game::car_command(...) && !game::player_command(...) && target->script)`.

## vmboot check (src/tools/vmboot.cpp, after the card-146 "car settles" check)
Get `GameLogic.player` like line ~576, then its `controller` field object `ctrl_obj`.
- `car_payload->control_slots == 1 && driver_slot == 0` (Axis `_main_*.cfg` one `controller` line).
- `car_payload->slot_player[0] && car_payload->slot_player[0]->script == ctrl_obj`
  (do NOT use `runtime()->handle(ctrl_obj)`: it is null for the Controller).
- `flags & 0x4000` set.
- `flags & 0x70` == bits from static `java.util.Config.player_transmission`
  (`tr&1 ? ((tr&2)?0:0x40) | ((tr&4)?0x20:0x10) : 0`); shipped value gives 0x50.
- every `car_payload->wheels[i]`: `fabs(ic_h[0]-hub[0]-10000) < 0.01 && fabs(ic_l[2]-hub[2]-10000) < 0.01`
  (Chassis.java:346 `setInstantCenter(10000 x6)` now runs in updatevariables; proves fix 1).
Print slots, driver, flags, transmission. Expected run: 75-ish checks, all pass.

## Done when
- `make -C src -j4 check` green; `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`
  prints `selfcheck: PASSED`. No DBG prints left (`git diff | grep -c DBG` == 0).

## Stop rule
Max 2 build+vmboot runs to fix a failing check. If a check above still fails, stop and report the
printed numbers and the failing line; do not add debug tracing, do not touch vm.cpp/packages.cpp,
do not bend checks.

## Not this card
CarData -> vehicle::Car bridge (149), part install / engine torque curve (150), reading axes (148).
