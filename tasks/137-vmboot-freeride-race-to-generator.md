---
id: 137
status: todo
branch: card/137
depends: [136]
---
# vmboot: send the freeride CMD_RACE to the EventList's Generator, hard-check java.game.Track

## Why (vmboot 2026-10-24, 30000 frames, after card 136)
0 failed threads, but `freeride: not reached (actualState java.game.EventList after EventList
cmd 0x10E from java.game.EventList)`. THRD-EVENT-RACE ends `done` doing nothing: vmboot
calls `osdCommand(0x10E)` on the `EventList` object, which has no osdCommand of its own, so
`find_method` resolves `java.lang.Object.osdCommand(I)V` (empty body in shipped Object.class).
This is a driver bug, the VM is right. Full RE: notes/18 §5.

## Evidence (shipped scripts, no exe code involved)
- `sl/Scripts/game/EventList.java:14-42`: `class EventList implements GameState { int mode;
  Generator eGen; enter(){ eGen = new Generator(mode); eGen.myPrevState = prevState;
  eGen.show(); } ... }`.
- `EventList.java:44-52,512-546`: `class Generator extends Dialog`, `CMD_RACE = 0x10E`,
  `osdCommand(int)` case CMD_RACE -> `freerideSetup()` (freeride) ->
  `GameLogic.changeActiveSection(new Track(cEvent.careerGM, cEvent, cEvent.track))`.
- `EventList.java:345`: `button_race = button_gorace.addItem("", CMD_RACE)`, created while
  `osd.globalHandler = this` (Generator ctor); Osd buttons take `globalHandler` as their
  target (2.2.1 `Osd.java:85,1052`). The R hotkey (`:156`) also targets the Generator.
- `GameState.java`: empty interface.
- Probe already done (local patch, reverted): with the receiver = `EventList.eGen`,
  actualState is `java.game.Track` from frame 20001, Track.game `java.game.Freeride`,
  Track.map `java.util.resource.GroundRef`, still 0 failed threads.

## Files (1)
- src/tools/vmboot.cpp only (the `i == kRaceCmd` block ~line 480 and the result check ~563).

## How
1. In the `kRaceCmd` block: when actualState is a `java.game.EventList`, read its instance
   field `eGen` (`get_instance_field`); if it is a non-null object, call
   `osd_command(boot, eGen, 0x10E, "THRD-EVENT-RACE", &rerr)`; otherwise print
   `eventlist cmd 0x10E: EventList.eGen is null` (no fallback to the EventList). Update the
   comment: EventList.java:44,345 Generator owns CMD_RACE; keep the existing `INVENTED:`
   line (stands in for clicking TAKE A RIDE).
2. Replace the soft `if (class_after == "java.game.Track") ... else printf("freeride: not
   reached ...")` with hard checks (all on the final actualState):
   - `testdrive -> race reaches java.game.Track (got <class>)`;
   - its field `game` is a `java.game.Freeride` (Track.java:291-297 `game = gm`, freeride
     event's careerGM);
   - its field `map` is a non-null `java.util.resource.GroundRef` (Track.java:296).
   Keep printing `final actualState:`. Drop the card-127 comment that made it soft.
3. Nothing else: no VM/native change, no new INVENTED logic.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`,
  `final actualState: java.game.Track`, the 3 new checks ok, still `failed: 0`. Report the
  new `thread ...: stopped` lines (expect a `java.game.Track<..>` thread waiting in
  `GameType.run line 157`). If a thread fails inside Track, report it, do not patch around it.

## Next (not this card)
The car still does not move: `Game__physicsTick` 0x47e050 sub-steps `PhysicsWorld__step`
0x502060; src `advance_sim` ports only the clock, and there is no process-wide physics world
nor native ground object (`type_ground__vf3` 0x4bade0, notes/19). That is the next RE target.
