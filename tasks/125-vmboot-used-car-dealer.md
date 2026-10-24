---
id: 125
status: implemented
branch: card/125
depends: [124]
---
# vmboot: go from the Garage to the used car dealer and report

## Goal
vmboot (cards 121-124) brings the shipped game into `java.game.Garage` (new career, no car;
the welcome WarningDialog says to visit the Used Car Dealer). Drive one more step: the
Garage's `osdCommand(I)V` with `CMD_BUYCARSUSED` = 122 (Garage.java:87, handler at
Garage.java:1724-) opens the dealer only between 7 and 17 h game time (`GameLogic.getTime()`;
otherwise it warns). The dealer shows vehicles: this runs car/vehicle scripts.

## Changes (src/tools/vmboot.cpp only)
1. Raise kFrames to 12000 (keep everything before frame 6000 as is).
2. At frame 6300: if actualState is `java.game.Garage`, spawn its `osdCommand(I)V` with 122
   as a script thread `THRD-GARAGE-CMD` (same helper as THRD-MENU-SELECT). Comment:
   `// INVENTED: stands in for clicking the Used Car Dealer button`.
   Print `  garage cmd 122 at game hour <GameLogic.getTime()/3600>` before spawning (read
   the static via the VM; if it is a script method, invoke it synchronously only if it
   is a plain getter, otherwise read the static field `time` or whatever getTime returns;
   skip the hour print if unclear).
3. Keep `answer_dialogs` running every 300 frames to the end.
4. After the run print `  final actualState: <class>` (exists) and the thread report.
   If actualState changes away from Garage, add a check that it changed (shipped
   behaviour); otherwise report only.

## Done when
- `make -C src clean check` green; vmboot run stays well under the 300 s make-check cap.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'garage|dialog|actualState|failed|^  frames|^  thread' | head -40`
