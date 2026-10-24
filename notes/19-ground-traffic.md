# 19 — GroundRef: ground object, traffic graph, routes, pedestrians, water

Scope: the 30 `java.util.resource.GroundRef` natives (traffic, routes, pedestrians,
water; `setFog` is done, card 072). Everything below is from the exe (2.3.1) unless
marked; addresses are function entries. Script side: `GroundRef extends GameRef`
(`bin/2.2.1 sources/system/Scripts/util/resource/src/GroundRef.java`); `addTraffic`,
`setPedestrianDensity`, `getNearestCross(V)` are script wrappers that call the natives
with `Config.trafficDensity` / `Config.pedestrianDensity` / `0.0`.

## 1. What the ground handle resolves to

Scripts do `map = new GroundRef(maps.city:0x00000001r)` (City.java:179, Track.java:297,
...). The handle points at a typeof-1 record. In `maps/city.rdb`:

    <FILE 00000001.rsd >
    gametype 0x00010035
    params 0x00000010,0,0,0,0,0,0,maps\city\meshes\city.scm,maps\city\ai\city.trc

`0x00010035` resolves through the first external link (`system\`) to system record
`0x35`, whose rsd is `native ground`. The other native types in `system.rdb` are
`camera` (0x33), `player` (0x32), `trigger` (0x34); `maps/city.rdb` itself defines
`native road` (0x15) and `native walk` (0x352); city.rpk instances both (§3.1).
`city2.rdb` has a ground with only the `.scm` (no traffic file); ROC_track/test_track
have no `.trc`. Only `maps/city/ai/city.trc` exists in the install.

`native ground` = RTTI `_type_ground` (vtable 0x59e464, 17 slots, object 0x2dc bytes,
ctor 0x4ba790). Slots: vf3 0x4bade0 create, vf4 0x4bb3d0 destroy, vf13 0x4bac10
per-frame update, vf14 0x4bb6b0 message handler (`obs_add`/`obs_rem`, `%f,%x,%d` hit),
vf15 0x4bb570 getInfo.

### 0x47c640 Game__getInfo(handle, id, out) — ECX = Game 0x5fde90
    if (handle->id(+8) == 0) return 0;
    RI = handle->+0xc; t = RI ? RI->typeof(+0x7c) : 0;
    if (t == 1) { make RI resident; typed = getTyped(RI, 0xa0000000); typed ? return 0x47e450(typed, id, out) : 0 }
    else if (t == 8) { typed(0x80000000) -> (*(typed+0xc))->vf[0x3c](...) }   // jump table not recovered
    else { log "GetInfo: Unexpected TypeOf!"; return out; }
    return 0;
### 0x47e450 GameType__getInfo(typed, id, out)
    if (typed+0x4c == 0) { if (typed+0x50) return 0x47dd50(typed, "getInfo", 1, 2, id, 0x4f); }   // script method
    else { native object = typed+0x44 resource -> typed(0x80000000)->+0xc ->vf[0x3c](typed+0x4c, id, out) }
    return 0;
### 0x4bb570 type_ground__vf15 getInfo(this, id, out)
    1:    out ? bind(out, this+0x28) ; return this+0x24
    2:    out ? out = (0,0,0) ; return 1
    0x17: out ? bind(out, this+0x74) ; return this+0x70
    0x39: return this+0x88            // traffic manager (0x3144 bytes) or 0
    0x3d: return this+0x8c            // pedestrian manager (0x35c0 bytes) or 0
    0x4a: out ? { free(this+0x84, 0x10); this+0x84 = out; return 0 } : return this+0x84   // fog record (card 072)
    default: return 0

Every traffic native gates on `getInfo(self, 0x39, 0)`, every pedestrian native on
`getInfo(self, 0x3d, 0)`. With no native ground object both return 0, which is the
**service-less path** of every native (card 085).

### 0x4bade0 type_ground__vf3 create(owner, params) — traffic/ped part
    Str__scan(params, "%x,%f,%f,%f,%f,%f,%f,%s,%s", &id, &x,&y,&z, &a,&b,&c, scm, trc);   // 0x569380
    g = new type_ground (0x2dc, 0x4ba790); g+0x88 = g+0x8c = g+0x84 = 0; ...
    gravity = Config("gravity") -> world gravity (0,-g,0) if -g <= 0; g+0x94 = Config("ground_friction") (<=0 -> 1.0)
    ... ground body, scm materials (0x4f6240), particles/sound rpk precache, render instance "r_ground" ...
    if (trc[0] == 0) { g+0x88 = 0; g+0x8c = 0; }
    else {
        buf = 0; n = File__readAll(trc, &buf);                     // 0x565610: <0 = missing
        if (n < 0) g+0x88 = 0;
        else { t = new Traffic (0x3144, 0x416000); g+0x88 = t; Traffic__load(t, buf) (0x41a000);
               free(buf); Traffic__init(t, *g_physWorld) (0x419da0); 0x47d670(g, 0x1c); }
        g+0x8c = new Pedestrians(*g_physWorld) (0x35c0, 0x41b570);  // created even when the .trc is missing
    }
    ... g+0x90 = 0 ...
The scan is the exe's own `Str__scan` 0x569380 -> `Str__vscan` 0x527140, not CRT
sscanf: `%s` copies until whitespace (9..13, ' '), NUL, width (default 0x400) **or the
next format character** (here ','), so `%s,%s` splits the two paths at the comma. With a
single path (city2) the scan stops after `scm` and `trc` stays "" (initialised to 0).
`%x` -> hex reader 0x527490; `%f` -> 0x527530 (untraced); a failed conversion returns
the count so far, leaving later fields untouched.
Destroy 0x4bb3d0 frees traffic (0x416400) / peds (0x41b9c0) and sets `DAT_005e652c = 0`.

## 2. Traffic manager (`Traffic`, 0x3144 bytes)

Ctor 0x416000, load 0x41a000, init 0x419da0, dtor 0x416400. Globals point into the
active instance: `DAT_005e652c` = Traffic, `DAT_005e6524` = +0x10, `DAT_005e6528` =
+0x28, `DAT_005e6510` = +0x40, `DAT_005e650c` = +0x58, `DAT_005e6504` = +0x70.

Four chunked arrays (`ChunkArray` = {+0 page table, +4 count, +8 capacity, +0xc page
size (ctor 0x411f80: 0x400; load 0x414fc0 sets it to the file count, 0 -> 1), +0x10
element size}; element i = `pages[i / pagesize] + elsize * (i % pagesize)`, allocator
0x414b00):

| Traffic off | class (ctor) | elem | file loader | city.trc count |
|---|---|---|---|---|
| +0x10 (count +0x14) | ctCrossEq (0x415470) | 0x60 | 0x415b30 = jmp 0x40e850 ctCross__load | 53 |
| +0x28 (count +0x2c) | ctCrossLight (0x4154b0) | 0x78 | 0x415b30 | 218 |
| +0x40 (count +0x44) | ctRoad (0x40c6d0) | 0x78 | 0x40eb20 | 484 |
| +0x58 (count +0x5c) | ctLane (0x40c610) | 0xd0 | 0x40e9b0 | 1572 |
+0x70: traffic-car pool (0x164-byte cars, pages of 0x400, count +0x78); +0x84 sim time
(set by 0x419da0 from `*g_physWorld`); +0xcc/+0xd4 halt-handle list; +0x3118 player
pseudo-car; +0x3130 event sink (`car_rem %d %d` messages); +0x3140 cross stop offset.

### .trc format (0x41a000) — verified on `maps/city/ai/city.trc`
Little-endian, no padding. Parsing the whole 843,301-byte file with this grammar ends
exactly at EOF and every node index is in 1..2327 (scratch check, card 086 makes it a
`make check` step).

    u32 version (Traffic+0xc; city: 1)
    u32 nCrossEq, nCrossLight, nRoad, nLane          (city: 53, 218, 484, 1572)
    nCrossEq  x Cross, nCrossLight x Cross, nRoad x Road, nLane x Lane
    Node index: 1-based into [crossEq..., crossLight..., road..., lane...]; 0 = null
    (the loader builds a pointer table of nTotal+1 entries, entry 0 = 0).

    Cross (0x40e850):  u32 nLink; nLink x {u32 node (+0x14[i].ptr), u8 (+0x14[i].flag)}
                       u32 nPath; nPath x {u8 (+0), u8 (+1), u16 (+2), u32 (+0x58), u32 (+0x5c), Segment (+4)}   // 0x60-byte entries at +0x1c
                       f32[3] pos (+0x24..+0x2c); u32 (+0x30)
    Road (0x40eb20):   u32 node (+4), node (+8), node (+0xc); f32[3] (+0x10); f32[3] (+0x1c); u32 (+0x64); Path (+0x28)
    Lane (0x40e9b0):   u32 node (+0x10), node (+0x14); u8 (+0x18); f32[3] (+0x1c); f32[3] (+0x28);
                       u32 (+0x34), u32 (+0x38), f32 (+0x78), f32 (+0x7c), f32 (+0x80), u32 (+0x84 lane index, raw);
                       u32 node (+0x88), node (+0x8c), node (+0x90), node (+0x94); u32 (+0x98), (+0x9c), (+0xa0); Path (+0x3c)
    Path (0x414b80, 0x3c bytes): u32 n; n x Segment (0x54-byte entries)
    Segment (0x414d50, file 57 bytes): f32[3] p0 (+0), p1 (+0xc), m0 (+0x18), m1 (+0x24); f32 len (+0x30); f32 (+0x34); u8 curved (+0x38); mem +0x40 = 1.0f

Path object after load (0x414b80): `+0 n, +4 seg*, +8..+0x10 start = seg[0].p0,
+0x14..+0x1c end = seg[n-1].p1, +0x20..+0x28 start dir = normalize(seg[0].m0),
+0x2c..+0x34 end dir = normalize(last.curved ? last.m1 : -last.m0), +0x38 total length
= sum(len)` (float accumulated in an int slot, same bits).
Lane +0x78/+0x7c/+0x80/+0x84 are not per-lane data but an **x-sorted spatial index**
stored in the lane array: entry i refers to lane `+0x84` (a permutation of 0..n-1 in
city.trc), `+0x78` ≈ that lane's min x (padded, ~6 m), `+0x80` ≈ its max x (padded),
`+0x7c` = running max of `+0x80` over entries 0..i (holds for all 1,572 entries); the
entries are sorted ascending by `+0x78`. `0x4191d0` binary-searches it (scratch check
on city.trc; card 086 asserts sortedness and the running max).

### Segment / path geometry
- 0x4134e0 `ctSegment__pos(out, d)`: curved: t = d/len, Hermite `p0*(2t³-3t²+1) + p1*(3t²-2t³) + m0*(t³-2t²+t) + m1*(t³-t²)`; straight: `p0 + m0*d`.
- 0x412f60 `ctSegment__dir(out, d)`: curved: normalized Hermite derivative (coefs `6(t²-t)`, `6(t-t²)`, `3t²-4t+1`, `3t²-2t` on p0,p1,m0,m1); straight: m0.
- 0x4131a0 `ctSegment__side(out, d)`: curved: derivative (dx, dz) -> normalize(-dz, 0, dx)·…(see card 089); straight: m1.
- 0x413660 `ctSegment__posOffset(out, d, off)` = pos + side·off.
- 0x413360 `ctPath__posAt(out, d)` / 0x412ed0 `ctPath__dirAt(out, d)` / 0x4133f0 `ctPath__posOffsetAt(out, d, off)`: negative d wraps by total length, then walk segments by len.
- 0x414480 `ctSegment__project(x, z, &d, &lat)` (xz plane), 0x414830 `ctPath__project(x, z, &d, &lat)`: nearest segment by |lat|, returns 1 if any segment exists.

### Queries
- 0x419050 `Traffic__nearestCross(x, y, z, dist, yflag)`: scans crossEq then crossLight; key `| |pos - p| - dist |`; with yflag only crosses with |pos.y - y| < 5; first minimum wins (strict `<`).
- 0x4191d0 `Traffic__nearestLanePos(x, y, z, &d, &lat)`: lane binary search + project (card 090).
- 0x417e50 `Traffic__findRoute(p1, p2, flag1, flag2)` -> new 0x3c-byte Path or 0: A*-style search (heap 0x41a8b0, node array 0x4167a0, heuristic 0x419c50, path build 0x4120d0/0x412960). **UNRESOLVED**: Ghidra drops ~70 blocks of this function ("Removing unreachable block" 0x4182e7..0x418ae0); it needs an assembly-level trace before it can be carded. Blocks: `getRouteLength(VV)/findRoute`, `getStartDirection` (0x419530), `haltTrafficPath` (0x4196f0).
- Route state is process-wide: `0x5fe450` Path*, `0x5fe454` length `|e - s|`, `0x5fe458` s, `0x5fe45c` e (positions on the path from 0x414830), `0x5fe460..468` p1, `0x5fe46c..474` p2. Only `findRoute` (0x4ddc50) writes it; `getRouteLength()` 0x4ddc30, `getRoutePos` 0x4dde50, `getRouteDist` 0x4ddb50 read it.

### Traffic cars (not carded: need vehicle sim + render instances)
`addTrafficN` 0x4dc9e0 spawns `min(n, trafficDensity*20)` scripted `traffic_car` objects
(`GameObject__createScripted(..., "0,-10000,0,0,0,0", "traffic_car")`) plus n lane cars
(0x40ce00 `ctLane__spawnCar`, random lane; Ghidra shows constant args 1.0, 2.0, 4.0,
1.0 — UNRESOLVED until asm-checked), setting car
+0x154 random id, +0x2c speed `13.611 + r·0.000237` (m/s), +0x30 `0.7 + r·1.22e-5`,
+0x14/+0x18/+0x1c/+0x20 the script floats, +0x24 = 1, +0xdd notify flag
(`getInfo(type, 0x45)`), +0xa8 lateral `2r-1`; returns the number spawned. `addTrafficP`
0x4dcdc0 is the same near a position (0x416800). `addTrafficCar` 0x4dd090 wraps a script
car instance into a traffic car (speed `19.44 + r·0.000339`). Car ids returned to
script are raw C++ pointers (`remTrafficCar`, `notifyTrafficCar`, `setTrafficCarBehaviour`
+0x160, `getTrafficCarPos` dereference the int). Per-frame update: type_ground vf13
0x4bac10 -> 0x41a9d0, 0x41b290, 0x417c90 (untraced). Halts: `haltTrafficCross` 0x4de360
(0x419050 + 0x40de70), `haltTrafficRoad` 0x4de270 (0x419ba0), `clearHalts` 0x4dd6c0
(0x417c50: every lane +0xc4 = 0), `delTraffic` 0x4dd840 (0x4177a0), stop offset
0x4dd7f0 (Traffic+0x3140).

## 3. Pedestrians (`Pedestrians`, 0x35c0 bytes; not carded beyond no-service)
Ctor 0x41b570(physWorld), dtor 0x41b9c0, update 0x41cc20/0x41ed70 (from vf13). Walker
groups list at +0x3398 (`{next, ..., +0xc walker list}`; walker +0x10 state, +0xc0 type
id, +0xc4 pos). Types: up to 32 handles at +0x33c0 + 16·i, count +0x33bc (add
0x41c370, remove 0x41e1b0 swap-with-last). `pedestrianDistance` 0x4de420 ->
0x41d540 (nearest walker of type, 0 = any; -1.0 when none); `getPedestrianPos`
0x4dda10 -> 0x41d620 (nearest walker pos, (0,0,0) when none); `setPedestrianDensityN`
0x4de580 -> 0x41e690(d, d·1.1) which only writes globals `0x5da008`/`0x5da00c`.
Walk areas come from `_type_walk` (vtable 0x5a0254) create 0x4d5600: params `"<%d>;<data>"`
-> `0x41dd70(n, data, obj)` on the ground's ped manager (getInfo 0x3d of the
owner). See §3.1 for where the data comes from.

### 3.1 Walk areas and road objects come from the map RPK (correction)
Earlier text said no shipped data instances the walk/road types. Wrong: `maps/city.rpk`
places 1,799 objects with `gametype 0x00000352` (`native walk`, `_type_walk`) and 755 with
`gametype 0x00000015` (`native road`, `_type_road`) inside its spatial records
(city2.rpk has none). `maps/city/ai/city.walk` (1,799 lines) and `city.rest` (755 lines) are
editor exports of exactly these `bbox params` pairs; the exe has no `.walk`/`.rest` string
and never opens them. The user-provided editors (`bin/tools/SCXWalkMarker`, `SCXTRCMarker`,
both on SlrrLib `Model/TrcAccess`) read and write the RPK objects, not those files.

Walk params `"<index>;<text>"` (0x4d5600, fmt `"%d;"` at 0x5a024c):
- text = 8-char length prefix (skipped by the exe, 0x41b480) + byte pairs, letters `A`..`P`
  = nibbles, byte = ((c0-1)<<4 | (c1-0x41)) & 0xff.
- 0x41dc10: areas are keyed by index in a 0x400-bucket hash at Pedestrians+0x240; an existing
  index returns the existing area (no re-parse). New area: +0x10 index, parse 0x41d8c0, size
  0x41cb20 (+0x44), head-insert into list +0x3398, spawn walkers 0x41e350.
- 0x41d8c0 layout: f32 x5 (+0x14..+0x24; SlrrLib: x, z, slope1, slope2, y); i32 n; n x
  { i32 type; type 0x17 -> `cWAreaSpline`: i32 k, (pos f32[3], nrm f32[3]) x (k+1)
  (0x412000 first, 0x412210(.., 0, 1.0, 1, 10000, 10000, 0) rest); else `cWAreaWall`;
  f32 x1, z1, x2, z2; i32 link (other area index, 1-based, -1 none) }. Edges with
  length < 0.0001 are deleted; others head-inserted at area+4 with normal
  (+0x20, +0x24) = (-dz, dx)/len. Then i32 m; m x i32 related index, head-inserted at +8.
  Matches SlrrLib `TrcWalk(string)` field for field.

Road params `"%d;%d,%d"` = kind; graph; index (0x41a3e0, via ground getInfo 0x39 =
Traffic). graph must equal Traffic+0xc = the .trc header u32 (city.trc: 1). kind 1 ->
roads[index] (0x40ed30), 2 -> cross_eq[index]+0x10, 3 -> cross_light[index]+0x10 (0x40ebb0);
no bounds check. city.rpk: kinds 484/53/218 = the city.trc pool sizes. 0x40ed30/0x40ebb0
re-parent the traffic cars on that road/cross under the road object; the returned pointer is
kept at obj+0x1c and drives the LOD callback vf5 0x4d44d0. Card 100 ports the walk decoder
and checks both param kinds against city.rpk.


## 4. Water (physics world, no traffic)
- `setWater(FFF)` 0x4dc400: `getArgs(self, &level?, &density, &viscosity)` — the first
  float is read into the point's y slot: point = (0, level, 0), normal = (0,1,0);
  defaults (0,-12,0), (0,1,0), 300, 550. If `handle+0xc` (RI) != 0: density > 0 &&
  viscosity > 0 -> `PhysicsWorld__setWater(density, viscosity, &normal, &point, 0)`
  (0x501e10) and `world+0x80 = 1`; else `world+0x80 = 0`. UNRESOLVED: the getArgs
  call passes `&local_18` (point.y) for the first float — matches the 2.2.1 signature
  `setWater(level, density, viscosity)`; Track.java calls it with Vector3s, which
  selects the other overload.
- `setWater(V,V,F,F)` 0x4dc2b0: same with point/normal from the two Vector3 (x,y,z).
- `addWaterLimit(V,V)` 0x4da500: if RI != 0 -> 0x4f8330(point, normal): append a
  6-float plane {point, normal} to `world+0x380` (count `world+0x384`), -1 on exact
  duplicate or realloc failure.
- 0x501e10 `PhysicsWorld__setWater(density, viscosity, normal, point, vel)`:
  `+0x394 = density, +0x398 = viscosity`; normal -> `+0x374` (normalized), point ->
  `+0x368`, vel -> `+0x388` (when non-null); frees the limit list (`+0x380 = 0`,
  `+0x384 = 0`). These are the 0x800-body plane-contact fields of notes/08/11
  (`PhysicsWorld__init` defaults: point (0,-7,0), normal (0,-1,0), 300, 550).
src has no process-wide physics world (`physics::World` lives in callers), so water
needs a world-owner service; not carded here (same blocker as the Chassis natives).

## 5. No-service returns (src has no native ground object)
| native | exe address | no-service result |
|---|---|---|
| setWater(FFF), setWater(VVFF), addWaterLimit | 0x4dc400, 0x4dc2b0, 0x4da500 | void, no effect (needs physics world) |
| addTrafficCar | 0x4dd090 | instance null: log `vm_ground_addtrafficcar: Traffic car instance is null in "..."`, 0; else 0 |
| remTrafficCar, setTrafficCarBehaviour | 0x4de520, 0x4e6280 | void (id 0 -> nothing) |
| notifyTrafficCar | 0x4de5f0 | 0 |
| addTrafficN, addTrafficP | 0x4dc9e0, 0x4dcdc0 | 0 |
| delTraffic, clearHalts, setTrafficCrossStopOffset, haltTrafficCross/Path/Road | 0x4dd840, 0x4dd6c0, 0x4dd7f0, 0x4de360, 0x4dd700, 0x4de270 | void |
| getNearestCross, getStartDirection | 0x4dd880, 0x4ddfb0 | null |
| getRouteLength(VV), findRoute | 0x4ddc50 (shared) | -1.0 |
| getRouteLength() | 0x4ddc30 | 0.0 (route global unset) |
| getRoutePos(F) | 0x4dde50 | null (route global unset) |
| getRouteDist(V) | 0x4ddb50 | 0.0 (route global unset; -1.0 only with a route and a null arg) |
| alignToRoad | 0x4dd470 | null |
| setPedestrianDensityN, addPedestrianType, removePedestrianType | 0x4de580, 0x4dc940, 0x4de4d0 | void |
| pedestrianDistance | 0x4de420 | -1.0 |
| getTrafficCarPos | 0x4de1c0 | void (id 0 -> out untouched) |
| getPedestrianPos | 0x4dda10 | out.x = out.y = out.z = 0 |

## 6. Cards
085 no-service returns (all 29 stub rows) · 086 `.trc` loader + shipped-file check ·
087 ground binding (getInfo 0x39) + setTrafficCrossStopOffset (needs 085, 086) · 088
getNearestCross + clearHalts (needs 087) · 089 segment/path geometry (needs 086) · 090
projection + alignToRoad (needs 087, 089).
Not carded (next traces): `Traffic__findRoute` 0x417e50 at asm level, then
findRoute/getRouteLength(VV), getRouteLength(), getRoutePos (also needs 0x4133f0 /
0x413660 offset eval, decompiled above), getRouteDist (0x414830), getStartDirection
(0x419530), haltTrafficPath (0x4196f0); haltTrafficRoad (0x419ba0, needs 0x419ab0
untraced); traffic cars (vehicle sim + scripted `traffic_car` objects + per-frame
0x41a9d0/0x41b290/0x417c90); pedestrians (0x41b570 family + walk areas 0x41dd70);
water (needs a process-wide physics world owner).

## 7. Map collision: game-instance data blob, ground body, triangle contacts (2026-10-24, cards 142/143)

**Where the map collision lives.** Not in the `.scm` (that file is only `materials N` /
`material i name`, parsed by `Cfg__parseKeyTable` 0x4f6240 with the key table at 0x5daa80)
and not in a type-6 shape. It is in the **binary data of the map's `ins_game` (type 1)
records**: the top map record (`maps/<map>.rpk` id 1, flags 0x11/0x01, size 0x10d in
ROC_track) carries an embedded resource tree of type-1 child records (area_N, every map
object), and each child's data has a `PHYS` triangle soup. `ResourceInstance_Game__load`
0x5195a0 (vtable 0x5a7d70 slot 10, +0x28; `ResourceInstance_Game__vf10` in names.json) calls the parser below when the
instance is first made resident, then runs the native create (`typed->+0xc->vf3`, here
`type_ground__vf3` 0x4bade0) and the script `<init>` (THRD-CREATE).

### `ResourceInstance_Game__loadData` 0x519940 (ECX = typed data `T` = R+0x108; args R, data, size)
```
p = data + 4 + 4*u32[data]                    // skip n leading dwords (0 in every shipped map)
nChild = u32 p; p += 4
repeat nChild: rec.flags(+9) |= 8; p += 0x51e6b0(p, R+0x80, &child, R)   // same record format as the
              // .rpk record table (notes/01): super,id,type,flags,f32,off,size,nameLen,name,[48 if flags&1];
              // off/size point into the same .rpk file. if R+0x84 & 0x80000: child flag 0x80000 + make resident
while (p - data < size) { tag = u32 p; sz = u32 p+4; q = p+8
  tag 0, sz 0   : the rest of the blob (size - 12 bytes from p+0xc) is RSD text, as below
  'RSD\0' 0x445352 : sz bytes of text lines: "gametype %d" -> 0x51fa40(0x51e860(id), 8, 0) (precache type);
                  "params %2048s" -> T+0x48 (malloc 0x801 once). Then continue after the text.
  'PHYS' 0x53594850 : nf = u32 q; T+0x18 = nf; T+0x1c = malloc(nf*0x20); q += 4
                  per face (file 28 bytes): u32 mat -> +0, u32 i0,i1,i2 -> +4,+8,+0xc, f32 n[3] -> +0x10..+0x18; +0x1c = 0
                  nv = u32; T+0x10 = nv; T+0x14 = malloc(nv*12); nv x f32[3] verts (metres, world frame)
                  (sz is not read: the walker continues after the parsed data; shipped sz == parsed length)
  'EXTP' 0x50545845 : sz/4 ids, each 0x51e650(id) (precache); then R's AABB (+0xb0 kind=1, centre +0xb4, half
                  +0xc0) is grown to cover every child's AABB
  'POLY' 0x594c4f50 : T+0x30 = count, T+0x34 = polygon records {u32 a, u32 n14, u32 n3, n14 x 14 dwords, n3 x 3 dwords}
                  (not needed for collision; not traced further)
  other         : p = q + sz }
DAT_00773ab4++ (loaded game instances)
```
Unload 0x51a1b0 frees `R+0x11c` (=T+0x14 verts), `R+0x13c`, `R+0x124` (T+0x1c faces; face `+0x1c` != 0 goes to
0x4fb000 on the world first), so `+0x1c` is a runtime per-face link (contact/pair cache), 0 after load.

**Shipped corpus (scratch parse, card 142 makes it an rpkdump check).** Every type-1 blob of the four map
packages parses to its exact size, all face indices < nv, PHYS `sz` = parsed length, embedded ids unique:
| map | type-1 instances (top + embedded) | PHYS chunks | faces | verts | POLY | max mat | `.scm` materials |
|---|---|---|---|---|---|---|---|
| city | 58,210 | 23,976 | 197,286 | 236,064 | 624 | 107 | 108 |
| city2 | 56,689 | 19,040 | 251,902 | 276,107 | 748 | 74 | 75 |
| test_track | 12,224 | 6,420 | 66,825 | 77,132 | 242 | 85 | none |
| ROC_track | 10,894 | 5,147 | 55,040 | 61,777 | 155 | 42 | none |
Vertex bounds are map-sized (city x −1471..1485, y −27..70, z −1929..329), i.e. metres in the world frame
(no ×0.01 as in `.scx`). Highest triangle under (x,z) = (0,0): city 1.00, test_track 6.37, ROC_track 0.003.
UNRESOLVED: what `mat` indexes when the map has no `.scm` (test_track/ROC_track use 0..85/0..42, while
type_ground's default material array has 1 entry).

### `type_ground__vf3` 0x4bade0, body part (the part elided in §1)
```
g = new type_ground; g+0x3c = 0x517b80(g+8); g+0x7c = g+0x80 = g+0x2d8 = g+0x78 = 0
gravity:  v = Config("gravity") (0x47c080); if (-v <= 0) world+0x1c4..0x1cc = (0, -v, 0)
g+0x94 = Config("ground_friction"); if (<= 0) 1.0
def = Physics__createBox(0, 0, 0,0,0, 0)       // mass 0 -> inv mass 0, def+0xb4 |= 4 (static, no integration);
                                               // shape kind 0 (none): Collide__bodyPair never pairs it
g+0x40 = ref(def); def+0xb4 |= 4; def+0x2c = 0
shape = def+0x90: +0x58 = 0.2 (0x3e4ccccd), +0x50 = g+0x94 (friction), +0x54 = 0.01 (0x3c23d70a, rolling res),
        +0x9c = 0x80000000, +0xa0 = 0.6 (0x3f19999a)    // the shape doubles as the ground material (S+0xac, notes/09)
precache particles.rpk 2/10 (type 0xe), sound.rpk 6 (type 6)
if (scm[0]) { n = File__readAll(scm); if (n >= 0) Cfg__parseKeyTable(buf, n, "materials" table 0x5daa80, g) }
if (g+0x78 == 0) { g+0x78 = 1; g+0x7c = new material[1] (0x48 bytes, ctor 0x487a80); precache system.rpk 3, 9 }
Physics__attachToResource 0x4e8a50(g, def, &(x,y,z) /*params %f 1..3*/, mat, "groundbody")
   = PhysicsWorld__addBody(def, pos, mat): def flag 4 -> NOT head-inserted under w+0x84 (never stepped)
     + GameObject__createPhysInstance(g, 0, body, "groundbody")
g->resource flags (+0x84) |= 0x200000 (0x4ebdd0)       // the ground's own node is skipped by the collision walk
b = GameRef__getPhysBody(g); if (b) b+0xb4 body +0x58 &= ~1
precache render record id (0x51e860(id)) type 0xe; Render__createInstance(g, g+0x48, "r_ground", 0); bone00 ...
traffic / pedestrians (§1), 0x4ebd90(g, 0x14, 0), g+0x90 = 0, IntervalMgr__addNode 0x4ee1a0(g, 0)
```
`a,b,c` (params %f 4..6) are scanned but this function never reads them (the matrix `local_6c` is not
initialised in the decompile; UNRESOLVED whether it is the identity built by 0x5663c0/0x569b20).

### Who collides with the soup
`IntervalMgr__collidePass` 0x4ee460 (big per-step pass over interval nodes; also calls `Collide__candidatesTOI`
0x4fc7e0) walks resource children: type 1 -> **`Collide__groundTree` 0x4fe6f0(obj, node, groundBody, depth,
bounds)**, type 2 -> `Collide__physInstPair` 0x4ff9e0.
- `Collide__groundTree`: node AABB (+0xb4 centre, +0xc0 half) vs obj AABB; miss -> (cheap path 0x4fe550 when
  `DAT_006028dc < DAT_005dadbc`) return 0. Hit: if not loaded, `ResourceNode__requestLoad(node, node+0x9c, 0,
  0 or 10)` (**streaming: map nodes load because a body is near them**); when loaded, T = typed data; if
  `T+0x28 == node+0x80` the ground body is re-fetched through getInfo(1) of the native (vf15 id 1 = g+0x24);
  `if (T+0x18) Collide__groundSoup(obj, node, T, groundBody)`; then recurse into the node's type-1 children
  (except `+0x84 & 0x200000`) and pair type-2 children with `Collide__physInstPair`.
- **`Collide__groundSoup` 0x4ff6b0(obj, node, T, groundBody)**: for every type-2 child (physics instance) of
  obj whose node AABB test (0x516640 sphere / 0x516450 box by node+0xb0) does not reject and whose body and def
  are not flagged 0x24: clear the 64 candidate slots at 0x6029e4 (stride 0xa0); body flag 0x800 -> for each
  face `Collide__bodyVsStatic`-style sweep 0x4fd110(rootBody, T+0x14 verts, face, groundBody, &prev/cur pos);
  capsule-y/z sensor bodies (shape 3/4, flag 0x40, not 0x1000) -> 0x4fd110 per face with the capsule segment;
  otherwise (no 0x20000) **`Collide__bodyVsTriangles` 0x4feb20(body, verts, faces, groundBody, node+0xb0,
  nf, 0)** (recursive over compound children; callees 0x4fbda0, 0x4fd110, 0x4fc450, 0x50f000, 0x504570,
  0x4ffec0) which queues `body_dummy_candidate`s; `Collide__candidatesTOI` 0x4fc7e0 then bisects the earliest
  TOI and calls `PhysicsWorld__addContact(body, …, B = groundBody, …)`. Not traced in detail (card 144+).
So in the exe the car (box `car_root` + `part_comp` children) gets c_collision contacts against triangles of
every resident map node near it, with the static `groundbody` as side B (friction = min(shape μ), §5 of
notes/11).

### What src has (2026-10-24)
- `src/resource/rpk.*` parses the record table only; type-1 data blobs (embedded child records, PHYS, RSD)
  are not parsed. `load_ground` (src/game/runtime.cpp) line-scans the blob as text: it finds `gametype`/`params`
  only because the RSD text is the blob tail (fragile; the exe reads the RSD/0 chunk, 0x519940).
- Embedded child instances (58k in city) are never mounted (`Runtime::mount_package` walks the table only).
- `physics::World` pairs only active roots (`bodies`); it has `SHAPE_MESH` + `CollisionMesh` (AABB tree) +
  `mesh_contacts` (sphere/box/capsule vs soup), static bodies (`inv_mass == 0`), but nothing creates a ground.
- `Runtime::instantiate` ignores native kinds other than car (shortcut comment ~runtime.cpp:528).

### Side note: `Class not found: java.game.parts.bodypart.Block_Vee` (vmboot --selfcheck)
Not real misses. They come from `jvm::load_class` (src/script/packages.cpp:272) being called by the VM
class-loader callback (src/app/boot.cpp:79) while compile-on-load resolves a simple name through the
source's import list in order: e.g. `Block_Vee_OHC.java` (package `...block.block_vee`) imports
`java.render.osd.*`, `java.io.*`, ..., `java.game.parts.bodypart.*`, `java.game.parts.enginepart.*`,
`java.game.parts.enginepart.block.*`; every earlier import prints one line before the last one finds
`parts/scripts/enginepart/block/Block_Vee.class` (same for Camshaft, Part, Inventory, Headlights: 6,289
lines in one run). The same run reports `compile on load: 635 sources, ok 635, failed 0` and
`selfcheck: PASSED`. The exe has the string `"Class not found: "` (0x5b46e4) but whether it prints it on
import probes is not traced; src could silence probe misses (cosmetic, no card).
