---
id: 120
status: todo
branch:
depends: [119]
---
# VM: "java.lang.Thread.methodStatus line 157: null receiver" (MainMenu animation thread)

## Goal
`tools/vmboot-capped.sh --selfcheck` (6000 simulated frames after GameInit) ends with
`vm thread failed: java.lang.Thread.methodStatus line 157: null receiver`.
Callers: `bin/the game/sl/Scripts/game/MainMenu.java:618` (`mmaThread.methodStatus(0)`),
HallOfFame.java:167. methodStatus is a SCRIPT method of the 2.3.1 `Thread.class`
(no .java ships; 2.2.1 `bin/2.2.1 sources/system/Scripts/lang/src/Thread.java` is older
and lacks it). Find what is null at line 157 (likely an instance field such as a status
array set up by `Thread(Runnable,String,int)` (body 19) / `identifyThread` / `inject`,
or a field initializer the VM did not run) and fix the ROOT cause in the VM or natives.
Do not paper over it with a null check.

## Material
- `src/build/classdump "bin/the game/system/scripts/lang/Thread.class"` (constants,
  method table) and `src/build/classdump --disasm ...` (per-body op/operand; the
  constant pool numbers map operands to members/strings). Find how mmaThread is created in
  MainMenu.java.
- Run ONLY capped: `tools/vmboot-capped.sh --selfcheck` (VMBOOT_TIMEOUT=<s>). Temporary
  debug prints are fine but must be removed before committing; no files left in the repo.

## Files
- edit: src/script/vm.cpp and/or src/script/natives.cpp (Thread natives)

## Done when
- the failure is gone or replaced by a later one; report
  `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'failed|^  frames'`.
- `make -C src clean check` green. Root cause in one paragraph; untraced logic `INVENTED:`.
