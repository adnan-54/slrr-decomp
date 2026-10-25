---
id: 124
status: merged
branch:
depends: [123]
---
# vmboot: answer every waiting modal dialog with OK, then report

## Goal
Card 123 answers the NEW CAREER name dialog and the game enters `java.game.Garage`. The
Garage then opens another modal dialog (THRD-MENU-SELECT waits in
`java.render.osd.dialog.Dialog.display` line 390 again, likely a hint/warning dialog).
Generalise the 123 driver so the shipped game keeps going.

## Changes (src/tools/vmboot.cpp only)
1. Factor the 123 code into `answer_dialogs(...)`: for every thread in
   `natives::thread_report` that is waiting and whose `wait_on` object's class is or
   extends `java.render.osd.dialog.Dialog`: if it is (or extends)
   `StringRequesterDialog` and its `input` field is null/empty, set `input` = "vmboot";
   then spawn its `osdCommand(I)V` with 0 (OK) as a script thread named `THRD-DIALOG-OK`.
   Answer each dialog object at most once (remember answered Object pointers).
   Print one line per answered dialog: `  dialog OK: <class> (frame N)`.
2. Call `answer_dialogs` every 300 frames from frame 4200 to the end (6000), instead of the
   single 123 call (keep 123's printed lines/check working: the check that actualState
   becomes java.game.Garage must still pass).
3. After the run print `  final actualState: <class>` (reuse the existing helper).
   No new pass/fail check.

## Done when
- `make -C src clean check` green.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'dialog|actualState|failed|^  frames|^  thread' | head -40`
