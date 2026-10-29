# 38 — City drive probe: player car wakes and falls; the city is two packages (2026-10-29)

Follow-up of notes/37 §0 ("no lane goes live in `vmboot --city`"). The probe was a
temporary harness block in `vmboot --city`: 300 frames with Up held (DIK 0xc8, same key
path as the race checks, `0x498599`), then a dump of the live lists, the destroyed
map records and the world bodies. Prints were reverted, not committed.

## 1. Observed (`vmboot --city`, 63 checks still pass)
- The player car body (mass 1280.3) **does wake**: it is absent from `World::bodies`
  until the throttle frames, then listed awake (`asleep 0`).
- It starts at `posGarage[0]` = (-278.5, 9.8, 1033.0) and **free-falls**: y 7.6 at frame
  50, -1.4 at 100, -16.6 at 150, -38.1 at 200, -65.7 at 250, -99.5 at 300; |v| 46.1 m/s.
  No ground contact at all.
- The city ground body exists: 197,286 triangles, 58,210 map nodes, bounds
  (-1470.6, -27.3, -1928.7) .. (1484.6, 70.0, **328.5**). The start point z = 1033 is
  outside every triangle.
- The event-phase walk runs and touches records: 15 map records got `created`, but only
  node 0 (record 0x1, the ground root: `gametype 0x10035`, params naming
  `maps\city\meshes\city.scm` + `maps\city\ai\city.trc`) has a gametype; the other 14
  are gametype-0 records (no create). **No `native road` record is touched** → 0 road
  objects created, 0 lanes live, `live_lanes` 0, `live_count` 0, and the eq / light /
  live-car lists stay empty.
- `World::statics` in the city run holds two grounds: the city soup above and a leftover
  32,820-triangle race-track ground (bounds ±14 x, 0..33 y, ±6.7 z).

## 2. Cause of the fall: the city ships as two packages
`bin/the game/maps/city.rpk` has a package-table entry `maps\city2.rpk` (string at file
offset 0x99), and `bin/the game/maps/city2.rpk` exists on disk (36.3 MB). The rest of the
city (z > 328, which includes the garage forecourt start) is in `city2`. src mounts only
`maps.city:1` (the record behind `new GroundRef(maps.city:0x00000001r)`, Valocity.java:30)
and builds one soup from it, so the far half has no collision.

Not traced yet: how the exe mounts `city2` and whether the far half is a second ground
record (0x519940 instance blob) or streamed nodes of the same tree (notes/19 §streaming,
0x4fe6f0 walk + node residency). Until that is answered the garage start cannot rest, so
the keep-alive never fires in a harness and cards 232/233 stay dead code everywhere.

## 3. Consequence for the traffic work
The four `Graph::step` walks are now ported (232 eq/light, 233 live cars, 219/220 lanes)
and exe-checked, but nothing observes them at runtime. The cheapest next experiment is to
place the player car inside the city soup (e.g. at a `native road` node found in the
`z <= 328` half) and hold throttle: the car should rest, the walk should touch road nodes,
`keep_road_live` should fire and lanes should go live. That is an INVENTED harness
fixture, so it needs no exe change; the real fix is mounting `city2` like the exe.

## 4. Numbers
`--city` 63 checks, sim_time 475.396790; `--race` 165 checks, race 659.414 s. Neither
moved (the probe was reverted).
