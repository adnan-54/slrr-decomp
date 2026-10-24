---
id: 127
status: todo
branch:
depends: [126]
---
# vmboot: from the Garage (with a car) into freeride (Track) and report

## Goal
Card 126 leaves the shipped game in `java.game.Garage` with `player.car` set. Two more
table steps (use the card-126 step table in src/tools/vmboot.cpp):
- Garage cmd 134 = `CMD_TESTDRIVE` (Garage.java:99, handler ~1598) ->
  `GameLogic.changeActiveSection(new EventList(EventList.MODE_FREERIDE))`.
- EventList cmd 0x10E = `CMD_RACE` (EventList.java:52, handler ~537): in freeride mode
  `freerideSetup()` then `changeActiveSection(new Track(...))` = the city/track with the car.

## Changes (src/tools/vmboot.cpp only)
1. Steps: frame 18300 Garage cmd 134; frame 20000 EventList cmd 0x10E. Raise kFrames to
   30000 (check vmboot still runs well under the 300 s cap; report the wall time).
2. After the run print `  final actualState: <class>` (exists) and add a check that
   actualState is `java.game.Track` ONLY if it got there. Also print the count of
   `native not implemented` failures if any thread failed with it (thread report shows it).
3. If a thread fails, do NOT patch around it in vmboot; just report it (Claude fixes VM/
   native bugs).

## Done when
- `make -C src clean check` green.
- Report: `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E 'cmd|actualState|failed|not implemented|^  frames|^  thread' | head -40`
  and the wall time of `time tools/vmboot-capped.sh --selfcheck`.
