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

### Sensor segment vs PHYS faces (0x4ff6b0 sensor branch -> 0x4fd110), RE 2026-10-24 (cards 156/157)
`Collide__groundSoup` 0x4ff6b0, for a body with shape kind 3/4 (capsule y/z), flag 0x40, not 0x1000 (the
wheel sensors of 0x4f3f10): seg P0 = state * (0, −a, 0), P1 = state * (0, +a, 0) (kind 3; kind 4 uses z;
a = shape +0xb4 half length; 0x4a2b40 = local -> world with the body's active state), then for **every** face
of the node (`T+0x18` faces, no per-face AABB) `0x4fd110(body, T+0x14 verts, face, groundBody, seg)`:
```
v0 = verts[face.i0]; n = face.n (PHYS +0x10, stored normal, not recomputed)
d0 = n·(P0 − v0); d1 = n·(P1 − v0)
if !(d1 > 0 && d0 <= 0) return 0                 // one-sided: bottom behind / on the plane, top in front
if (|d0| > 0.02 || |d1| > 0.02):                 // crossing
  t = |d0 / (d0 − d1)|;  q = (P0 − v0) + t·(P1 − P0)
  (i, j) = 0x5043d0(face): |ny|<|nx| ? (|nz|<=|nx| ? (1,2) : (0,1)) : (|nz|<=|ny| ? (0,2) : (0,1))
  a = v1 − v0, b = v2 − v0, inv = 1/(b_j·a_i − b_i·a_j)
  s = (b_j·q_i − b_i·q_j)·inv;  u = (a_i·q_j − a_j·q_i)·inv
  if (0<=s<=1 && u>=0 && s+u<=1) { X = v0 + q; if body: addTriggerContact 0x502d60(body, ground, face,
       pA = 0x4af9a0(X) (body frame), pB = X, n = &face.n, 0); return t }
  return 0
else (segment within 0.02 of the plane): 2D clip of P0->P1 against the 3 edges in axes (i,j), entering t
  max / leaving t min, break (return 0) when outside or empty; hit point = P0 + t_enter·(P0 − P1) (sign as
  decompiled), same 0x502d60 call; return t_enter
```
0x502d60 without body flag 0x1000 fills a temporary c_collision (0x4f2770: A = sensor, B = ground, c+0x74 = pA,
c+0x80 = pB, c+0x8c = n, c+0x60 = face) and `Game__queueEvent(sensor+0x38, 0, 0x20, c)` (synchronous) ->
car handler 0x4a2e60 -> `c_triggerspring__onSensorContact` 0x4f3d70 (notes/09 "Who writes the ray hit").
So the exe wheel "ray" is: segment from the car_root mount down by the createCar-time `L = W+0x5c`, only
PHYS faces whose stored normal faces the top end, nearest crossing to the top wins (strict `<`, first face
in node order on ties), rejected when it lies more than `r` above the previous length, and counted only
when `d < max_ray`. src (card 144/146) instead casts a two-sided ray of length `max_ray` against the
merged soup and uses the geometric normal flipped toward the ray (`mesh_raycast`): cards 156/157 port the
exe rule; the merged-soup collider stays (geometrically the same face set as the per-node walk as long
as every face lies inside its node AABB; streaming is a residency/script effect, not a collision one).
RESOLVED (notes/08 "Exact pass order"): `IntervalMgr__collidePass` runs inside 0x502060's event phase
(0x4fc610 -> 0x4ee460), after integrate and before the next step's controllers, so src's query at the start
of `update_suspension` sees the same pose (except after an in-loop `resolveImpacts`).

### Body-vs-soup narrow phase `Collide__bodyVsTriangles` 0x4feb20 (decompiled 2026-10-24, not ported)
Args `(body, verts, faces, groundBody, nodeBounds, nf, faceMask*)`, ECX = world-ish counter block (`+0x40++`).
```
if (body+0x58 & 0x800): seg = (other-state pos, active pos); for each face 0x4fd110(body, verts, face, ground, seg); return 0
shape = body.def+0x90; br = shape+0xc; s = other state (prev)
if |s.pos - nodeBounds.c|_k > nodeBounds.h_k + br for any k: return 0           // node AABB vs body sphere
if shape kind 3/4 and (flags & 0x1040) == 0x40: sensor segment, 0x4fd110 per face (mask[i]==0 only); return 0
mask[nf] = copy of faceMask (or 0); live = #zero
for face i (mask 0): if slot DAT_00602948[i]+0x9c == 0: 0x504570(verts, face) (face AABB cache, stride 0xa0)
                     0x4f9860(s, sweep = (prev pos, half motion), br, slot) == 0 -> mask = 1, live--  // swept-sphere vs face AABB
if live && kind != 1:
   OBB = shape+0x14 half extents (kind 6 with children: AABB of shape+0x24 bounds rotated) + |R·motion|·1.5 + 0.05
   for face (mask 0): 0x4fc450(OBB, tri in body frame) == 0 -> mask = 1, live--                  // SAT box vs triangle
if live:
   children (+0x24): recurse 0x4feb20(child, ..., mask) for children without 0x20 / def 0x20 / 0x20000; return Σ
   else per face (mask 0): face+0x1c cached triangle shape (0x4ffec0 alloc, +0x84 = 0.2, +0x7c = 0.6, 0x50f000 builds it
        from the face's 3 vertices / normal), +0xf4 = *ECX; n += 0x4fbda0(body, shape, triShape, ground)   // queues body_dummy_candidate in DAT_006028f8
return n
```
`Collide__candidatesTOI` 0x4fc7e0(ground): for every candidate in `DAT_006028f8`: resolve cached features
(0x4f6ea0), d = `0x50d230(prev state, ground state, shape, tri+0x2c, features..)` − margin; keep the minimum;
10-step time bisection (t = 1.0, step 0.5, `0x506c00` interpolate prev->cur) until d in (0, 0.02] and
approaching (normal·Δ < 0); deep penetration (d <= −margin): pick the vertex (`shape+0x30` list, kind 1) with
min distance along the face normal; then `0x4fa550`, margin re-added/subtracted (pA −= r·Rᵀn), and
`PhysicsWorld__addContact 0x502830(body, ground, tri, fA, fB, pA, pB, n, d, toi, t, worldPt)` — **one contact
per body per collide pass (earliest TOI)**; candidates freed, their features written back.
src instead: `consider_pair` -> `collide_shapes_within(box/sphere/capsule, mesh)` -> `mesh_contacts` returns up
to 4 hits over the merged soup on the start pose. Differences: number of contacts per pass (exe 1, earliest
TOI; addContact also keeps only the earliest-TOI contact per root), TOI bisection between prev and current
state, contact created in the event phase. Port needs 0x4fbda0, 0x4fc450, 0x4f9860, 0x50d230, 0x4fa550,
0x50f000 too (not decompiled here) -> too big for one card (> 5 functions), and vmboot has no real-data check
that bites (car_root/part_comp never touch the soup at rest or in the launch). Card 159 not written; next step
is a trace of those six callees plus a vmboot scenario where the box hits the ground (e.g. a car without
wheels/springs, if a shipped car or command produces one).

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

## 8. Race bots: `native bot` (sl:0x6E "botbrain"), RE 2026-10-24 (cards 169+)

**Script side (what the race asks for).** `CircuitRacing` (`gmcEvent.racers` > 0) -> `Gamemode.createBotCars`
(Gamemode.java:581: `Bot.createCar(map, 1)` builds a `Vehicle`) -> `joinBotCars` -> `Bot.enterCar`
(Bot.java:217): `brain = new GameRef(world, sl:0x0000006Er, "", "BOTBRAIN")` and the brain is driven only by
`EVENT_COMMAND` strings: `AI_level n`, `renderinstance id`, `controllable <car id>`, `AI_suspend`, then
`ai_cheat ...` (joinBotCars), `AI_BeginRace 0.5/1.0` (prepareBots) and at `beginRace` ->
`applySplines` -> `Bot.followSplineTrack` = `AI_spline <width> <splinesAI[i]> <opponent car id>`
(Bot.java:491). `sl.rpk` record 0x6E is `botbrain`, rsd `native bot` (no script). The
script never steers; the whole driver is the exe's native bot type. `Navigator.updateNavigator` (0x4df470)
is the player's GPS/minimap, not a driver.

**Exe side (all under `native bot`, vtable 0x59aaf0).**
- `0x4803a0` vf0 create: `"%d,%f"` params, `new 0x2730` (ctor 0x47ed10), `+0x2690` flag, skill
  `+0x26a8 = ((clamp01(f) - 0.5) + 1) * 0.5`, name `"BOT%d"`, frontend.rpk 0x23 resource, event mask 0x10.
- `0x480850` vf11 event handler: 0x10 = command parser (~25 commands; `FUN_0057c11e` = stricmp, strings
  around 0x59a5c8 `AI_BeginRace`, 0x59a5e0 `AI_spline`, 0x59aa78 `controllable`; controlled cars live in
  `+0x2310 + i*0x10` (count `+0x230c`, active `+0x2424`); `controllable` also sends the car
  `"control %d"`); 0x20/0x40 = its own `bot_trigger` sphere sensor contacts (obstacle list `+0x26d4`).
- `0x482630` `AI_spline` loader: reads the whole file (0x565610), per line
  `"%lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %d"` (pos xyz, dir xyz, width, ...; >5 fields kept), each
  pair of consecutive points -> `0x4122b0` spline segment of the 0x3c-byte spline object (ctor 0x411fb0)
  at `+0x2604`; then mode `+0x259c = 3`, opponent = 0x51f9b0(id), start time `+0x2720`, and sends the car
  `"transmission 0"`. Shipped .spl lines have 7 fields: `x y z<TAB>dx dy dz<TAB>width`.
- `0x47f980` per-frame update -> `0x484b90` mode switch (`+0x259c`: 2 race, 3 spline -> `0x485e10`,
  4, 5/6 traffic, 7, 8, default `0x482910` stop) -> `0x486020` planner (~600 lines: curvature speed limit
  from the spline, look-ahead target, opponent avoidance) -> `0x483bd0` (creates the `"bot_trigger"`
  sphere r 100 via 0x523c80, obstacle steering through 0x40ab10/0x40a8a0/0x40a7d0) -> `0x482910` controls.
  Spline queries: 0x414830 (project xz -> param), 0x412ed0 (tangent at param), 0x4133f0 (offset point),
  0x414a80 (width), 0x413360, 0x418b40, 0x413fd0, 0x414a00, 0x414050.
- **Direction**: every look-ahead in 0x486020 is at `param - d` (`local_28 - 20`, `local_28 - 5.1`,
  `local_28 - (local_28 - local_30) * 0.8`): bots drive the spline in **decreasing** order. Norisring
  `route_1.spl` (38 points, first == last) listed backwards matches the script's `raceposData` order
  (rp6 57.5,-430 / rp7 71.3,-503 lie near spline points 35/34, rp9..12 near points 28..25) and passes
  cp0 (48.8,-542.4) at point 28 (48.3,-538.5) just after the hairpin (points 33..29).

**Verdict (2026-10-24).** The bot is not a small blocker: first missing piece is the `native bot` manager
itself (src `create_*` dispatch only knows `native car`/`native trigger`, so `GameRef(world, sl:0x6E)` makes a
plain instance and every `AI_*` command is dropped), then the 0x2730 object, the command parser, the spline
class (~10 functions at 0x411fb0-0x418b40) and the planner/obstacle path (0x486020, 0x483bd0, 0x40a7d0..).
Checked 2026-10-24: the vmboot career event has `racers` 4 (see below). Stopgap: card 169 =
vmboot-only INVENTED pure-pursuit harness on the same `.spl` (reverse order like 0x486020) that drives
the player car to cp0. Bot cards later, in this order: (a) native bot create + `controllable`/`AI_spline`
parse + spline loader 0x482630/0x4122b0, (b) spline queries 0x414830/0x412ed0/0x4133f0/0x414a80,
(c) 0x485e10/0x486020 planner without opponents -> 0x482910 controls, (d) bot_trigger obstacle path.

**Probe + parser detail (2026-10-24, card 173).** Scratch vmboot probe: career event `JuniorCurcuit`, `racers` 4,
`gBot.length` 4, all 4 bot cars exist with bodies; each `brain` is a plain instance named BOTBRAIN, event mask 0, so
every command is dropped today. 0x480850 code 0x10 compares (stricmp 0x57c11e) 31 words in this order: device
viewport osd renderinstance camera controllable leave control lost team AI_suspend AI_NightRace AI_race
AI_BeginRace AI_spline AI_follow AI_GoToTraffic AI_GoToTrafficSlow AI_GoToTarget AI_stop AI_level AI_smooth AI_horn
AI_params AI_throttle AI_turn AI_brake AI_handbrake AI_clutch AI_gear AI_nitro; returns 1 for every code.
`ai_cheat` (joinBotCars) goes to the Bot script object, not the brain. Corrections to the list above:
- `controllable` does **not** append; it calls 0x4d2ab0 (shared with the native player, notes/09): car gets
  `activate 0x<bot> ..` + `damage_multiplier`, the bot gets `control 0x<car>`; the `control` branch appends the ref
  at `+0x2310 + n*0x10` and sets `+0x2424 = n` when getInfo(7) == 5.
- `AI_suspend` 0x4877a0: reset 0x484a40, mode 1. `AI_BeginRace f` 0x4828e0: reset, mode 8, `+0x25dc = clamp01(f)`.
  `AI_spline` format `"%*s %f %s %d"`; 0x482630 width base = `+0x2594 + width`, per-line width wins when >= 0;
  opponent ref `+0x25e0`; `"transmission 0"` goes to the active controlled car. Script order -> mode 1, 8, 8, 3.
- `AI_level f`: `+0x26a8 = ((clamp01(f) - 0.5) + 1) * 0.5` (same formula as create).
- 0x4122b0 segment record is 0x54 bytes: 0x414fe0 builds it from (prev pos, prev dir, pos, dir), `+0x30` length
  (summed into spline `+0x38`), `+0x3c` width, `+0x40..+0x50` the 5 optional fields (defaults 1.0, 1, 10000, 10000, 1).

**Card plan.** 173 (todo): create + command state + .spl rows, no driving (task card). Sketches:
- 174 planner: spline build 0x414fe0 (Hermite from pos/dir, length) + queries 0x414830/0x412ed0/0x4133f0/0x414a80,
  then 0x47f980 -> 0x484b90 mode 3 -> 0x485e10/0x486020 target + speed limit, opponents ignored (INVENTED: none
  yet). Hard check (real data): each bot car's grid pose projects onto its spline within the segment width, and
  the param of the cp0 trigger position lies at route point 28 (s8 direction note).
- 175 controls: 0x482910 writes throttle/brake/steer/gear/clutch of `controlled[active]` (CarData inputs, the same
  fields player input fills). Hard check: in --race the bot cars leave the grid and the script's race-position
  triggers (handleRacePos, CircuitRacing:262) fire for bot cars; no failed thread.
- 176 race start + obstacles: mode 8 (`+0x25dc` rev level) and mode 1 (suspended) in 0x484b90 so bots stay on the
  grid until beginRace, then 0x483bd0 `bot_trigger` sphere (r 100, 0x523c80) + 0x480850 codes 0x20/0x40 obstacle
  list `+0x26d4` and opponent avoidance in 0x486020. Hard check: bot cars do not move before raceActive, the
  script's `bot_laps` counts laps for all 4 bots by the player's finish.

### 8.1 Bot tick, car snapshot, spline, planner, controls (RE 2026-10-24, cards 174-178)

The card plan above (174 planner / 175 controls / 176 race start) is **superseded** by this section.

**Who ticks the bot and how often.** `native bot` vtable 0x59aae4 (names.json indexing: vf3 = 0x4803a0 at
0x59aaf0): vf11 = **0x47f980** (`stdcall(bot, dt)`, ret 8), vf12 0x47e960, vf13 0x4d1fe0, vf14 0x480850, vf15
0x480720. vf15 returns 1 for GII 8 only (not 9), so the brain sits on the **mode-8 control list**:
`Game__physicsTick` 0x47e050 -> `Game__runControlCallbacks` 0x47bc20(ctrlAcc) -> 0x47bc80 -> vf11 = 0x47f980,
once per physics tick before the sub-steps, dt = ctrlAcc (same list and dt as `Vehicle__updateControls`
0x498300). The list is in creation order: bot cars (Gamemode.createBotCars) are created before their brains
(Bot.enterCar), so each car reads the bot axes written on the **previous** tick. (`0x47f980` is not a camera
function; names.json now `type_BOT__vf11_tick` (was the wrong `Camera__chaseAndDebugHud`); its tail only moves the bot's own debug object +0x26f8
and debug HUD texts +0x2460.., not needed.)

**How the car reads the bot (no new car code).** The bot is the car's slot player (0x4d2ab0 `activate`), so
0x498300 reads it through `getInfo(brain, 0x43 / 0x19, axis)` = bot vf15 **0x480720**: axis table
`bot+0x21dc + v*4` (v in 1..0x4b), `0x43` returns `raw * 0.007874016` (1/127), `0x19` returns `(int)raw`;
`0x2a` returns mode `+0x259c`. Axis slots the bot writes (raw range -127..127 / 0..127):
v1 `+0x21e0` steer, v28 `+0x224c` throttle, v29 `+0x2250` brake, v31 `+0x2258` nitro, v48 `+0x229c` handbrake,
v49 `+0x22a0` clutch, v50 `+0x22a4` gear up/down (+127 / -127 pulse). Gearbox mode goes as car commands:
`"transmission 0/2/5"`, once `"abs 0"`, `"asr 0"`, `"steerhelp 0"`.

**0x47f980 vf11(bot, dt):** return if mode == 1. Drop dead control refs (+0x2310 list). If
`controlled[active]` exists, build the car snapshot `S` (0xc8 bytes on the stack) with `getInfo(car, code, out)`:
```
S+0x00 int  gear            = getInfo(0x36)  (car+0x1e5c; out=0)
S+0x04..0c  pos             = getInfo(2)     (0x4a15c0 case 2: body pos via 0x5060d0, and when the manager has
            >3 wheels: y += (1/n) * sum over 4 wheels W of (W+0x7c - (W+0x60 + W+0x50 - (|g|*M/n) / W+0x44)),
            M = 1/def+0x2c of the chassis body, |g| = |PTR_DAT_005dadf0+0x1c4..1cc|)
S+0x10..18  velocity dir    = getInfo(3) (body linear velocity, state+0x70); S+0xa4 = |v| (3D) first, then
            y := 0 and xz normalised (when the length != 0)
S+0x1c..24  forward         = getInfo(4) = -(R third column) of the body; y := 0, xz normalised
S+0x28..30  local ang. vel. = getInfo(0x33) = R^T * omega (state+0xd4); S+0x2c = yaw rate
S+0x34..3c  Euler           = 0x569970 Math__matToYpr(getInfo(0x32) = 3x4 body matrix); |S+0x3c| > 1 = rolled
S+0x40 int  wheel count min(n,4) = getInfo(0x35); per wheel i (S+0x44 + i*0x18): f0 = W+0xc0 (spin w),
            f1 = load: 0 when out of contact (S+0x50 <= S+0xf0) else S+0xf4 * (def+0x2c / |g|)  (F_N/(M g)),
            f2 = W+0x6c (friction) * (S+0xac ? S+0xac->+0x50 : 1), f3,f4 = W+0x140,+0x144 (contact slip
            velocity), f5 = W+0x148;  then f2 of wheel i /= bot+0x2710+i*4 (ctor 1.0)
S+0xa8      dt;  S+0xac rpm = car+0x1e80 * 9.549296 (0 without car); S+0xb0 redline = e(car+0x2050)+0x1c,
            default 7000, 6000 when typed+0x50 == 0;  S+0xb4 wheelbase = getInfo(0x40) (manager wheel z:
            (w2+w3-w0-w1)/2, 2.0 without wheels);  S+0xb8 nitro = car+0x1f5c / e+0xa4c when e+0xa4c > 0 and
            e+0xa58 > 0 else 0;  S+0xbc = bot+0x22a4;  S+0xc0 byte = 0, S+0xc4 (planner outputs)
bot+0x25d4 "engine running" latch: 1 when car+0x1e80 > e+0xf0, 0 when car+0x1e80 < 1.0
```
then `0x484b90(S)`. Here `car` = typed+0x4c (vehicle struct), `e` = car+0x2050 (engine).

**0x484b90 mode switch (ECX bot):** old = +0x21e0; mode 1: nothing; 2 0x4859d0; **3 0x485e10**; 4 0x484db0;
5/6 0x485600 (traffic, 5->6 when +0x2690 and a global slot is free); 7 0x487660; 8 0x484ca0 (grid);
default `0x482910(S, 0, 0)`. Then steer slew: `r = +0x2598 (ctor 510) * S.dt`;
`+0x21e0 = clamp(new, old - r, old + r)`.

**0x485e10 mode 3:** `+0x2580 = +0x2588 * 1.2` (7.2), `+0x2584 = min(+0x258c * 2, 1)` (0.8). If spline:
`look = min(|v| * +0x2558 + +0x255c, +0x2554)` (= min(10 v + 20, 55)); `0x414830(S.x, S.z, &d, &lat)`; on
success `+0x268c = d - look`, `0x486020(S, look, 1000.0)` and return; else `0x482910(S, 0, 0)`. (The
DAT_005fe210 / 0x413eb0 lines in between are a debug lap timer, no effect.)

**Ctor 0x47ed10 defaults** (relevant): +0x2554 55, +0x2558 10, +0x255c 20, +0x2578 10, +0x257c 180, +0x2580 6,
+0x2584 0.4, +0x2588 6, +0x258c 0.4, +0x2590 0 (drive type, set on first control), **+0x2594 1.4** (half car
width margin; correction to card 173's INVENTED 0), +0x2598 510, +0x26a8 0.5, +0x25c4/+0x25d0 -1,
+0x270c..+0x271c 1.0, global obstacle solver `DAT_005fe1f4` = new 0x38 (0x409f50) once.
**Reset 0x484a40** (AI_suspend/BeginRace/spline): frees spline, `+0x2608 = -1` (param floor), +0x25bc 0,
+0x25c8 0, axes +0x224c/+0x2250/+0x21e0/+0x22a0 = 0, +0x25d8 0 (max grip), +0x25f4 0.1, +0x25f8/+0x25fc 0,
+0x26a4 0, **+0x2724 = -1** (< 0 enables the curvature/crest terms below).

**Spline = the traffic `ctPath` class** (notes s2, src `traffic::Path`/`Segment`, ported for .trc). Bot-specific:
- `0x4122b0 Path::addSegment(p0, d0, p1, d1, w, f1, i1, f2, f3, i2)`: realloc n+1 records (0x54), build via
  0x414fe0, then `+0x3c w, +0x40 f1, +0x44 i1, +0x48 f2, +0x4c f3, +0x50 i2`; `length += seg.len`;
  first segment: `start = p0`, `start_dir = normalize(seg.m0)`; every segment: `end = p1`,
  `end_dir = normalize(seg.m1)`. (.spl 7-field lines: f1 1.0, i1 1, f2 10000, f3 10000, i2 1.)
- `0x414fe0 Segment::buildHermite(p0, d0, p1, d1)`: `curved = 1`; `m0 = normalize(d0)`, `m1 = normalize(d1)`;
  `L = |p1 - p0|`, `c = (p1 - p0)/L`; `f = sqrt(1/L)` (|m| = 1); `s0 = sqrt(max(0, (m0.c + 1)/2))`,
  `s1 = sqrt(max(0, (m1.c + 1)/2))`; `len = L / ((s1 f + 1 - f) (s0 f + 1 - f))`; `m0 *= len`, `m1 *= len`;
  loop { old = len; `0x4124d0` (new len); `m0, m1 *= len/old` } while `|(len - old)/old| > 0.01`;
  `+0x34 = len`; defaults `+0x40 1.0, +0x44 1, +0x48 10000, +0x4c 10000, +0x50 0` (overwritten by 0x4122b0).
- `0x4124d0 Segment::arcLength`: `len := 1`, sum of chord lengths between the Hermite points at t = 0, 0.01,
  ..., 1.00 (101 points, `t/len` with len 1; straight segments `p0 + m0 t`), accumulated as two float sums
  (i 1..50, 51..100), `len = sum1 + sum2`. Norisring route_1 (scratch python of these formulas): 37 segments,
  chord sum 2427.42 m, length 2431.41 m.
- Per-segment getters (all: negative d wraps by `+0x38`, walk while `d >= seg.len` up to n-1, i.e. the same walk
  as `path_pos_at`): 0x414a80 width `+0x3c`, 0x414980 `+0x40` (f1, speed factor), 0x414a00 `+0x44` (i1),
  0x413fd0 `+0x50` (i2), 0x413f30 `(1-t)*+0x4c + t*+0x48` (t = local d / len, km/h limit), 0x413110 side
  (0x4131a0) at d, 0x4133f0 `posOffsetAt(d, off) = segment_pos + segment_side * off`.
- `0x414050 Path::projectStrict(x, z, &d, &lat)`: `segment_project` per segment, **no endpoint fallback**
  (0x414830 = src `path_project` has one), first strict `|lat|` minimum, `d = local + sum of earlier len`.

**0x486020 planner (ECX bot, S, look, vmax0 = 1000)**, spline = +0x2604, "dirAt" = 0x412ed0, "w(q)" = width:
```
x,z = S.pos.xz; 0x414830(x, z, &d, &lat); if i1(d) == 0: S+0xb8 = -1; S+0xc0 = (i2(d) != 0)
vmax = vmax0   [traffic-density term only when +0x2724 < 0 and DAT_005e652c (Traffic) != 0: 0x418b40 bins
                DAT_005fe250/2b0; skip while src has no traffic manager in the race: bins stay 0]
// 1. lateral line offset from curvature ahead
lim = +0x2584 * 6; k = 20; q = min(length, d - 20); t0 = normalize_xz(dirAt(q)); q -= 2; c = 1; acc = 0; off = 0
if (+0x2724 < 0 && q > 0) for i < 100:
    t = normalize_xz(dirAt(q)); a = signed angle(t0 -> t) (acos(t.t0), negative when t.x*t0.z - t.z*t0.x <= 0)
    v = clamp(a * -15 * lim, -lim, lim); (bins term skipped, see above) ; acc += c
    off = (c/acc) * v + (1 - c/acc) * off; q -= 2; k += 2; c *= 0.96; if q <= 0 break; t0 = t
off = clamp(off, -0.8, 0.8)
// 2. opponent block (mode 2/3 and +0x25e8 opponent id): 0x486766..0x486942, overtaking side offset -> card 179
// 3. pull toward the current lateral position
if (mode == 3 || +0x2724 > 0): r = lat / w(d); (thr, k) = S+0xb8 > 0 ? (0.9, 0.25) : (0.2, 0.1)
    if |off - r| < thr: b = max(0, (|off - r| - thr + k) / k); off = clamp((1-b) r + b off, -0.9, 0.9)
// 4. target search, backwards from d-10 to L = +0x268c (= d - look)
f = dirAt(L).xz (raw); lo = -1; hi = 1; back = 0; q = d - 10
do { q = max(q - 5, L); h = max(w(q) - 1.4, 1); A = posOffsetAt(q, h); B = posOffsetAt(q, -h);
     C = posOffsetAt(q, (h - 1.4) * off)
     s(P): u = normalize_xz(P - (x, z)) (0 if |.| <= 1e-5); g = u.z f.z + u.x f.x;
           s = (u.x f.z - u.z f.x <= 0 ? -1 - g : g + 1) * 0.5
     if s(B) < s(A) break
     if s(C) < lo || s(C) > hi { q += 5.5; back = 1 } else { lo = max(s(A), lo); hi = min(s(B), hi); if back break }
} while (q > L)
q2 = max(d - (d - q) * 0.8, +0x2608); D = dirAt(q2); T = posOffsetAt(q2, (w(q2) - 1.4) * off)
u = normalize3(T - S.pos); step = 28; n = 100000
do { P = T + u * step; if (projectStrict(P.x, P.z, &pd, &pl) && |pl| < w(pd) && pd > +0x2608)
          { T = P; if (--n == 0) { n = 2; step *= 0.333333 } } else { n = 2; step *= 0.333333 } } while step >= 1
// 5. speed limit: braking distance look-ahead, curvature, km/h limit, crests
a0 = mode 3 ? 5 : 15; dist = a0; reach = v^2 / (2 * +0x2580) * 1.5 * (mode 3 ? 2 : 1) + a0
q = d - a0; prev = normalize_xz(dirAt(q)); prevY = dirAt(q).y
if dist < reach: while ((q -= 2) >= 0):
    t = dirAt(q); dec = clamp(+0x2580 - t.y * 9.81, 0, +0x2580); f1 = f1(q); txz = normalize_xz(t)
    kk = 1 (traffic: (Traffic+0x312c / w(q)) / 3 + 2/3)
    g = +0x25d8 = max(+0x25d8, (S.w0.f2 + S.w1.f2 + S.w2.f2 + S.w3.f2) / 4)
    mu = +0x2584 * g * (1 - t.y^2); a = acos(clamp(txz . prev, -1, 1))
    if a > 0: vmax = min(vmax, sqrt(mu * 9.81 * 2 / a / kk * f1^2 + 2 dec dist))
    if mode 3: vmax = min(vmax, sqrt(2 dec dist + (+0x258c * lerp_kmh(q) * 0.2777778 * 2.5)^2))
    dY = asin(t.y) - asin(prevY)   (0x570ac1 = asinf)
    if (+0x2724 < 0 && dY > 0.001): vmax = min(vmax, sqrt(cos(dY) * S.wheelbase * 4.905 / sin(dY) * f1^2
                                                          + 2 dec dist))
    prevY = t.y; dist += 2; if dist >= reach break; prev = txz
if S+0xc0: r = |lat| / w(d); a = max(1.8 - r, 1.01) * |v|; if max(a, vmax/2) <= vmax: vmax = max(a, vmax/2)
           S+0xc4 = min(r, 1)
0x483bd0(S, T.x, T.z, -D.x, -D.z, vmax, vmax)
```
**0x483bd0 (S, tx, tz, dx, dz, v, v2)**: front `F = S.pos.xz + S.fwd.xz`; `V = (tx, tz) - F`;
`0x40ab10` on the solver (F, V, +0x2594, 0): stores F, `len = |V|`, `dir = V/len`. If `+0x2700 == 0` create
the `bot_trigger` sphere (`"%.2f,%.2f,%.2f,0,0,0,sphere,%.2f"`, r 100, 0x523c80) - card 179; walk obstacle
list +0x26b4 (types 1/2, 0x409fa0 adds blocked angle intervals) - card 179. `0x40a8a0(&V, +0x269c, +0x26a0)`:
with no intervals the heap is empty, the chosen angle is 0, so `V` comes back unchanged (`len * dir`);
`+0x269c/+0x26a0 = V`; `c = normalize(V)`; `ang = 0x40a7d0(c, S.fwd.xz)` (`g = c.f`; `x = c.x f.z - f.x c.z`;
`x <= 0 ? (g > 1 ? 0 : g < -1 ? -pi : -acos g) : (g > 1 ? 0 : g < -1 ? pi : acos g)`); `+0x25f0 = 0`;
**`0x482910(S, steer = -ang, speed = max(0, v2))`**.

**0x482910 controls (S, steer, speed):**
```
if |S+0x3c| > 1 (rolled): +0x25c8 += S.dt; crossing 5.0 -> queue "ai_info_rolled" to the bot's parent (+0xc);
    zero +0x22a0 +0x224c +0x2250 +0x229c +0x21e0 +0x2258; return
+0x25c8 = 0
+0x25bc 1/2/3 and |v| < 0.5 -> 4/5/6 (+0x25c1 = 1); 7 -> 0          (stuck/reverse state, card 179)
if +0x25bc in 4..6: 0x482f50(S, steer) (reverse out); return
switch +0x2590: 0 -> queue "abs 0", "asr 0", "steerhelp 0"; type = getInfo(car, 0x34) (0 none, 1 -> 4,
    2 -> 2, 3 -> 3, 4 cross -> 1, else 1); 1 -> 4; recurse 0x482910 once.
  2..5: 0x483220(S, steer, speed)   then (all non-zero):
  if +0x26a4: 0x4830a0(S)   (launch: set by AI_spline)
  if |v| < 1 && +0x224c > 16 && gear < 7: +0x25d0 < -0.5 -> +0x25cc = 1, return;  now - +0x25d0 <= 3 -> return;
      else +0x25c1 = 1, +0x25bc = 5;   then +0x25d0 = -1
0x482da0 gate(S, steer, speed): axes := clutch 0, throttle 127, brake 0, steer 0, nitro 0;
  shift pulse +0x22a4 toward 1st/reverse: gear g = S.gear: (g <= 0 or g == 7) and speed > 1e-4 -> +127 until
  in gear; g in 1..6 and speed < -1e-4 -> -127; g == 0 with tiny speed and |v| <= 1 -> pulse; returns 0 while
  shifting. In gear: +0x22a4 = 0; engine latch +0x25d4 == 0 -> throttle = brake = clutch = 127, return 0;
  else (clears +0x25c1/+0x25cc timestamps) return 1.
0x483220 drive (S, steer, speed)  [after gate == 1]:
  skill = min(2 * +0x26a8, 1); load = sum f1 of 4 wheels;  wheel spin term ws by drive type (2: 5*(f3 w0 + f3 w1)
  + f3 w2 + f3 w3, cap 1;  3: sum f3, cap 10;  4: 3*(front) + rear, cap 5; else steer)/(|v| + 1)
  +0x25d8 = max(+0x25d8, avg f2)
  yaw damping: y = ((|v| * 0.4 * 0.036)^2 + 0.2) * S.yawrate, dead zone 0.05; steer' = steer, if y*steer <= 0 and
  |steer| < |y|: steer' = clamp(2 steer + y, sign-limited 0.1); speed fade (|v| - 10) * 0.0005 toward 0;
  rate limit by S.dt * |v| * 2 against S.wheelbase, clamp [0.2, 1.2] * |steer'|, clamp +-0.75  -> st
  understeer allowance u = clamp(S.wheelbase / (v^2 / ((f1w0 f2w0 + f1w1 f2w1) 9.81 / (f1w0 + f1w1))), 0, 0.75),
  doubled in mode 3 (x4 when S+0xc0); slip a = acos(fwd.z vel.x - fwd.x vel.z) - pi/2;
  +0x21e0 = clamp(st * -169.33333, -max(cap, (max(a,0) + u) * 169.33), max(cap, (u - min(a,0)) * 169.33)),
            caps <= 127 (cap = 1, 10 or 5 from the drive type)
  speed target: sp = min(speed, |v|); decel estimate (+0x25f8, +0x25fc, +0x25f4 = S.dt); if |st| > 0.375 and the
  steer is not growing fast: speed = min(speed, sqrt(+0x25d8 * 9.81 * S.wheelbase / (2 (|st| - 0.375))))
  e = clamp((0.1 - (|v| - speed) / ((speed + 1) * 0.1)) - clamp(2 - speed, 0, 2), -1, 1.1)
  throttle cap = min(load*500+40 (mode 3), 127 - (|ws|*150 - 20), 128 - (skill - 1) * -28, 127);
  +0x224c = clamp(int(e * 127), 0, cap); +0x22a0 clutch = clamp(127 - throttle*10, 0, 127);
  nitro +0x2258 = 127 when S.nitro > 0 && e > 1.05 && cap == 128 && rpm > 0.33 redline && gear > 1
  brake = clamp(int(e * -127), 0, 127) limited by the front/rear slip caps (423.33 / 211.67 terms, x2 with
  S+0xc0), the load cap and 127 - |yaw + st| * 300  -> +0x2250; handbrake +0x229c = the rest up to the
  127 - |yaw + st| * 200 cap
  gearbox: "transmission 5" when |steer axis| <= 5 or rpm >= 0.9 redline or rpm <= 0.2 redline, and load >= 0.001;
  else "transmission 0"  (sent every tick to controlled[active])
0x4830a0 launch (once after AI_spline, +0x26a4): if brake/handbrake axes are 0 and (rpm <= 0.8 redline or
  sideways slip >= 0.3 or |v| <= 5): "transmission 2", +0x22a4 = 127 unless gear > 0 or S.bc >= 127,
  throttle 127, brake/handbrake 0, nitro 127, clutch max(80 - |v| * 20, 0); else "transmission 5", +0x26a4 = 0.
```
Globals: 0x5da4bc 0.4, 0x5da4c0 0.05, 0x5da4c4/c5 = 1/1 (obstacle switches). `0x5706e4` = acosf,
`0x570ac1` = asinf, `0x570e7e`/`0x570fcb` = cosf/sinf (by usage).

**Untraced (card 179):** 0x484ca0 (mode 8 grid rev), 0x482f50 reverse already decompiled (sets +0x2710..+0x271c
to 1, 0x4876a0, steer = 169.33 * steer, throttle 64, ends in state 7), 0x409fa0/0x46b5e0/0x40abe0 obstacle
intervals, the bot vf14 0x20/0x40 trigger contacts (+0x26d4), the opponent block of 0x486020.

**Cards.** 174 spline build + strict queries · 175 bot tick + snapshot + axes plumbing (mode 3 only, no planner
output yet) · 176 planner steps 1, 3, 4 (target point) · 177 planner step 5 + 0x483bd0 no-obstacle steering ·
178 controls 0x482910/0x482da0/0x483220/0x4830a0 (bots drive, rpTotal for bot cars) · 179 sketch: mode 8 +
bot_trigger obstacles + opponent block + reverse recovery.

## 9. Career circuit race: laps, finish and exit (script side, CircuitRacing.java; RE 2026-10-24, cards 171/172)
Exe provides only triggers/timers/OSD/controls; the rules are script (memory: exe is a shell).
- Norisring career event (vmboot): `laps = gmcEvent.specialSlots[0]` = 2, `cpTrigger.length` 4, `fines` null (no
  useFines, so no runout/pit logic), `raceActive` 1 after the countdown.
- `handleCheckpoint` (507): player cp i == nextTrigger[0] -> nextTrigger[0]++ until the last cp (start line, index 3);
  there `current_lap+1 <= laps` -> current_lap++, `nextTrigger[0] = 0`, else `stopTimer(0)`, `gmThread.execute(7)`.
  Lap 1 -> lap 2 needs cp0..cp3 once; the finish needs them twice (8 checkpoint hits).
- THOR method 7 in `run()` (1183): raceActive 0, clearScreen, `controller.command("controllable 0")`,
  `finishRace(0)` (Gamemode 1218: `erWindow = new endRaceWindow(osd, 0)`), timers 12 (2 s, stopCars) and 11 (4 s).
- Timer 11 (handleEvent 392): erWindow.hide until windowStatus -1, then `fWindow = new finishWindowCircuitRacing(this,
  bot_laps)` (prize via `gmcEvent.getPrizeCash`). `finishWindowCircuitRacing.osdCommand(0x01)` -> hide(); on
  windowStatus 3 it sets `game.addTimer(1.0, 13)`; timer 13 waits for windowStatus -1 then `unlaunch()` ->
  `gTrack.getOut()` (Track 2831): freeride -> EventList(FREERIDE); `!game.failable` -> `GameLogic.changeActiveSection(
  new Garage())`; failable (career) -> `new EventList(EventList.MODE_AMATEUR)`. So a career race returns to the
  EventList, not directly to the Garage.
- First blocker after cp0 found by the probe: physics (notes/11 "Contact refresh 0x4f9af0"), card 171. Finish flow
  (erWindow/fWindow/Enter/exit) is card 172 and is untested until 171 lets the car lap.
