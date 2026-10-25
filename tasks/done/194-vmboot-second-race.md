---
id: 194
status: merged
branch: card/194
depends: [193]
---
# vmboot: second career race from the EventList after the first finish

## Goal
Exercise the exe's Track exit -> re-enter path once: after the `--race` finish key the section is
java.game.EventList (Track.getOut). Start the next race through the same Generator command the
first race used and run a short block. A scratch probe (notes/18 s9, line 287) showed this already
works with 0 failed threads (`Generator.osdCommand(0x10E)` -> java.game.Track within 500 frames,
world roots 54+10 vs race 1 51+9 before card 193). This card turns it into checks so Track.exit
cleanup (Track.java:1074-1160: destroyBots, releaseCar, map.unload, remSceneElements) and a
second Track.enter stay covered. No engine changes.

## Script / exe facts used
- EventList.java:22,44,345,537-545: `eGen` (Generator) owns CMD_RACE 0x10E; its osdCommand
  starts the career Gamemode setup and switches to java.game.Track.
- vmboot already does this for race 1 (`kRaceCmd` block, ~src/tools/vmboot.cpp:766: reads
  `EventList.eGen`, `osd_command(boot, gen, 0x10E, "THRD-EVENT-RACE", ...)`). If card 192 is
  merged, use its click queue / `find_gadget_pos` instead (cursor click on the Generator race
  button, fallback osd_command) -- same as race 1.
- Track exit removes the bot cars (card 193 check) and unloads the map ground (Runtime::unload
  ground_body, type_ground vf4 0x4bb3d0).

## Steps
1. In the `--race` branch after the finish-key checks (~vmboot.cpp:3420, uses the local
   `frames(n)` lambda): run 1200 frames (the EventList settles; INVENTED wait), then send the race
   command to `EventList.eGen` as race 1 does; print `  second race: cmd 0x10E -> <class>`.
2. Run up to 3000 frames, stopping at the first frame where `actual_state` is
   `java.game.Track` and then 500 more frames (INVENTED budget). Print the frame count, the
   active section, world roots (`process_world().bodies.size()`, `.sleeping.size()`), and the
   number of native bot payloads (`game::car_payloads()` entries with `root_body != nullptr`,
   or the `native bots:` helper used for race 1 if it is reusable without copying).
3. Checks (facts of this run, no hand-made numbers):
   - `actualState` is `java.game.Track` after the second race command
   - no failed thread over the second-race block (thread_report errors == 0)
   - the second Track has a map ground again: `process_world().statics` holds a body with
     `def.shape.type == SHAPE_MESH && def.shape.mesh` (the lookup drive_cars uses, car.cpp:1733;
     probe: statics 2 in race 1, 1 in the EventList)
4. Keep the whole `--race` run inside the Makefile's 300 s timeout (`time make -C src check`
   once; if it goes over, shorten step 2's tail, never raise the timeout).

## Files
- edit: src/tools/vmboot.cpp (only the `--race` finish block and, if needed, a tiny helper)
- read only: src/game/runtime.h, src/game/car.h (car_payloads)

## Done when
- three checks above pass; INVENTED on the frame budgets
- `make -C src clean check` green in both vmboot modes, `--race` under 300 s (report the time)
- card frontmatter: status implemented, branch name
