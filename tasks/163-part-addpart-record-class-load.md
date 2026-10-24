---
id: 163
status: merged
branch: card/163
depends: [162]
---
# add_part -> Chassis.addpart (0x4ca390/0x4942b0) + on-demand load of record-named classes (0x55add0)

## Why (RE 2026-10-24, notes/13 §15.2, notes/18 §6)
The next gameplay step is a career race: Garage CMD_EVENTLIST 133 (Garage.java:1587-1593) only opens
`EventList(MODE_AMATEUR)` when `player.car.isDriveable()` is null. In src it never is:
1. `Part.attachedParts` (Part.java:263) is filled only by the exe calling `Part.addpart(GameRef)` on the
   car's script (the Chassis) for every committed install. src `part_attach` (car.cpp ~715) skips it,
   so `isDriveable2` (Vehicle.java:288) says "there's no engine installed." for every car.
2. `Runtime::script_instance` (runtime.cpp:706) resolves a record's `script <FQN>` with
   `vm_->find_class` (already-loaded table). The exe record loader 0x51fcc0 sends that form to
   0x55add0 `JVM::getClass`, which loads the class on demand (notes/03 "Class load path"). Classes no
   script names as a constant (`java.game.parts.accessories.stock_Battery_{black,blue,silver}`,
   parts.rpk 0x53ff/0xe8/0xe9) get no script instance -> `xa.create` returns null -> no car ever has a
   battery -> "the car is missing a battery.".
Scratch probe with both fixed: CMD_EVENTLIST -> EventList -> CMD_RACE -> `java.game.Track` with
`java.game.CircuitRacing`, 0 failed threads; freeride selfcheck stays 127/127.

## Exe code (rea/Ghidra 12.1.4 + asm, read 2026-10-24)
`0x4c7d20` Part__commitInstall (notes/13 §15): after `P+0xc0` car ref and `P+0xd0 = 0x4c8b10(P,0)`:
`0x4ca390(P, 0)`, then the `"install"` script callback.
```
void FUN_004ca390(P, int param_2) {                       // ECX = part type M
  if (P+0xc8 /*car id*/ != P+8 /*own id*/) {               // 0x4ca3a4
    sprintf(buf, "add_part %d", P);                        // 0x4ca3b3: %d = the native part pointer
    Game__queueEvent(P+0xc0 /*car ref*/, 0, 0x10, buf, 0); // 0x47d550 -> 0x47c760, synchronous
    if (param_2 == 0) P+0xac = Game__getInfo(P+0xc0, 0x3f, 0);
    /* per render P+0x4c[i]: +0x1b4 = +0x1b8 = P+0xac */
  }
  if (getInfo(P+0xc0, 7) == 5) { /* add_light, add_wing, add_rollbar, add_mslot commands */ }
  /* add_linked / bone00 block for type slots with flag 1 */
  for (each M runtime slot s of P (stride 0x68, count M+0x1160))
    if (s+8 && (s+0x14 & 2) && s->ref /*child installed on P*/ resolves) {
      child+0xc0 := ref(P+0xcc /*car id*/); child+0xd0 = P+0xd0; child+0xac = P+0xac;
      FUN_004ca390(child, 1);                              // 0x4ca922
    }
  if (param_2 == 0 && P+0xc8 != P+8) Game__queueEvent(P+0xc0, 0, 0x10, "updatevariables", 0);
}
```
`type_car__vf14 0x4a2e60`, word `add_part` (asm 0x4a50bc..0x4a510b):
```
  Str__scan(cmd, "%*s %d", &p);        // 0x569380
  FUN_004942b0(car, p);
  r = FUN_004a8200(car, p); if (r) r+0x88 |= 1;
  return 1;                            // handled: no script handleEvent
```
`0x4942b0(car, P)` (first block only for this card):
```
  node = ResourceRefDLNode(P);                         // 0x47b090
  append node to the car list car+0xfc..+0x104;
  typed = car's typed data; if (typed && typed+0x50 /*script = Chassis*/)
    GameObject__callScriptMethod(typed, "addpart", 0, 4, new GameRef(P), 0x4f);   // 0x47dd50
  /* engine slot 0x191 bookkeeping + wheel-part pivot branch: not this card */
```
`0x51fcc0` record loader (notes/03): `script <FQN>` (no `.class`) -> `0x55add0(FQN)` = loaded table,
else load through the package tree (0x55c0c0 -> 0x55fc10 -> 0x55bec0).

## Field map exe -> src
| exe | src |
|---|---|
| `P+0xc0/+0xc8` car ref / id | `PartData::car_ref` / `car_id` |
| `P+0xd0` wheel id | `PartData::wheel_id` |
| M runtime slot `flags & 2`, `+0xc` ref | `Slot::flags & 2`, `Slot::attached` (script Object of the child) |
| car typed `+0x50` script | `rt.script_instance(car_ref)` (the Chassis object) |
| `new GameRef(P)` | `rt.wrap(P, &err)` (runtime.cpp ~979) |
| 0x47dd50 | `rt.call_method(obj, "addpart", {Value::make_obj(ref)}, &rv, &err)` |
| 0x55add0 | `Vm::get_class` (vm.cpp ~1469) |

## Files (<= 3 + vmboot)
- `src/game/runtime.cpp` line ~706: `vm_->find_class(r->script_class)` -> `vm_->get_class(r->script_class)`
  with a `// 0x51fcc0 -> 0x55add0: loads on demand` comment. Nothing else.
- `src/game/car.cpp`: `static void report_install(Runtime &rt, Runtime::Resource *P, bool nested)` =
  0x4ca390's first block + child recursion: if `pp->car_ref && pp->car_ref != P` call the car's
  script `addpart(GameRef(P))` (0x4942b0 first block) and push P on `CarData::part_list` (new
  `std::vector<Runtime::Resource *>`, the `car+0xfc` list; field goes in car.h, see below); then for every
  slot of P with `flags & 2` and an attached child whose Resource resolves (use the same lookup
  `install_search`/`part_attach` already use to go from Object to Resource; do not invent a new map),
  copy `car_ref`, `car_id`, `wheel_id` from P and recurse with `nested = true`. Call it from
  `part_attach` right after `pp->wheel_id = ...` and before `rebuild_compound`/`install`. INVENTED
  comment: direct call instead of the exe's `"add_part %d"` string round-trip (the %d is a 32-bit native
  pointer). Shortcut comments (one line each): the `P+0xac`/render `+0x1b4` copy, add_light/wing/
  rollbar/mslot/linked commands, the queued `"updatevariables"` tail, 0x4942b0's engine-slot and
  wheel-pivot branches, 0x4a8200. Keep `add_part/rem_part` in part_command's shortcut comment (the
  part-side word 0x4c17b0 and rem_part/uninstall 0x4cb510 stay unported).
- `src/game/car.h`: `CarData::part_list` (`std::vector<Runtime::Resource *>`, comment `car+0xfc`).
  Only if CarData is not the right holder (car_ref may be a chassis Resource that maps to CarData via
  `g_cars`): use whatever `g_cars.find(pp->car_ref)` returns.
- vmboot (`src/tools/vmboot.cpp`): checks below.

## vmboot checks (shipped data only)
Keep every existing check (127). INVENTED harness change: the dealer browse (`shown_car_has_engine`,
vmboot.cpp ~150) also requires `Vehicle.isDriveable()` (call it through `rt->call_method` on the shown
car object) to return Null; keep the D18V slot-401 condition. Print each browsed car's isDriveable text.
New hard checks at `kTestDriveCmd` (player car in the Garage, before the test drive):
1. `player.car.chassis.attachedParts.elementCount` == installed parts of the recursive slot walk the
   vmboot already prints (`installed parts on the car (recursive slot walk)` minus the chassis itself,
   see notes/13 §15.1 end), and == `CarData::part_list.size()`. Print all three.
2. Every element of `attachedParts.elementData[0..count)` is a distinct object that `is_a`
   `java.game.parts.Part`, and none is the chassis.
3. `player.car.isDriveable()` returns Null (Vehicle.java:279).
4. At least one element `is_a` `java.game.parts.enginepart.Battery` and one `is_a`
   `java.game.parts.enginepart.Block` (both exist: parts/scripts/enginepart/{Battery,Block}.class).
Print the bought car's dialog price (NoYesDialog text, as dialog OK already reaches it) for the Result.

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, all old checks plus 1-4 pass, 0 failed threads. Result lists counts from check 1,
the price, and the freeride car_root mass line (must stay 1280.294 kg or explain the difference with
the cfg masses of newly installed parts).

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or
synthetic inputs; cite exe addresses; `INVENTED:` for the dealer isDriveable filter and the direct call.
If `get_class` makes `make check` slower than 300 s or changes a class-load corpus count, stop and report.

## Not this card
vmboot career flow (CMD_EVENTLIST 133 + RACING EVENTS hint + eGen CMD_RACE -> CircuitRacing Track),
rem_part / uninstall 0x4cb510, 0x4942b0 engine-slot/wheel branches, 0x4a8200, the queued
`updatevariables`, add_light/add_wing/add_rollbar/add_mslot/add_linked.

## Result
`make -C src clean check` green (exit 0). vmboot: `selfcheck: PASSED (131 checks, 0
failure(s))`; 30000 frames, 1532 threads, **failed: 0** (was 127 checks + 4 new).
- Card 163 check 1: `attachedParts.elementCount` 76 = recursive slot walk 77 - 1
  (the walk reaches the chassis again through the engine's slot-99 back-pointer) =
  `car+0xfc` list 76.
- Checks 2-4 ok: 76 distinct `java.game.parts.Part` elements, none the chassis; one
  `enginepart.Battery` and one `enginepart.Block`; `player.car.isDriveable()` null.
- Bought car: Duhen SunStrip 1.8 DVC, NoYesDialog text price **$20568** (browse shows
  isDriveable texts: no engine / no tyre / missing a battery / null ...).
- Freeride `car_root` mass **1280.294 kg** unchanged (cfg bodies 742.297 + wheels
  40.040 + installed parts 497.957); part_comp 742.297 kg.
- `get_class` on demand: class-load corpus counts unchanged (package tree 335
  classes, 260 resolved; compile on load 635 ok), `make check` well under 300 s.

INVENTED: dealer browse filter now also requires `Vehicle.isDriveable()` null;
`report_install` calls the Chassis `addpart` directly instead of the exe's
`"add_part %d"` queue string round-trip. Shortcut comments cover P+0xac/render
+0x1b4, add_light/wing/rollbar/mslot/linked, the queued `updatevariables`,
0x4942b0's engine-slot/wheel-pivot branches and 0x4a8200.
Note: the four new hard checks read the player car after the 30000-frame loop
(the same `player.car` object the kTestDriveCmd step sees) instead of inside that
frame step; the state compared is unchanged by the test drive.
