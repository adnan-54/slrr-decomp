---
id: 141
status: todo
branch: card/141
depends: [140]
---
# Native car command handler: "wakeup"/"suspend"/"start"/"brake"/"reset" (car falls once awake)

## Why
After card 140 every car's `car_root` body is asleep in the process world (`w+0x88`).
Scripts wake it with `Vehicle.wakeUp()` = `queueEvent(null, EVENT_COMMAND, "wakeup")`
(Vehicle.java:185), which the exe delivers to the native type handler before the script
(`Game__dispatchEvent` 0x47c760 step 2). src `Runtime::queue_event` has no native handler
(`shortcut:` comment there). RE: notes/08 "Car body creation, sleep/wake" (Who wakes the car),
notes/18 §1.

## Exe functions (5)
- `Game__dispatchEvent` 0x47c760 step 2 (notes/18 §1):
```
if ((typed+0xa0 & code) && (typed+0x4c == 0 || native->vf14(typed+0x4c, sender, code & 0x0fffffff, param) == 0)
    && typed+0x50) script dispatch 0x47c960
// notification walk unchanged
```
  NOTE the order: the native handler runs only when the mask bit is set, and its non-zero
  return suppresses the script `handleEvent`.
- `type_car__vf14` 0x4a2e60 `(car, sender, code, msg)`, EVENT_COMMAND (0x10) branch, command
  = first word of msg (`Str__scan(msg, "%s")`, strcmp 0x57c11e), only these five:
```
"brake"   (0x4a3f6d): car+0x70 |= 0x40000;                      return 1
"start"   (0x4a3fa6): car+0x70 &= 0xfffbfffb;                   return 1
"suspend" (0x4a3fdf): PhysBody__sleepRoot(PhysBody__root(car+0x13ec));   return 1   // 0x4ae5a0
"wakeup"  (0x4a4023): PhysBody__wakeRoot(PhysBody__root(car+0x13ec));    return 1   // 0x48ca50
"reset"   (0x4a4067): r = PhysBody__root(car+0x13ec)
                      r.active: vel(+0x70..0x78) = 0, (+0xd4..0xdc) = 0
                      car+0x1e5c gear = 0; car+0x1e0c = (car+0x2050)->+0x24
                      car+0x1ddc steer, +0x1de0, +0x1de4 throttle, +0x1de8 brake, +0x1df4 handbrake = 0
                      car+0x1e7c rpm_raw = 0, car+0x1e78 = 0
                      ... (0x4a413a onward: wheel loop on +0x1fb4, not this card)
```
- `PhysBody__root` 0x480700: `while (b+0x58 & 4) b = b+0x20; return b`.
- `PhysBody__wakeRoot` 0x48ca50: `if (b+0x58 & 0x80) wake(root(b))` (wake = 0x507060, card 140).
- `PhysBody__sleepRoot` 0x4ae5a0: `if (!(b+0x58 & 0x80)) sleep(root(b))` (sleep = 0x506d90, card 140).

## Files (3)
- src/game/runtime.cpp `Runtime::queue_event`: inside the mask test, call the native handler
  first when the target has a car payload with bodies (`game::car_command(...)`, exported
  from car.h); run `dispatch_script` only if it returned 0. Replace the `shortcut:` line with
  one naming what is still missing (other native kinds, streaming check).
- src/game/car.cpp + car.h: `int car_command(Runtime &rt, Runtime::Resource *car, uint32_t code,
  const Value &param)`: code != 0x10 or param not a string -> 0; the five commands above on
  `CarData` (`flags` = car+0x70, inputs, gear) and `process_world().wake/sleep` of the root
  of `chassis_body` (`PhysBody__root` = follow `parent`). Commands not in this list return 0
  (`INVENTED:` the rest of the 0x4a2e60 chain — cfg keys like `abs`, `filter`, `render`,
  `reload` — is not ported; returning 0 lets the script see them, as before this card).
- src/tools/vmboot.cpp (checks + one driver step):
  - real-data check: the Garage path already runs `player.car.wakeUp(); player.car.command("suspend")`
    (Garage.java:289/473) -> when `actualState` first becomes `java.game.Track` (before the
    driver step below) the player car root is in `process_world().sleeping`, and no
    `handleEvent` failure appears.
  - `INVENTED:` driver (same as `Gamemode.wakeUpCars`, Gamemode.java:799, whose call in
    Track.java:1056 is commented out; in the exe the freeride car wakes from player input in
    `Vehicle__updateControls` 0x498300 at 0x498599): once `actualState` is `java.game.Track`,
    queue `EVENT_COMMAND "wakeup"` to `player.car` through `Runtime::queue_event`, record
    `root_body->pos.y`, run 300 more frames; hard checks: root body is in
    `process_world().bodies` (awake), its `pos.y` decreased (gravity, no ground yet),
    `failed: 0`.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, new
  checks ok. Report pos.y before/after the 300 frames (free fall: about -0.5*9.81*t^2 with
  t = simulated seconds; drag is small at mass 1).

## Next (not this card)
Native ground `type_ground__vf3` 0x4bade0 (contacts so the car rests on the map), wheels and
child shapes (0x4f78d0, suspension 0x4915d0), compound mass 0x5032d0, wake from input
(0x498300 at 0x498599), vehicle::Car force pass on the woken body.
