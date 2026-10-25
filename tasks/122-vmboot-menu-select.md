---
id: 122
status: merged
branch: card/122
depends: [121]
---
# vmboot: select the current main-menu item (CMD_MENU_SELECT) and report

## Goal
Card 121 opens the shipped main menu (Enter at frame 3000, showMenu 0 -> 2). The menu items
(a slider: NEW CAREER, FREERIDE, ... ; MainMenu.java:281-) are activated by mouse only:
arrow buttons send CMD_MENU_LEFT/RIGHT (0xB005/0xB006) and an invisible "ghost" button
sends CMD_MENU_SELECT (0xB007) (MainMenu.java:132-145); MainMenu.osdCommand (line ~607)
then runs `slider.execute(Osd.CMD_MENU_SEL)`. src has no render-side Osd hit test, so
vmboot calls the shipped script method directly, as the click would.

## Changes (src/tools/vmboot.cpp only)
1. After the Enter press/release of card 121 and some frames (e.g. at frame 3600), find the
   active MainMenu script object (the same way menu_probe reads
   `GameLogic.actualState`), and invoke its instance method `osdCommand(I)V` with
   0xB007 through the VM (spawn it as a script thread with the existing natives helpers,
   e.g. the way Boot/Runtime start THRD-CREATE threads, so modal dialogs that sleep/wait
   inside it work; do not run it synchronously with Vm::invoke if it can block).
   Comment: `// INVENTED: stands in for the Osd ghost-button click (no render hit test)`.
2. Keep running frames to 6000. Print, after the run:
   `  menu select: actualState <class before> -> <class after>` (class name of
   GameLogic.actualState) and keep the existing thread report.
3. No new pass/fail check (report only), unless the actualState class changes; then add
   a check that it changed (it is shipped-script behaviour).

## Done when
- `make -C src clean check` green.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'menu|failed|^  frames|^  thread' | grep -v 'done$' | head -20`
