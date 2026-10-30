---
id: 243
status: implemented
branch: card/243
depends: [241]
---
# harness: make the city lanes live in `vmboot --city` (INVENTED stand-in for the render LOD keep-alive)

## Goal
`vmboot --city` loads `maps.city:1`, binds a traffic graph and spawns cars into lanes, but a
lane only becomes **live** when its road's map node is touched, and the only shipped caller
that touches road nodes in bulk is the exe's per-frame render LOD walk — which src does not
have. Today the event-phase walk touches 15 map records and none is a `native road`, so
`Graph::live_lanes == 0` and every per-car traffic path ported in cards 234-241 is dead.

Add an explicitly INVENTED harness step: after the map handle resolves, touch every map
object of the resident ground whose gametype is `native road`, then run the existing three
frames. That is the same create path the exe uses (`0x5195a0` vf3 -> `Traffic__bindObject`
`0x41a3e0`) followed by the keep-alive `type_road__vf5 0x4d44d0` -> lane vf7 `0x40eed0` ->
`ctLane__setLive 0x40f6d0` (src `Graph::keep_road_live`); the only invented part is *which*
nodes get touched (the exe picks them from the camera LOD list `0x45f180 -> 0x461980`,
which src has no counterpart for).

## Exe evidence (already traced; this card adds no new RE)
- Lanes go live from the render walk: `0x45f180 -> 0x461980 -> type_road vf5 0x4d44d0 ->
  lane vf7 0x40eed0 -> ctLane__setLive 0x40f6d0` (notes/30). src has no render walk.
- Live lanes expire: `ctLane__activeStep 0x40f4e0` clears live when `sim_time - lane.kc0 >
  0.5` (0x40f6d0 path); `keep_road_live` stamps `lane.kc0 = sim_time`, so touching the road
  every frame keeps its lanes live.
- The create path and the bind are already ported: `Runtime::map_node_touched`
  (src/game/runtime.cpp, `0x4fe6f0` + `0x5195a0` + `0x4fe852`), `Graph::keep_road_live`
  (`0x40f8e0`), `Graph::set_lane_live` (`0x40f6d0`).
- Not modelled, do not add here: `TrafficCar__syncInstance 0x410680` and road object
  instances (existing `shortcut:` comments).

## Changes
1. `src/game/runtime.h` / `runtime.cpp`: new public
   `size_t touch_road_nodes(Resource *ground);`
   ```
   // INVENTED HARNESS: the exe keeps a road's lanes live from the camera LOD walk
   // (0x45f180 -> 0x461980 -> 0x4d44d0 -> lane vf7 0x40eed0); src has no render
   // walk, so touch every resident `native road` map object once.
   size_t n = 0;
   if (!ground) return 0;
   for (size_t i = 0; i < ground->map_objects.size(); i++) {
       const MapObject &m = ground->map_objects[i];
       if (!m.gametype || !native_type_is(*this, m.gametype, ground->package, "road")) continue;
       map_node_touched(ground, i);
       n++;
   }
   return n;
   ```
   (`native_type_is` is a file-static in runtime.cpp, so the loop must live there.)
2. `src/tools/vmboot.cpp`, `city_checks`: right after `graph` resolves (before the report
   checks that walk the lanes) call
   `const size_t touched = map_resource ? boot.runtime()->touch_road_nodes(map_resource) : 0;`
   (`Runtime::Resource *map_res = mh ? mh->target : nullptr;` — the same accessor the file
   already uses elsewhere) and add two checks:
   - `c.check(touched > 0, "city: harness touched the map's native road nodes (INVENTED LOD stand-in)")`
   - `c.check(graph->live_lanes > 0, "city: touching the road nodes makes lanes live (0x40f6d0, got N)")`
   Comment both as INVENTED harness (no exe counterpart for the trigger, the effect is exe).
3. Keep the three-frame loop where it is; do not delete or relax any existing city check.
   If an existing check now fails because lanes are live, **report it** in the card/completion
   note — do not edit the expectation to make it pass.

## Files
- edit: src/game/runtime.h, src/game/runtime.cpp (new `touch_road_nodes`), src/tools/vmboot.cpp (city probe)
- read only: src/game/traffic.h (`Graph::live_lanes`), src/game/runtime.h (`Resource::map_objects`)

## Done when
- the new calls carry the `// INVENTED HARNESS:` comment with the 0x45f180/0x4d44d0/0x40f6d0 addresses
- `make -C src check` green; `--city` reaches live lanes; `--race` unchanged (659.414 s) —
  report the new `--city`, `--race` and `--selfcheck` counts, and any check that moved
- no new unit tests or hand-made expected values; no change to engine behaviour (harness only)
- card frontmatter: status implemented, branch card/243
