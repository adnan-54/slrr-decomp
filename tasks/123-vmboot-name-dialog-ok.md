---
id: 123
status: merged
branch:
depends: [122]
---
# vmboot: answer the NEW CAREER name dialog (input + OK) and report the next section

## Goal
Card 122 selects NEW CAREER; the shipped handler (MainMenu.java:417) opens
`StringRequesterDialog(..., "Enter player name", "")` and `THRD-MENU-SELECT` waits in
`Dialog.display` line 390. In the game the native Osd string gadget writes the typed text
to `osd.stringValue` and fires osdCommand(12345); StringRequesterDialog (2.2.1 source:
`bin/2.2.1 sources/system/Scripts/render/osd/dialog/src/StringRequesterDialog.java`) copies
it to its `input` field, and osdCommand(0) is OK (refused while input is empty with
SIF_NOEMPTY). Drive that from vmboot so display() returns 0 and the handler continues to
`GameLogic.changeActiveSection(GameLogic.garage)`.

## Changes
1. `src/script/natives.h/.cpp`: add `Object *wait_on = nullptr;` to `ThreadInfo` and fill it
   in `thread_report` from the scheduler's per-thread monitor object (ScriptThreadState
   `wait_on`, used by Object.wait) when the thread is waiting.
2. `src/tools/vmboot.cpp`: at a frame after the select (e.g. 4200): find the thread named
   `THRD-MENU-SELECT`; if it waits, take its `wait_on` object. If that object's class is (or
   extends) `java.render.osd.dialog.StringRequesterDialog`, set its instance field `input`
   to the String "vmboot" (Vm instance-field setter; find the existing API in vm.h) and
   spawn its `osdCommand(I)V` with 0 as a script thread (same way card 122 spawns
   `THRD-MENU-SELECT`, name `THRD-DIALOG-OK`). If wait_on is some other object, print its
   class and do nothing (report only).
   Comment: `// INVENTED: stands in for typing into the Osd string gadget + OK`.
3. Print `  name dialog: <class of wait_on> -> sent OK` (or the reason it was skipped) and
   keep the `menu select: actualState A -> B` line (it should now change to the garage
   section class if the handler completes).
4. If actualState changes class, add a check that it did (shipped-script behaviour).

## Done when
- `make -C src clean check` green.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'menu|dialog|failed|^  frames|^  thread' | head -30`
