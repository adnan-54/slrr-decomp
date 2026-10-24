---
id: 167
status: todo
branch: card/167
depends: [166]
---
# `native trigger` create (type_trigger vf3 0x4d4700): race checkpoint/position boxes become sensor bodies

## Why (RE 2026-10-24, notes/11 "Script triggers")
The career race (Track + `java.game.CircuitRacing` on the Norisring mod map) already runs its whole script
countdown in vmboot (probe: `countDown` 0, `raceActive` 1 at the drive start; `beginRace` -> `unlockCars` sent
"start"). The next race step is the lap logic, and it is dead: every checkpoint / race-position / pit trigger is
`new GameRef(gTrack.map, system:0x34, "x,y,z,0,0,0,box,sx,sy,sz", alias)` (game/Trigger.java), and src builds
nothing for the `native trigger` type (system.rdb record 0x34), so no body exists and no EVENT_TRIGGER_ON can
ever fire. This card builds the body (exe side only). Card 168 adds the trigger pass and the events.
Script does: which triggers, where, sizes, notifications, lap/finish rules. Exe provides: the sensor body.

## Exe code (rea/Ghidra 12.1.4, read 2026-10-24)
`0x4d4700` type_trigger vf3 create(type, params):
```
  T = alloc 0x38;  sx = 10.0, sy = sz = 0;  pos = ypr = 0;  kind = "";
  Str__scan(params, "%f,%f,%f,%f,%f,%f,%64s,%f,%f,%f", pos3, ypr3, kind, sx, sy, sz);
  if (kind == "" || kind == "sphere") def = Physics__createBox(0, 1, sx, 0, 0, 0);   // sphere radius sx
  else if (kind == "box")  def = createBox(0, 5, sx, sy, sz, 0);  // box, half extents (notes/11 §1)
  else if (kind == "rod")  def = createBox(0, 4, sx, sy, sz, 0);  // capsule along z
  else                     def = createBox(0, 6, 1, 1, 1, kind);  // named .scx hull
  def+0xb4 |= 0x20;  def+0xb4 &= ~4;          // mass 0, NOT the flag-4 static (ground) kind
  R = Math__eulerToMat(ypr);                   // 0x5666d0
  Physics__attachToResource 0x4e8a50(T, def, pos, R);   // 0x4f7830 world add at pos/R + bind to the instance
  body = GameRef__getPhysBody();  body+0x58 = (body+0x58 & ~1) | 0x15040;   // 0x40 sensor, 0x1000 keep records
```
`0x4d4a60` vf4 destroy: `0x4e9450` detach, `0x4fb2a0(T+0x1c)` release the body.
`Collide__bodyPair` 0x4fb370 step 4 (notes/11 §4): a pair with body flag 0x40 on one side never makes a
physical contact (goes to 0x4fd910 -> addTriggerContact, card 168).
Str__scan: `%64s` stops at ','; same scanner rules as the type_ground params scan in runtime.cpp
(`scan_float`, `%s` helpers ~line 65).

## Files (<= 3 + vmboot)
- `src/game/runtime.cpp` (+ one field in `runtime.h`: `std::unique_ptr<RigidBody> trigger_body;` next to
  `ground_body`): in `Runtime::instantiate`, after `create_car_bodies`, when the type record's refs hold
  `DataRef::Native` "trigger" (same test as `native_ground_type`, runtime.cpp:181), build the body as above:
  sphere/box/rod -> `set_sphere` / `set_box(half)` / capsule z with mass 0 (inv_mass 0, never integrated);
  `def.flags = 0x20` (bit 2 clear); `body_flags = 0x15040`; `owner_id = r->id`; `owner = r`; pos = parent
  instance pos + params pos (the map record sits at the origin; shortcut comment: the exe composes the full
  parent pose in 0x4e8a50), rot from `euler_to_mat(ypr)` -> `mat_to_quat`; `process_world().add_body`.
  Named .scx kind (kind 6): shortcut, no body + one stderr line (no shipped Track/Gamemode uses it; grep says so
  in Result). In `Runtime::destroy`/unload next to the `ground_body` release: `remove_body` + reset (vf4).
- `src/physics/physics.h`: `RigidBody` gets `uint32_t body_flags = 0;` (+0x58, only bits 0x40/0x1000 used),
  `uint32_t owner_id = 0;` (+0x40, resource id), `void *owner = nullptr;` (+0x38 ResourceRef target).
- `src/physics/world.cpp` `consider_pair`: return early when either side has `body_flags & 0x40`
  (0x4fb370 step 4: sensor pairs make no physical contact; trigger events are card 168). Also keep sensor
  bodies out of `refresh_hull_contacts` / the soup pass (they have no hulls; assert by reading, no code if
  already true).
- vmboot (`src/tools/vmboot.cpp`): the body_count check formula adds the trigger bodies.

## vmboot checks (shipped data only; read the script arrays, no hooks in src)
At the drive start (before the 300 settle frames), `game_object` = Track.game (CircuitRacing):
1. Countdown ran in script: `countDown == 0`, `raceActive == 1` (EVENT_TIME timers p4, CircuitRacing.java
   :460-490) and the player CarData `flags & 0x40004 == 0` ("start" 0x4a3fa6 after "brake"/"stop").
2. For every element of `cpTrigger[]`, `rpTrigger[]`, `pitTrigger[]` (Trigger objects, field `trigger` =
   GameRef, field `pos` = Vector3): its resource has a `trigger_body`, box half extents = the size the script
   passed (cp 30/30/5, rp `rpTriggerSize`/`rpTriggerSize`/5, pit 2/2/5: read them from the shipped Gamemode
   source constants, print them), body pos == `pos` within 1e-3, `inv_mass == 0`, `body_flags == 0x15040`,
   `owner_id == trigger.id()`. Print the counts (expected for Norisring: 4 cp, 39 rp, 3 pit).
   Re-check pos after the semi-auto drive: unchanged (mass 0 lives in `World::bodies`, the non-static list;
   if the integrator, gravity or auto-sleep moves it, that is a bug in this card).
3. No world contact has a sensor body as a side over the whole career block (settle, auto drive, drag,
   semi-auto): the car passes through checkpoint #3 (z 1.84) and race-position boxes on the straight.
4. Every existing check still passes (137), 0 failed threads, V-Clip overruns 0, same final speeds
   (43.98 / 42.04 m/s; a change means a sensor pair leaked into the contact path).

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-4 pass. Result lists: trigger counts per array, one cp/rp/pit body (pos, half
extents), body_count before/after, final speeds.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or synthetic
inputs; cite exe addresses; `INVENTED:` for any gap filling. No map names or trigger sizes in src/ (they come
from the params string). Do not touch the race scripts' rules.

## Not this card
Trigger contact pass, records, EVENT_TRIGGER_ON/OFF and the "%d" param (card 168); Poly__buildBox; kind-6
named hull triggers; trigger render/LOD flags on type+0xc; removing vmboot's settle "wakeup" (the exe wakes
the car on input 0x498599; vmboot already checks that path).

## Result
