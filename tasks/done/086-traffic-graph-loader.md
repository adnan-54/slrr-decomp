---
id: 086
status: merged
branch: card/086
depends: []
---
# game/traffic: `.trc` road graph loader + shipped-file check

## Goal
Port the traffic graph file loader `Traffic__load` (0x41a000) and its record readers so
`maps/city/ai/city.trc` (the only shipped `.trc`) loads into a plain data model:
crosses (equal-priority and traffic-light), roads, lanes, each with Hermite segment paths.
No behaviour yet (card 087 binds it to the GroundRef natives). Add a `make check` step
that parses the shipped file. Full trace: notes/19 §2.

## Exe evidence (Claude-traced; grammar verified by parsing all of city.trc to EOF)
Byte reader: every field is `memcpy` from the buffer and the cursor advances
(`FUN_0056db00` = memcpy). Little endian, no padding, no bounds checks in the exe.

### 0x41a000 Traffic__load(buf) — ECX = Traffic
    version(+0xc) = u32;
    nEq = u32 (+0x24); nLight = u32 (+0x3c); nRoad = u32 (+0x54); nLane = u32 (+0x6c);
    // ChunkArray__setPageSize(count) 0x414fc0: page = count ? count : 1 (layout only)
    // one dummy element per array is allocated then dropped (count reset to 0) — layout only
    table = new ptr[nEq + nLight + nRoad + nLane + 1]; table[0] = 0;
    table[1..] = &crossEq[0..], &crossLight[0..], &road[0..], &lane[0..]   (in that order)
    for i < nEq:    e = alloc ctCrossEq (0x415470);    ctCross__load(e, &cur, table)   // 0x415b30 = jmp 0x40e850
    for i < nLight: e = alloc ctCrossLight (0x4154b0); ctCross__load(e, &cur, table)
    for i < nRoad:  e = alloc ctRoad (0x40c6d0);       ctRoad__load(e, &cur, table)    // 0x40eb20
    for i < nLane:  e = alloc ctLane (0x40c610);       ctLane__load(e, &cur, table)    // 0x40e9b0
    free(table);
A node reference in the file is a u32 index `k` and the loader stores `table[k]` (0 =
null). Index 1..nEq = crossEq, then crossLight, road, lane.

### 0x40e850 ctCross__load(cur, table) — ECX = cross
    nLink(+0x18) = u32; links(+0x14) = realloc(nLink * 8);
    for i: links[i].node = table[u32]; links[i].flag = u8;           // 5 file bytes each
    nPath(+0x20) = u32; paths(+0x1c) = realloc(nPath * 0x60);
    for i: p = paths + i*0x60: p+0 = u8; p+1 = u8; p+2 = u16; p+0x58 = u32; p+0x5c = u32;
           ctSegment__load(p + 4, cur)                                  // 0x414d50 (ECX = p+4)
    pos(+0x24,+0x28,+0x2c) = f32 x3; +0x30 = u32;
### 0x40eb20 ctRoad__load(cur, table) — ECX = road
    +4 = table[u32]; +8 = table[u32]; +0xc = table[u32];
    +0x10..+0x18 = f32 x3; +0x1c..+0x24 = f32 x3; +0x64 = u32;
    ctPath__load(road + 0x28, cur)                                      // 0x414b80
### 0x40e9b0 ctLane__load(cur, table) — ECX = lane
    +0x10 = table[u32]; +0x14 = table[u32]; +0x18 = u8;
    +0x1c..+0x24 = f32 x3; +0x28..+0x30 = f32 x3;
    +0x34 = u32; +0x38 = u32; +0x78 = f32; +0x7c = f32; +0x80 = f32; +0x84 = u32 (raw, not a table lookup);
    +0x88 = table[u32]; +0x8c = table[u32]; +0x90 = table[u32]; +0x94 = table[u32];
    +0x98 = u32; +0x9c = u32; +0xa0 = u32;
    ctPath__load(lane + 0x3c, cur)
`+0x78/+0x7c/+0x80/+0x84` form an x-sorted lane index (entry i -> lane `+0x84`; min x,
running max of max x, max x — notes/19 §2), used by card 090.
### 0x414b80 ctPath__load(cur) — ECX = path (0x3c bytes)
    n(+0) = u32; segs(+4) = realloc(n * 0x54); total(+0x38) = 0;
    for i < n: ctSegment__load(segs + i*0x54, cur); total = segs[i].len + total;   // float add, in order
    start(+8..+0x10)   = segs[0].p0;      startDir(+0x20..+0x28) = segs[0].m0;
    end(+0x14..+0x1c)  = segs[n-1].p1;
    endDir(+0x2c..+0x34) = segs[n-1].curved ? segs[n-1].m1 : -segs[n-1].m0;
    startDir = normalize(startDir) if length != 0; endDir = normalize(endDir) if length != 0;
    // normalize: l = sqrt(x²+y²+z²); if (l != 0) { inv = 1/l; v *= inv }
(The exe reads segs[0] even when n == 0; INVENTED: leave start/end/dirs zero then.)
### 0x414d50 ctSegment__load(cur) — ECX = segment (0x54 bytes in memory, 57 in the file)
    p0(+0) = f32 x3; p1(+0xc) = f32 x3; m0(+0x18) = f32 x3; m1(+0x24) = f32 x3;
    len(+0x30) = f32; flatLen(+0x34) = f32; curved(+0x38) = u8; +0x40 = 1.0f;
(`+0x34` is the xz length used by the straight-segment projection 0x414480, card 090;
for straight segments `m1` holds the side normal, notes/19 §2.)
### Runtime fields used by later cards (not in the file)
Traffic ctor 0x416000: `+0x3140` (cross stop offset) = 0. Lane `+0xc4` (halt flag, byte)
is cleared by `Traffic__clearHalts` 0x417c50; its initial value comes from
`FUN_0040e150` in `Traffic__init` 0x419da0 (untraced) — INVENTED: false.

## Changes
1. New `src/game/traffic.h` (generic engine names, no game prefix):
   ```cpp
   // Road graph of the Invictus traffic system (Traffic__load 0x41a000, notes/19 §2).
   namespace traffic {
   struct Segment {                // 0x414d50, 0x54 bytes in the exe
       float p0[3], p1[3], m0[3], m1[3];
       float len = 0, flat_len = 0;  // +0x30, +0x34
       bool curved = false;          // +0x38
       float k40 = 1.0f;             // +0x40, set by the loader, use unknown
   };
   struct Path {                   // 0x414b80, 0x3c bytes in the exe
       std::vector<Segment> segs;
       float start[3] = {}, end[3] = {}, start_dir[3] = {}, end_dir[3] = {};
       float length = 0;             // +0x38
   };
   struct CrossLink { uint32_t node = 0; uint8_t flag = 0; };
   struct CrossPath { uint8_t a = 0, b = 0; uint16_t c = 0; uint32_t k58 = 0, k5c = 0; Segment seg; };
   struct Cross {                  // ctCross 0x40e850
       std::vector<CrossLink> links; std::vector<CrossPath> paths;
       float pos[3] = {}; uint32_t k30 = 0;
   };
   struct Road { uint32_t node[3] = {}; float v10[3] = {}, v1c[3] = {}; uint32_t k64 = 0; Path path; };
   struct Lane {
       uint32_t road = 0, node14 = 0; uint8_t k18 = 0;       // +0x10 (parent road), +0x14, +0x18
       float v1c[3] = {}, v28[3] = {}; uint32_t k34 = 0, k38 = 0;
       float index_min_x = 0, index_run_max_x = 0, index_max_x = 0; uint32_t index_lane = 0;  // +0x78..+0x84
       uint32_t node88[4] = {}; uint32_t k98 = 0, k9c = 0, ka0 = 0;
       Path path;                                            // +0x3c
       bool halted = false;                                  // +0xc4, INVENTED initial value
   };
   struct Graph {
       uint32_t version = 0;
       std::vector<Cross> cross_eq, cross_light;   // Traffic +0x10, +0x28
       std::vector<Road> roads;                    // +0x40
       std::vector<Lane> lanes;                    // +0x58
       float cross_stop_offset = 0;                // +0x3140 (ctor 0x416000: 0)
       // Node ids stay raw (1-based into eq, light, road, lane; 0 = null).
       // INVENTED: ids instead of pointers; bounds checks the exe does not have.
       bool load(const uint8_t *data, size_t size, size_t *consumed, std::string *err);
       size_t node_count() const;  // eq + light + road + lane
   };
   }  // namespace traffic
   ```
2. New `src/game/traffic.cpp`: a small cursor reader (`u8/u16/u32/f32`, fail on
   truncation with an error naming the record kind and index) and `Graph::load` exactly
   in the order above, each reader commented with its address. `*consumed` = bytes read.
   Path post-processing per 0x414b80. Unknown node ids are kept as read (validation is
   the selfcheck's job).
3. `src/Makefile`: add `game/traffic.cpp` to `LIB_SRC` (build list only).
4. `src/tools/resman.cpp` `selfcheck`: after the existing checks, read
   `<game_dir>/maps/city/ai/city.trc` with `fs::read_file` and check (real-data facts):
   - `Graph::load` succeeds and `consumed == size` (the grammar covers the whole file)
   - every node id (cross links, road `node[]`, lane `road`/`node14`/`node88[]`) is
     `<= node_count()`
   - lane `index_lane` values are a permutation of `0..lanes.size()-1`
   - lane `index_min_x` is non-decreasing over the array (0x4191d0 binary-searches it)
   - lane `index_run_max_x` equals the running max of `index_max_x`
   Print the four counts and the total road length (sum of `roads[i].path.length`) as
   info lines (no expected values).

## Files
- new: `src/game/traffic.h`, `src/game/traffic.cpp`
- edit: `src/tools/resman.cpp` (selfcheck step), `src/Makefile` (one LIB_SRC entry)
- read only: `src/platform/fs.h` (`read_file`), notes/19 §2

## Done when
- each reader carries its `// 0xADDR Name` comment; ids-instead-of-pointers, bounds checks,
  the n == 0 path defaults and `halted` initial value are `INVENTED:`
- `make -C src clean check` green with the new resman checks passing on the shipped file;
  no unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
