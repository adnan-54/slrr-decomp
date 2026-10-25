---
id: 100
status: merged
branch:
depends: []
---
# game: pedestrian walk areas from `native walk` params + road-object params check

## Goal
`city.rpk` places 1,799 objects of gametype 0x352 (`native walk`, C++ `_type_walk`) and 755
of gametype 0x15 (`native road`, `_type_road`). Their `params` line carries the data
(`maps/city/ai/city.walk` / `city.rest` are only editor exports of the same lines; the exe
never opens them). Port the walk-area decoder (0x4d5600 -> 0x41dd70 -> 0x41b480 + 0x41d8c0)
into a new `src/game/pedestrians.{h,cpp}`, plus the road params parser (0x41a3e0, parse part
only), and check both against every instance in the shipped `maps/city.rpk` in
`resman --selfcheck`. No walker simulation, no render binding (follow-ups).

## Exe evidence (cleaned pseudocode)

### 0x4d5600 _type_walk vf3 create(obj, params)
    obj = alloc 0x24; obj+0x1c = 0;
    p = strchr(params, ';');                    // 0x569330
    if (!p) return obj;                          // no area
    sscanf(params, "%d;", &obj+0x1c);           // 0x569380, fmt 0x5a024c = "%d;"
    peds = ground.getInfo(0x3d);                // pedestrian manager of the owner ground
    if (peds) obj+0x20 = Pedestrians__addArea(peds, obj+0x1c, p + 1, obj);   // 0x41dd70

### 0x41dd70 addArea(index, text, obj)
    decode(text, g_buf 0x5e6560);               // 0x41b450 -> 0x41b480
    return 0x41dc10(index, &g_buf, obj);

### 0x41b480 decode(&src, &dst)
    src += 8;                                    // 8-char length prefix is SKIPPED, not used
    while (src[0] != 0 && src[1] != 0) {
        *dst++ = (uint8_t)(((src[0] - 1) << 4) | (src[1] - 0x41));   // 8-bit truncation
        src += 2;
    }
    // ('A'..'P' = nibble 0..15; (c-1)<<4 == (c-0x41)<<4 mod 256)

### 0x41dc10 find-or-create(index, &data, obj)
    hash bucket (index & 0x3ff) at +0x240 (12-byte entries {key, value*, next});
    if an area with this index exists: return it (data is NOT parsed again);
    else: area = new (0x41df30); area+0x10 = index; parse(area, &data) (0x41d8c0);
          0x41cb20 (area size, see below); push area at the HEAD of list +0x3398;
          insert into the hash; 0x41e350 (spawn walkers, NOT part of this card).

### 0x41d8c0 parse(area, &p)   (all values little-endian, p advances)
    area+0x14 = f32; area+0x18 = f32;           // SlrrLib calls them x, z
    area+0x1c = f32; area+0x20 = f32; area+0x24 = f32;   // SlrrLib: slope1, slope2, y
    n = i32;
    repeat n:
        type = i32;
        if (type == 0x17) {                      // cWAreaSpline (vtable 0x5a0234 region)
            k = i32; p0 = f32[3]; n0 = f32[3];   // 0x412000(p0, n0)
            repeat k: pi = f32[3]; ni = f32[3];  // 0x412210(pi, ni, 0, 1.0f, 1, 10000.0f, 10000.0f, 0)
        } else {                                 // cWAreaWall
        }
        edge+0x0c = f32 x1; +0x10 = f32 z1; +0x14 = f32 x2; +0x18 = f32 z2; +0x1c = i32 link;
        dx = x2 - x1; dz = z2 - z1; len = sqrt(dz*dz + dx*dx);
        if (len >= 0.0001f) {
            push edge at the HEAD of area+4;     // list order = reverse file order
            l = sqrt(dz*dz + dx*dx);
            if (l <= 1e-5f) { nx = 0; nz = 0; } else { nx = -(dz / l); nz = dx / l; }
            // exact exe ops: r = 1.0f / l; nx = -(r * dz); nz = r * dx;
            edge+0x20 = nx; edge+0x24 = nz;
        } else {
            delete edge;                         // vf+8(1); its spline points were still read
        }
    m = i32;
    repeat m: v = i32; push {v} at the HEAD of area+8;   // related area indices

Cross-check (not authority): SlrrLib `TrcWalk(string)` reads the same layout and names
type 0x17 spline / other = 21 wall; `link` = other walk index (1-based, -1 = none).

### 0x41a3e0 Traffic road-object params (`_type_road` create 0x4d42a0)
    if (sscanf(params, "%d;%d,%d", &kind, &graph, &idx) == 3 && graph == Traffic+0xc) {
        // Traffic+0xc = first u32 of the .trc (0x41a000 copies it) = traffic::Graph::version
        kind 1 -> roads[idx]       (+0x40 pool) ; 0x40ed30(obj) re-parents traffic cars
        kind 2 -> cross_eq[idx]    (+0x10 pool) + 0x10 ; 0x40ebb0(obj)
        kind 3 -> cross_light[idx] (+0x28 pool) + 0x10 ; 0x40ebb0(obj)
    }
    else return 0;
    // no bounds check on idx in the exe.

## Changes
1. New `src/game/pedestrians.h`, namespace `ped`:
   ```cpp
   struct SplinePoint { float pos[3], nrm[3]; };
   struct Edge {                       // cWAreaWall / cWAreaSpline (0x41d8c0)
       int32_t type = 0;               // 0x17 = spline
       std::vector<SplinePoint> spline; // p0 + k points (spline only)
       float x1 = 0, z1 = 0, x2 = 0, z2 = 0;   // +0x0c..+0x18
       int32_t link = 0;               // +0x1c
       float nx = 0, nz = 0;           // +0x20, +0x24
   };
   struct Area {                       // 0x41df30
       int32_t index = 0;              // +0x10
       float f14 = 0, f18 = 0, f1c = 0, f20 = 0, f24 = 0;
       std::vector<Edge> edges;        // +4 list, exe order (head first)
       std::vector<int32_t> related;   // +8 list, exe order (head first)
   };
   // 0x41b480: hex-letter text -> bytes (skips the 8-char prefix).
   std::vector<uint8_t> decode_text(const char *text);
   // 0x41d8c0. INVENTED: bounds checks (returns false on a short buffer; exe has none);
   // *used = bytes consumed.
   bool parse_area(const uint8_t *data, size_t size, Area *out, size_t *used);
   // 0x4d5600 params split ("%d;" + text after ';'). false when there is no ';'.
   bool split_walk_params(const std::string &params, int32_t *index, std::string *text);
   struct Manager {                    // Pedestrians, area part only
       std::vector<Area> areas;        // exe list +0x3398 is head-first: INVENTED vector
       // 0x41dd70/0x41dc10: existing index -> that area, else decode+parse+add.
       const Area *add_area(int32_t index, const char *text);
   };
   // 0x41a3e0 parse part. INVENTED: bounds check of idx against the pool size.
   bool parse_road_params(const std::string &params, int32_t *kind, int32_t *graph, int32_t *idx);
   ```
   `add_area` with a failed parse: keep the area anyway (exe has no failure path) but the
   check below must see 0 failures.
2. New `src/game/pedestrians.cpp` implementing the above, each function with its
   `// 0xADDR` comment. Use `memcpy` for the LE reads (x86 target, same as traffic.cpp).
3. `src/Makefile`: add `game/pedestrians.cpp` to the engine library sources next to
   `game/traffic.cpp` (follow how traffic.cpp is listed).
4. `src/tools/resman.cpp` `selfcheck`, after the traffic graph block (~line 422-460, it
   already has `graph` loaded from city.trc): open `maps/city.rpk` with
   `ResourceManager::open(path, nullptr, &err)` (.rpk only, as the game; a failed open
   is a check failure). For every record
   of that package, scan the record data (`rpk.data(rec)`, `rec.data_size`) line by line
   (split on `\n`, strip `\r`, same tokenising as `load_ground` in game/runtime.cpp:150)
   and pair each `gametype <hex>` line with the next `params <rest>` line of the same
   record. Then:
   - gametype 0x352: `split_walk_params`, `decode_text`, `parse_area`; count them;
     failure if the split or parse fails, or `used != decoded.size()`, or the decoded
     size differs from the 8-char prefix value (decode the prefix the same way, LE u32).
     Feed each through one `ped::Manager::add_area` too.
     Expect exactly 1799 walk params and `manager.areas.size() == 1799` (indices unique).
   - gametype 0x15: `parse_road_params`; expect 755, all `graph == graph.version`, and
     `idx` < roads (kind 1) / cross_eq (kind 2) / cross_light (kind 3) size; expect the
     kind counts 484 / 53 / 218 (= the city.trc pool sizes).
   Print one summary line like the other checks. These counts come from the shipped
   file, not from the exe.

## Files
- new: src/game/pedestrians.h, src/game/pedestrians.cpp
- edit: src/Makefile, src/tools/resman.cpp
- read only: src/game/traffic.h, src/game/runtime.cpp (lines 145-180, line tokeniser)

## Done when
- every ported function has `// 0xADDR`; bounds checks / vector containers are `INVENTED:`
- `make -C src clean check` green with the new selfcheck lines passing; no unit tests
- card frontmatter: status implemented, branch name

## Not in this card (follow-ups)
- 0x41cb20 area size (+0x44) needs 0x41c450 (untraced) and the spline object (+0x38).
- 0x41e350 walker spawning, 0x41e6c0 area removal (`_type_walk` vf4), LOD vf5 0x4d5920.
- `_type_road` binding (0x40ed30/0x40ebb0 re-parent traffic cars to the road object; the
  return value is stored at obj+0x1c and drives LOD vf5 0x4d44d0).
- Hooking `_type_walk`/`_type_road` into Runtime instance creation.
