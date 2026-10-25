---
id: 211
status: todo
branch:
depends: []
---
# vmboot: `--city` mode, Garage "Go driving in the city" -> Valocity

## Goal
Add a third harness mode that runs the existing selfcheck boot (main menu -> new career ->
used dealer buy -> back in the Garage) and, at the frame where it now clicks garage cmd 133
(EventList), clicks **garage cmd 109** instead, so the shipped Garage handler enters
`java.game.Valocity` (the City free-ride, career mode). Then run the City checks below and stop,
skipping every race/Track check that follows in `selfcheck()`. Verified by a scratch run
(notes/28 §2): Valocity is reached with 0 failed threads.

## Script evidence (shipped)
- Garage.java:74 `CMD_HITTHESTREET = 109`; :536 menu item "Go driving in the city";
  :1693-1700 `if (player.car && !player.car.isDriveable()) changeActiveSection(new Valocity())`.
- Valocity.java:30 `map = new GroundRef(maps.city:0x00000001r)`; :41 `Integrator.isCity = 1`.
- 0x4bade0 type_ground create: the map's `.trc` -> Traffic__load 0x41a000 -> Traffic__init 0x419da0
  (src: `Runtime::ground_traffic(handle)` builds it at the first lookup, runtime.cpp).

## Files
- edit: src/tools/vmboot.cpp only.
  - `main` (end of file): accept `--city`; add `bool g_city_mode` next to `g_race_mode` (line ~644)
    with an `INVENTED harness switch` comment; usage string `--selfcheck | --race | --city`.
  - the garage-cmd-133 click (search `"garage cmd 133"`, ~line 825): push cmd 109 and name the
    step "garage cmd 109" / thread "THRD-GARAGE-CITY" in city mode.
  - right after the existing check `"buying the shown used car sets GameLogic.player.car"`
    (~line 896): in city mode run the checks below, print the thread summary the same way the
    end of `selfcheck()` does if that is a small call (otherwise skip it), and return from the
    function so none of the Track/race checks run. Read the surrounding code to see how the
    function returns its result.
- edit: src/Makefile: add a third line under the two vmboot lines (line ~75-76):
  `cd .. && ulimit -v 1500000 && timeout 300 src/$(BUILD)/vmboot --city`.

## City checks (all values from shipped data or the live run, none hand-typed)
1. final `actualState` class is `java.game.Valocity` (`object_class`, as the code does now).
2. `actualState.map` is a `java.util.resource.GroundRef` (same field read as the
   "Track.map is a non-null GroundRef" check, ~line 1176).
3. its handle (`boot.runtime()->handle(map_object)`) gives a non-null
   `boot.runtime()->ground_traffic(h)` graph `G`.
4. load `bin/the game/maps/city/ai/city.trc` directly into a fresh `traffic::Graph` with
   `Graph::load` (read the file with the same helper resman.cpp:430-445 uses, or plain fread) and
   check `G` has the same `cross_eq`, `cross_light`, `roads`, `lanes` sizes.
5. Traffic__init ran on `G`: `G.road_enabled.size() == max(0, max over roads (int32)k64) + 1`,
   `G.road_enabled[0] == 0`, every other entry 1 (0x419da0), and `G.max_road_width > 0`.
6. no failed script thread (reuse the existing failed-thread count if reachable; else skip).

## Done when
- `make -C src clean check` green, including the new `--city` line (it must stay under the
  300 s cap; the scratch run took ~2 min including the race checks it now skips).
- `--selfcheck` and `--race` output unchanged.
- harness-only logic marked `INVENTED:`; no unit tests, no hand-typed expected numbers.
- card frontmatter: status implemented, branch name.
