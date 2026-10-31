---
id: 255
status: todo
branch: card/255
depends: [253]
---
# traffic: Traffic__haltPath / Traffic__haltRoad + the two halt natives

## Goal
Wire the two remaining halt natives, which are still stubs in `src/script/natives.cpp`:
`GroundRef.haltTrafficPath 0x4dd700` and `GroundRef.haltTrafficRoad 0x4de270`. They call
`Traffic__haltPath 0x4196f0` and `Traffic__haltRoad 0x419ba0`, which use `Graph::find_route`
(card 253, merged) and propagate the halt through `halt_cross_propagate 0x419980` /
`halt_lane_propagate 0x419ab0` (which call the already-ported `Graph::halt_cross 0x40de70`,
card 250).

**Read the evidence in `tasks/251-traffic-findroute-haltpath-haltroad.md`** — sections
`### 0x4196f0` (line ~208), `### 0x419980` (~236), `### 0x419ab0` (~256), `### 0x419ba0`
(~266), `### 0x4dd700` (~279), `### 0x4de270` (~295) contain the traced pseudocode and the
argument order. `tasks/253` is the merged find_route port; do not modify it.

## Scope and constraints
1. `Graph::halt_path(p1[3], p2[3])` (0x4196f0) and `Graph::halt_road(p1[3], p2[3])`
   (0x419ba0): port the exe body exactly, including the global lane worklist
   (`0x5e6538` data / `0x5e653c` count / `0x5e6540` cap, grown by 0x400 through `0x5679b0`)
   as **graph-local state** (a `std::vector<uint32_t>` on `Graph`) — do not model the exe's
   global with a file static.
2. `halt_cross_propagate(cross)` (0x419980) and `halt_lane_propagate(lane)` (0x419ab0):
   halve the cross with `halt_cross(cross, 0.001f, ...)`, set `lane+0xc4 = 1` where the exe
   does, and expand along link lanes exactly as traced. **Careful with the lane->cross step**:
   the trace records `road node[1 + lane.k18]` here, while `lane_end_move 0x40e460` uses
   `node[2 - k18]` — implement what `0x419980`/`0x419ab0` actually read, cite the address,
   and add an `// INVENTED: k18 guard` only for the out-of-range index the exe would follow
   unchecked.
3. The two natives: `natives.cpp` already has `gr_halt_traffic_path` / `gr_halt_traffic_road`
   bodies returning true with a `SERVICE MISSING` comment. Replace the bodies with the traced
   marshalling (self, Vector3, Vector3 -> `getInfo 0x39` -> `halt_path`/`halt_road`), same
   shape as the `haltTrafficCross` handler next to them. Keep the registry rows untouched.
4. `Road::k64` is now `int32_t` (card 253) — use signed comparisons if you touch it.

## Verification
- `make -C src check` green; report `--city`, `--selfcheck`, `--race` counts and the race
  time (must stay 659.414 s).
- No unit tests. No synthetic programs. Do not change `find_route`, `halt_cross` or the
  traffic step functions.
- If any traced branch cannot be expressed without a guess, stop and report it as a
  `SERVICE MISSING`/`shortcut:` with the address instead of inventing behaviour.

## Done when
- every ported function carries a `// 0xADDR Name` comment, gaps are marked
  `INVENTED:`/`shortcut:`, and the natives no longer say `SERVICE MISSING`
- card frontmatter: status implemented, branch card/255
