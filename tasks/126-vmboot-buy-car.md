---
id: 126
status: implemented
branch: card/126
depends: [125]
---
# vmboot: buy a car at the used dealer and return to the Garage

## Goal
Card 125 brings the shipped game into `java.game.CarMarket` (used dealer). Buy the shown car
and go back, so the Garage has a player car (car/part/chassis scripts run on it).
CarMarket.osdCommand (CarMarket.java:817-): `Input.AXIS_SELECT` (34) in browse mode calls
`buyCar()` (line 1013), which asks a `NoYesDialog` "BUY CAR" (`display() == 0` buys);
`Input.AXIS_CANCEL` (35) does `GameLogic.changeActiveSection(parentState)` (the Garage).
Values 34/35 are the Input.class statics.

## Changes (src/tools/vmboot.cpp only)
1. Refactor the per-step drivers into one table-driven helper if that makes it shorter:
   `{frame, expected actualState class, command}`; at `frame`, if actualState's class equals
   the expected one, spawn `osdCommand(I)V` on the object that receives commands for that
   state (Garage/CarMarket: actualState itself; MainMenu: its `window`, as card 122 found)
   as a script thread, and print `  cmd <n> -> <class> (frame F)`. Keep existing behaviour
   and checks of cards 121-125.
2. New steps: frame 12300 CarMarket cmd 34 (buy; answer_dialogs answers the NoYesDialog
   with osdCommand(0) as today: check that 0 means "yes/OK" for NoYesDialog in
   `bin/2.2.1 sources/system/Scripts/render/osd/dialog/src/*.java` or the 2.3.1 class; if
   OK is a different command id for NoYesDialog, send that one for that class), then
   frame 14000 CarMarket cmd 35 (back). Raise kFrames to 18000.
3. Probe after the run: `GameLogic.player.car` non-null? Print
   `  player car: <class or null>` and `  final actualState: <class>`. Add checks only for
   things that happened (actualState back to Garage; player.car non-null) - shipped behaviour.

## Done when
- `make -C src clean check` green, vmboot well under the 300 s cap.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'cmd|player car|dialog|actualState|failed|^  frames|^  thread' | head -40`
