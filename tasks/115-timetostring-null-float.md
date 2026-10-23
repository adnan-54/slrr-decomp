---
id: 115
status: todo
branch:
depends: [114]
---
# VM: "String.timeToString line 236: cannot promote null to F" after GameInit

## Goal
`tools/vmboot-capped.sh --selfcheck` now runs the shipped GameInit constructor to
completion, then prints
`vm thread failed: java.lang.String.timeToString line 236: cannot promote null to F`
(some script thread run after the constructor). Find which thread/caller passes or reads a
null where a float is expected and fix the ROOT cause. Likely candidates (check, don't
assume): an uninitialised float static/field/local that the exe types as 0.0 (card 108 did
this for `float y;` locals: see `default_for_desc` in src/script/vm.cpp, also used for
fields/statics), a multi-name declaration `int hour, min;` where only one slot gets its
typed default, or a native returning null instead of a float (e.g. System.simTime,
GameLogic.getTime chain).

## Material
- Shipped class: `bin/the game/system/scripts/lang/String.class` (2.3.1); dump with
  `src/build/classdump "bin/the game/system/scripts/lang/String.class"`.
  `timeToString (FI)` has 236 instructions. 2.2.1 source for reference:
  `bin/2.2.1 sources/system/Scripts/lang/src/String.java` (line numbers differ).
- Game scripts: `bin/the game/sl/Scripts/game/*.java` (callers: grep timeToString).
- Run ONLY capped: `tools/vmboot-capped.sh --selfcheck` (env VMBOOT_TIMEOUT=<s>).
  A temporary env-gated trace in vm.cpp is fine; it must be removed before commit
  (no file writes to /tmp, no always-on output).

## Files
- edit: src/script/vm.cpp (or the native that returns the null, in src/script/natives.cpp)

## Done when
- the failure line is gone or replaced by a later one; report the new last lines of
  `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -v '^  ok' | tail -5`.
- `make -C src clean check` green. Root cause in one paragraph with the script line(s).
- Anything not confirmable from code: `INVENTED:` + say so.
