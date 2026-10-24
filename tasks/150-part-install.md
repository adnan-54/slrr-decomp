---
id: 150
status: blocked
branch:
depends: [147]
---
# Part install: "install ..." command attaches created parts to the car's slots (RE NEEDED)

Not a Flash card yet: Claude must trace the exe first and rewrite this card.

## Why
src never writes `game::Slot::attached` (grep: no assignment anywhere), so `Part.partOnSlot`
returns null for every slot. `Chassis.updatevariables` (Chassis.java:640-675) finds its engine via
`partOnSlot(slotID)` and `instanceof Block`, so no engine -> `engine.updatevariables()` never runs
-> no DynoData -> CarData torque curve = 1 point of 0 Nm (seen in vmboot, 2026-10-24). Card 148's
"car moves under throttle" needs this.

## Script side (shipped)
`Part.addPart` (parts/scripts/Part.java:818): `xa.create(GameRef(rootID), GameRef(id), "", name)`,
then `xa.command("install 0 " + carID + " 0 " + this.id() + " 0")`; success = parent changed
(`xa.getParentID() != rootID`), then `part.addStockParts(desc)` recursively. Axis_200S.java:55
lists the stock engine (`parts.engines.Einvagen_Duhen_Ishima_Focer:0x9B`); chassis slot 401 =
engine (`_main_200S.cfg:387`).

## Exe leads (untraced)
- string "install" refs: 0x4c44d6 and 0x4c8186 (part type command chain, likely type_part vf14);
  0x59b8e4 (third copy, refs not checked).
- `Part__installSearch` 0x4c96c0 / `install_OK` 0x4cd3a0 (notes/13 §12), `Vehicle__onPartAttached`
  0x4942b0 (CHECKPOINT: part-attach path 0x4a2e60 -> 0x4942b0, not carded), installed-part list
  `car+0xdc`, link records 0x34 bytes (notes/13).
To trace: the install command parse, slot match (slot id / mate id / position), reparent to the
car, the attach callback, and what the car records (e+0xc30 engine pointer, part list).
