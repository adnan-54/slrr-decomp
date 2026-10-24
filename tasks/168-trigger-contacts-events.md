---
id: 168
status: todo
branch: card/168
depends: [167]
---
# Trigger contacts + EVENT_TRIGGER_ON/OFF (0x502d60 / 0x4fa900 / 0x4fb070 / 0x47c960): the race-position triggers fire

## Why (RE 2026-10-24, notes/11 "Script triggers")
Card 167 builds the sensor bodies of the race's checkpoint / race-position / pit triggers. The script
counts laps and positions only through `EVENT_TRIGGER_ON` (0x20) notifications
(`CircuitRacing.applyTriggers` -> `handleCheckpoint` / `handleRacePos` / `handlePitStop`, each comparing
`param.token(0).intValue()` with `GameLogic.player.car.id()`). This card makes the world detect car-vs-trigger
overlap and send the events exactly as the exe does. Script does: lap/checkpoint order, positions, finish.
Exe provides: the overlap test, the record list, events 0x20/0x40 and the "%d" param.

## Exe code (rea/Ghidra 12.1.4, read 2026-10-24)
`0x502d60` PhysicsWorld__addTriggerContact(A = sensor body, B = other body, ...), ECX = world:
```
  if (!(A+0x58 & 0x1000)) { tmp c_collision(A,B); queueEvent(A+0x38, 0, 0x20, tmp); return 0; }
  while (B+0x58 & 4) B = B+0x20;                         // root of B
  for rec in world+0x2bc: if (rec+0x24 == A && rec+0x2c == B && rec+0x50 == B+0x40) { rec+0x34 = world.time; return 0; }
  rec = new 0x58 {A, B, +0x34 = world.time}; append to world+0x2bc; world+0x64++;
  c_collision__init(rec, A, B, ...); queueEvent(A+0x38, 0, 0x20, rec);
  if (B+0x40) queueEvent(B+0x38, 0, 0x20, rec);          // (+ B's root resource if it differs)
```
`0x4fa900` trigger sweep (only caller PhysicsWorld__step 0x502060), each record:
```
  if (!A) remove;
  else if (!B || rec+0x34 < world.time - 2*world.dt) {
      if (A && B && |posA - posB|^2 < (bA + bB + 0.05)^2) keep;   // other-state pos +0x64, b = shape+0xc
      else remove; }
```
`0x4fb070` remove: `queueEvent(A+0x38, 0, 0x40, rec)`, `queueEvent(B+0x38, 0, 0x40, rec)` (+ root if
different), unlink, world+0x64--, delete.
`0x47c960` dispatchScript case 0x20/0x40: `obj = sender ? sender : target`;
`other = rec.A+0x40; if (obj.id == other) other = rec.B+0x40; param = sprintf("%d", other)` (String).
A notification forwards with sender = the trigger instance (src `queue_event` already does), so the
Gamemode gets `handleCheckpoint(triggerRef, 0x20, "<car id>")`.
Pair test (0x4fb370 step 4 -> 0x4fd910): V-Clip distance of the sensor shape vs each collision child of the
other root, `d < rA + rB` (margins; 0 for box/kind-6 hulls) -> 0x502d60.

## Files (<= 3 + vmboot)
- `src/physics/world.cpp` (+ decls in `physics.h`): `World::triggers` record list {A, B, stamp} and
  `std::function<void(RigidBody *a, RigidBody *b, uint32_t code)> trigger_event` (set by runtime).
  A trigger pass in `World::step` once per step after the event phase (shortcut comment: the exe tests these
  pairs inside 0x4fb370 during contact generation): for every non-sleeping dynamic root with
  `collision_children`, for every body with `body_flags & 0x40`: bound-sphere broad phase
  (`|dp| <= bA + bB + 0.05`), then `vclip_closest(trigger pose, box poly, child pose, child poly)` < 0 ->
  0x502d60 as above (stamp = `time`). Box poly: `poly_from_triangles` of a 12-triangle box mesh built once per
  trigger body (INVENTED: the exe uses Poly__buildBox, 26 features; say so in a shortcut comment). Then the
  0x4fa900 sweep with 0x4fb070. Sphere/rod triggers: shortcut, skipped (not used by the race scripts).
- `src/game/runtime.cpp`: set `process_world().trigger_event` to `queue_event(owner-of-body, nullptr, code,
  Value::make_str(std::to_string(other id)))` with the 0x47c960 rule (the event to A carries B's id, the
  event to B carries A's id); replace the INVENTED note in `dispatch_script` about 0x20/0x40.
- `src/game/car.cpp`: one line where the car root body is created: `root->owner = r; root->owner_id = r->id;`
  (the exe binds every body to its resource, body+0x38/+0x40).
- vmboot.

## vmboot checks (shipped data only; script state, no hooks in src)
1. After the auto drive (car from z 22.5 to about z -400 along x 40-45, through Norisring race-position
   boxes 1-6 and checkpoint #3 at z 1.84): `CircuitRacing.rpTotal[0] > 0` (`handleRacePos` ran for the
   player car: the param token matched `player.car.id()`, CircuitRacing.java:720-760). Print `rpTotal[0]`,
   `rpNextTrigger[0]`.
2. `nextTrigger[0] == 0` after the drive (checkpoint #3 was crossed but is not the next one; the script must
   ignore it, CircuitRacing.java:513), `current_lap == 1`.
3. Every existing check passes (card 167 count), V-Clip overruns 0, 0 failed threads, final speeds unchanged.

## Done when
`make -C src -j4 check` green; vmboot `selfcheck: PASSED`, checks 1-3 pass. Result lists rpTotal /
rpNextTrigger, the number of trigger records alive at the end, and the "Voronoi error" count.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or
synthetic inputs; cite exe addresses; `INVENTED:` for any gap filling. No map names in src/. Do not port
script rules (lap counting, positions, finish) into src.

## Not this card
Sensor-vs-sensor pairs (the car's wheel triggerspring capsules also carry 0x40), the non-record path
(no 0x1000), kind-6/sphere/rod triggers, Poly__buildBox, reaching checkpoint 0 (z -542, needs steering /
bot Navigator), finish window and return to Garage.

## Result
