---
id: 155
status: todo
branch: card/155
depends: [150, 153, 154]
---
# Part install failures: report per car, prove each one is an exe failure, bolt corpus check

## Why (RE 2026-10-24, notes/13 §15.1)
CHECKPOINT listed "55 failed part installs (parts missing a host part)" as a bug. A scratch run with
a print in `part_command` shows all 55 are failures the exe has too, and the freeride SunStrip 1.8
DVC (the vmboot car) has 75 ok / 0 failed installs:
- 52 = 4 Whisper_Q1000XL cars x 13: `V12_block_racing` re-adds parts its sub-parts already added
  (flywheel, rods, pistons, camshafts, bearing bridges, head covers, exhaust headers); the only
  compatible slot is occupied.
- 3 = Axis_200XT used car: R wing before its trunk (list F before Rr), FL window and L mirror 2
  whose FL door was not rolled (`random() <= desc.optical`, Chassis.java:938).
Bolts: no shipped cfg has a `bolt` line, so installs never go pending (0x4bfd00 / 0x4c1a10 below)
and src's commit-at-once is exe-exact. Nothing in the install search changes. This card turns the
finding into hard real-data checks so a future regression (a failure the exe would not have)
shows up, and fixes the off-by-one in the vmboot installed-parts walk.

## Exe functions
`0x4bfd00` cfg keyword `bolt` (table 0x5da5a8): on the last-declared type slot S (`M+0x1194`):
new 0x70-byte `slot_bolt` linked into `S+0x98/+0xa0`; `S+0x6c += 1`; scan `"%f %f %f %f %f %f %d"`.

`0x4c1a10` (called by Part__attach 0x4cb1d0) returns `targetTypeSlot+0x6c + myTypeSlot+0x6c`;
0x4cb1d0 commits (0x4c7d20) only when that sum is 0.

`0x4c96c0` / `0x4c9af0` / `0x4c89f0` / `0x4c9d90`: the search as in notes/13 §15 (already in
src `install_search` / `find_mate_slot` / `target_slot_for` / `part_compatible`). An install
fails in the exe iff, at that moment, neither the target part, nor the car, nor any part on the
car (`getInfo(car, 0x31)`) has a slot that is free (`+8 == 0`), not disabled (`!(flags & 0x10)`),
on an unlocked part (0x4cb0b0) and compatible (0x4c9d90) with one of my free slots.

## Files
- src/game/car.h/.cpp: record every `part_command` install in a list readable by vmboot:
  `struct InstallRecord { Runtime::Resource *part, *target, *dest; uint32_t part_type, target_type;
  bool ok; bool free_compatible; }` and `const std::vector<InstallRecord> &install_log()`.
  `free_compatible` is computed on failure only: true when any resource in `{target, dest} ∪
  dest->children` (skip `part` itself) has a slot `t` and the part a slot `s` with `!t.attached &&
  !(t.flags & 0x10) && !part_locked(T) && !s.attached && !(s.flags & 0x10) &&
  part_compatible(T, t.id, part, s.id)`. Use the existing helpers; this is a report, it must not
  change search results. `INVENTED:` comment on the log (debug aid, no exe counterpart).
- src/tools/vmboot.cpp:
  1. installed-parts walk: insert `car_res` into `seen` before the loop (today it counts the
     chassis as its own part: prints 76, real 75).
  2. Print per car (group by `dest`): chassis type name, ok count, failed count, and one line per
     failure `part type -> target type (no free compatible slot | FREE SLOT)`.
  3. Bolt corpus: scan every `.cfg` under the game dir (fs::find or the existing cfg corpus walk)
     for a line whose first token is `bolt`; print the count.

## Done when (`VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh --selfcheck`, real data)
a. Hard check: the vmboot player car has 0 failed installs, and its ok-install count equals the
   installed-parts walk count (expect 75 = 75).
b. Hard check: no failed install anywhere has `free_compatible == true` (the exe would have
   installed it). Print the totals (expect 834 installs, 55 failed: 52 on Whisper_Q1000XL cars,
   3 on Axis_200XT; the counts are printed, not asserted, since they follow the script RNG).
c. Hard check: 0 shipped cfgs contain a `bolt` line (0x4bfd00 never runs, 0x4c1a10 returns 0,
   every install commits at once).
d. Mass check unchanged and green: car_root mass == cfg bodies + wheels + cfg masses of the
   installed parts (currently 1280.294 kg).
e. All older checks green, 0 failed threads; `make -C src check` green.

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; do not change install_search /
find_mate_slot / part_compatible behaviour (if b fails, report the failing line: it is an RE
finding, not something to patch around).

## Result
