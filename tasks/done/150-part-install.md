---
id: 150
status: merged
branch:
depends: [147, 149]
---
# Part install: native "install ..." command attaches created parts to slots

## Why
src never writes `game::Slot::attached`, so `Part.partOnSlot` is null for every slot,
`ch_force_update` (natives.cpp ~3745, slot 401) finds no engine, `Chassis.updatevariables` finds
no `Block`, no DynoData -> CarData torque curve = 1 point of 0 Nm. Full RE: notes/13 §15 (read
it first; it is the reference for this card).

## Script side (shipped, unchanged)
`Vehicle(parent, id, ...)` (sl/Scripts/game/Vehicle.java:107) creates the car (Chassis instance)
and calls `chassis.addStockParts(desc)` -> `Part.addPart` (parts/scripts/Part.java:818) per stock
id: `xa.create(GameRef(rootID), GameRef(id), "", name)` (part in the car's parent), then
`xa.command("install 0 <carID> 0 <this.id()> 0")`; success = `xa.getParentID() != rootID`, then
`part.addStockParts(desc)` (engine block adds crankshaft, heads, ... the same way). Axis_200S:
engine `parts.engines.Einvagen_Duhen_Ishima_Focer:0x9B` (Duhen_D18V_block, slot 99) ->
chassis slot 401 (`_main_200S.cfg:387 attach 0x000A009B 99`).

## Exe functions (5; behaviour to match, notes/13 §15 has the full text)
1. `type_part__vf14` 0x4c4400, code 0x10, word `install` (only this word in this card):
```
n = scan(cmd, "%*s %d %d %d %d %d %f %f %f", &cursor, &dest, &mySlot, &target, &tSlot, &x, &y, &z)
P.dest = dest; tref = target                  // mySlot is parsed but ignored
mySlot' = installSearch(P, tref, &tSlot, n >= 8 ? &pos : null)
if (!mySlot') return 0
attach(P, mySlot', tref, tSlot)               // 0x4cb1d0, steps below
```
2. `Part__installSearch` 0x4c96c0: `if (tref resolves) { r = findMateSlot(P, tref, &tSlot, null);
   if (r) return r }`; else fall back to dest: dest kind (getInfo 7) 5 = vehicle (or 9): try
   `findMateSlot(P, dest, ...)`, for 5 also every part of the car; with pos the nearest target
   slot (squared distance < 1e8), without pos the first hit.
3. `findMateSlot` 0x4c9af0(P, tref, &tSlot, pos):
```
if (target slot count < 1 || tref.id == P.id) return 0
if (tSlot == 0)
  for my slots s in cfg order: r = my runtime slot
    if ((!r.attached || r.flags & 1) && !(r.flags & 0x10))
      c = targetSlotFor(T, myType, s.id, pos)          // 0x4c89f0
      if (c) { tSlot = c; return s.id }                // (pos: nearest instead)
else if (target has slot tSlot, free, !(flags&0x10), target not locked)
  for my slots s with !r.attached && !(r.flags & 0x10):
    if (compatible(T.type, tSlot, myType, s.id)) return s.id
return 0
```
4. `targetSlotFor` 0x4c89f0(T, myType, mySlot, pos): `if (locked(T)) return 0` (0x4cb0b0: an
   occupied slot with `flags & 0x50 == 0x50`, recursing up the attached chain); for target slots t
   in cfg order with `!attached && !(flags & 0x10)`: `if (compatible(T.type, t.id, myType,
   mySlot)) return t.id` (pos: nearest).
5. `compatible` 0x4c9d90(A, slotA, B, slotB) (A/B = part TYPE ids `pkg<<16|local`; sA/sB the
   type slots; `attach`/`compatible` = cfg lines under that slot, entries (part id, slot id)):
```
if (A == B && slotA == slotB) return 0
if (sA.attach has (B, slotB)) return 1
if (sB.attach has (A, slotA)) return 1
if (any sA.attach entry == any sB.compatible entry) return 1
if (any sB.attach entry == any sA.compatible entry) return 1
return 0
```
attach 0x4cb1d0 + commit 0x4c7d20 (port this subset, in order):
- reparent P under dest (0x4ec210): `P->parent = dest`, `parent_id` updated (Part.addPart's
  success test). Use the existing Runtime reparent helper if one exists, else the same fields
  `GameRef.create` sets.
- my runtime slot: `attached = target script object`, `mate_id = tSlot`; target runtime slot:
  `attached = P script object`, `mate_id = mySlot'`.
- bolts = 0 (shortcut: type slot `+0x6c` bolt count not parsed, pending flag 4 path not ported),
  so commit at once: my slot `flags |= 1`, target slot `flags |= 2` (exe: flag 4 cleared),
  `PartData.installed++` on both (`P+0x74`).
- `P.car_ref / car_id` := the target's car (`getInfo(tgt, 0x2b)`; for the chassis target = the
  car itself, for a sub-part = the target's `car_id`). `Part.getCar` must return it afterwards.
- call the part's script method `install()` (0x47dd50, Part.java:707) if it has one.
- return 1 (command consumed).

Not ported (each a `shortcut:` comment): pose/body placement (0x4c1a10/0x4eac50), physics join
and compound mass (0x4c7d20 body block), children re-point 0x4cb6c0, `P+0xb0` mask inherit,
wheel id 0x4c8b10, bolts/pending installs, `remove`/`repair`/`texture`/... words, `install_OK`.

## Files (3 + check)
- src/game/car.h: `Slot` keeps `std::vector<CfgAttach> attaches, compatibles` (part id + slot
  id; today only part ids, `attaches` users: grep and adapt); `PartData::installed`;
  `int part_command(Runtime&, Runtime::Resource *part, uint32_t code, const Value &param)`.
- src/game/car.cpp: `fill_part` copies attaches + compatibles; `part_command` (the 5 functions
  above + attach subset). Slots of the CAR: car records have `native car <cfg>` and no `native
  part`, so today `part_data(car)` has no slots. The exe car manager embeds the part manager
  (0x4c9af0 reads the chassis' slot list from its type manager). Fill the car's PartData slots
  from the car cfg (`load_car_cfg` / `cfg_resource`, also in `create_car_bodies` if the
  PartData was created before `cfg_resource` was set). INVENTED-mark only what is not exe.
  Part instances need `event_mask |= 0x10` like cars (same INVENTED as car.cpp:501) so the
  command reaches the native handler; set it in `create_car_bodies` (the 0x4c1f90 path, top,
  when `has_native_ref(rt, type, "part")`), not in natives.cpp.
- src/game/runtime.cpp `queue_event`: `!game::car_command(...) && !game::part_command(...) &&
  !game::player_command(...)`.
- src/tools/vmboot.cpp: the check below; replace the "No engine part is installed yet (card
  150)" comment and make the existing curve check require max > 0.

Step 0 (before coding, print once in vmboot and delete): does a part instance created by
`GameRef.create` get a PartData with its cfg slots (Duhen_D18V_block: 46, 98, 99, 14, 47, 1, 40
...), and what is the type id of that part (must be 0x000A009B to match the chassis attach)?
If the instance chain does not reach the type record (same issue as `cfg_resource` for cars),
fix it the same way. Write the answer into the card's Result section.

## vmboot check (real data, hard)
After the Garage purchase (player car = Axis, Vehicle ctor ran `addStockParts`):
1. Chassis PartData slot 401: `attached` is an instance of class `...Duhen_D18V_block`
   (VM class name), `mate_id == 99`; the block's slot 99 `attached` = the chassis object,
   `mate_id == 401`.
2. The block resource's parent is the car (Part.addPart success path ran:
   `Part.getCar()` of the block == car id).
3. Sub-part chain: block slot 40 (crankshaft) attached (Duhen_D18V_block.java:99).
4. After `forceUpdate`: `car_payload->torque_nm.size() > 1`, max torque > 0, and the
   existing "drive bridge copies the engine torque curve" check still passes.
5. Count installed parts on the car (walk slots recursively) and print it; no hand-written
   expected number.
All existing vmboot checks stay green, 0 failed threads. Run with tools/vmboot-capped.sh only.

## Rules
- Stop and report after 2 failed vmboot runs on the same check (do not keep patching): write
  what you saw (output lines, which step fails) into Result and set status `blocked`.
- No unit tests or synthetic inputs; cite the exe address next to each ported branch.

## Done when
vmboot hard checks 1-4 pass on the shipped data (Axis bought in the Garage), all old checks
green, build clean.

## Result
