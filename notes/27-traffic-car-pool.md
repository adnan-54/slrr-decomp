# 27 — Traffic car pool: init/registration, addTrafficCar, lane car_rem (RE 2026-10-25)

Follow-up of notes/19 §2 and notes/25 §2.4. Ghidra session with analysis/names.json subset applied.

## 0. What shipped scripts exercise, and what vmboot sees
| native | shipped callers | reached? |
|---|---|---|
| addTrafficN 0x4dc9e0 (`map.addTraffic`) | Gamemode.setupCity:295-336, Valocity.java:95-110 | City / Valocity only (`Integrator.isCity`, Track.java:1057) |
| addTrafficP 0x4dcdc0 | City.java:818 (police scout) | City only |
| addTrafficCar 0x4dd090 | Bot.joinTraffic (Bot.java:373) | **never**: no shipped script calls `joinTraffic` |
| remTrafficCar 0x4de520 | City.java:889, Bot.leaveTraffic | City only |
| notifyTrafficCar / setTrafficCarBehaviour | Bot.java:299 (on `ai_info_entertraffic`), City.java:2311/2337 | City only, and gated by `traffic_id != 0` |

The bot side enters traffic natively: `AI_GoToTraffic` (cmd parser 0x480850) -> 0x485600 (the bot
drive step) calls 0x41aa80 (find lane slot, wrap the car) then `getInfo(car, 0x38, &tc)` and sends
`ai_info_entertraffic 0x<tc> ...` to the brain; that is the real producer of Bot.traffic_id, not
addTrafficCar. 0x4a2e60 sends `ai_info_leavetraffic`.

**vmboot**: `--selfcheck`/`--race` go Garage (isCity = 0, Garage.java:207) -> career race
JuniorCurcuit (Track, isCity 0). City.java/Valocity.java are never entered, `setupCity` never runs, no
traffic native is called, and the only sink consumer (City.handleEvent car_add/car_rem) is never
instantiated. **Nothing in this note changes anything observable in vmboot today.** It only matters
once a City free-ride run exists in the harness, and then it also needs the per-frame traffic sim
(type_ground vf13 0x4bac10 -> 0x41a9d0/0x41b290/0x417c90, untraced) to move or spawn anything.

## 1. (a) Creation, init and registration (type_ground vf3 0x4bade0, traffic block)
asm 0x4bb2a8..0x4bb2e8 (ECX values checked in the disassembly):
```
if (params trc name != "" && File__readAll(trc) >= 0) {
    T = new Traffic (0x3144) -> 0x416000;  ground+0x88 = T;
    ResRefLink(T+0x3130).set(ground+0xc);   // event SINK = the ground's parent instance (City GameType)
    Traffic__load(T, buf); free(buf);
    Traffic__init(T, *g_physWorld /*sim time, 0x5dadf0*/);
    Game__registerCallback(ground, 0x1c);   // animate list game+0xc0 (notes/09): per-frame vf13
}
pedestrians = new 0x35c0 -> 0x41b570(simtime)   (only when a trc name is present)
```
`Game__registerCallback` 0x47d670: modes 8/9/0x1c only, tail-append a 0x34-byte node; src has it
(runtime.cpp register_callback). Nothing in src walks mode 0x1c yet.

### 0x419da0 Traffic__init(T, t)
```
DAT_005e652c = T; T+0x84 = t;
if (T+0x3118 /*player pseudo-car*/) { pc+0xe8 = pc+0xe4 = pc+0xe0 = t; }   // 0 right after the ctor
T+0x312c = 0;                                     // max road width
for each crossEq:    0x415650(cross)
for each crossLight: 0x415700(cross)
for each road:       0x40e260(road)
for each lane:       0x40e150(lane)
m = max(0, max over roads of (int)road+0x64);
T+0xc4 = byte[m+1]; [0] = 0; [1..m] = 1;           // per road-group enable flags
```
### 0x415650 ctCross init (crossEq; also the first step of crossLight)
```
0x40e0d0: cross+0x38 = 0; cross+0x34 = max over links |link.lane.endpos(link.flag) - cross.pos|
          (endpos = lane+0x1c for flag 0, lane+0x28 for flag 1; strict >, starts 0)
cross+0x58 = realloc(nLink*8), all zero
mx = 0; mn = paths[0].c (u16 at path entry +2)
for each path: mx = max(mx, c); mn = min(mn, c)        (unsigned)
mx -= mx % 10; if (mx == (mn/10)*10) mx += 10;  cross+0x5c = mx
```
### 0x415700 ctCrossLight init
```
0x415650(cross); cross+0x6c = T+0x84;
cross+0x70 = realloc(nLink*4); cnt = 0; prev = links[0].lane+0x10 (road)
for i: if (!links[i].flag) phase[i] = -1 (prev unchanged)
       else { r = links[i].lane+0x10; if (r != prev) cnt++; phase[i] = cnt; prev = r; }
cross+0x74 = cnt + 1; cross+0x5c = 0;
```
### 0x40e260 ctRoad init (asm-read; consts 1.0, 1.1 @0x593714, 0.9090909 @0x593710, 1.75 @0x59371c)
```
n = 0; for (l = road+4 first lane; l; l = l+0x14 next) n += 1 - (l+0x88 != 0) - (l+0x8c != 0);
for dir in 0,1:
    f = 1.0; for l in lanes: if (l+0x18 == dir && !l+0x88 && !l+0x8c) f *= 1.1;
    for l in lanes: if (same test) { f *= 0.9090909; l+0x98 (float) *= f; }
w = n * 1.75; every segment of road path (+0x28): seg+0x3c = w;
if (w > T+0x312c) T+0x312c = w;
```
### 0x40e150 ctLane init
```
lane+0xc8 = 0;
if (lane+8 /*first traffic car*/) { ... 0x4107c0 car step, 0x417050 timed event ... }  // no cars at init
lane+0xc0 = T+0x84; lane+0xcc = lane+0x74 (lane path total length); lane+0xc4 (byte) = 0;
```

## 2. Traffic car (0x164 bytes, pool at Traffic+0x70) — fields seen
+0x14 length, +0x20/+0x24 gaps, +0x2c max speed, +0x30 accel, +0x38 decel, +0x88 pos on path,
+0xa8 lateral, +0xb0 speed, +0xb8/+0xbc next lane, +0xc4 lane, +0xc8 path, +0xcc dir (-1 = reverse),
+0xdc "materialized" byte, +0xdd notify byte, +0xe0/+0xe8 last-step times, +0x128 lane change,
**+0x130 ResRefLink** {prev, next, +0x138 instance id, +0x13c instance}: the car game object driven
by this traffic car, +0x144 ResRefLink to the spawned object, +0x14c car type, +0x154 instance
seed/id, +0x160 behaviour. Ids handed to script are the raw pointer.
`0x4107c0` = per-car step (car following, lights, lane change; ~600 lines, not carded).

## 3. (b) addTrafficCar 0x4dd090 and type_car getInfo 0x38/0x45
```
(inst, vec) = args; if (!inst) { log "vm_ground_addtrafficcar: Traffic car instance is null in ..." ; return 0 }
T = getInfo(ground, 0x39); if (!T) return 0;
if (!vec) tc = random lane among lanes with lane+4..+0xa8 == 0 -> ctLane__spawnCar(1,2,4,1)
else      tc = Traffic__spawnCarNear(vec, 0, 1.0, 2.0, 4.0, 1.0)
if (tc) {
    ResRefLink(tc+0x130).set(inst+0xc);           // asm 0x4dd376
    getInfo(inst, 0x38, &tc);                     // type_car: hand the car to traffic
    tc+0xdd = getInfo(inst, 0x45) != 0;           // notify flag (M+0x2ec0 bit 1)
    0x4ec210(...); tc+0x2c = 19.444 + r*0.000339; tc+0x30 = 0.7 + r*1.22e-5; tc+0xa8 = 2r-1; tc+0x154 = 0;
    0x4107a0(tc) -> 0x410680(lane+0x10 road +0x68)
}
return tc
```
type_car vf15 0x4a15c0:
- case 0x45: `return (M+0x2ec0 >> 1) & 1`.
- case 0x38 (p = &tc): T+0x2054 = current traffic car. Same -> return. tc = 0 (release): queue
  `obs_add <T.id>`, rebind, T+0x58 = old tc+0x154, re-register callbacks 8 and 9 (physics/drive on).
  current 0 -> tc != 0 (take over): 0x4ae5a0, zero wheel states, unregister callbacks 8/9 (car
  becomes kinematic, driven by the traffic step), restore links, T+0x2054 = tc. Returns T+0x2054.
Not carded: no shipped script reaches addTrafficCar, and the take-over needs the traffic per-frame
step to drive the frozen car; porting it alone would freeze a car that nothing moves.

## 4. (c) car_rem / car_add senders (all to the Traffic+0x3130 sink = City)
String xrefs: car_add in 0x40cbd0, 0x40d4e0, 0x40f5e0, 0x40f6d0; car_rem in 0x40cbd0, 0x40de70,
0x40ebb0, 0x40ed30, 0x40f350, 0x40f5e0, 0x40fa50, 0x410680. Shape shared by 0x40ebb0 (lane list
walk, +8 chain), 0x40ed30 (road: every lane), 0x410680 (single car):
```
id = tc+0x138
if (tc+0x14c == 0) { if (id) bind(tc+0xdc && sink+8 ? sink : T+0x311c) }
else if (!tc+0xdc || !sink+8) {          // dematerialize
    if (id) { if (tc+0xdd) queue(T+0x3130, 0x10, "car_rem %d %d", id, tc);
              tc+0xe8 = T+0x84; 0x417080(tc+0x14c, &tc+0x130); unlink tc+0x130; counters }
} else if (!id) { counters; 0x41a500(tc) /* queue materialize */ } else bind(sink)
```
0x40d4e0 (materialize): creates `"x,y,z,rx,ry,rz,seed"` `traffic_car` object (0x523c80) or reuses a
pooled instance (0x418f60), `getInfo(obj, 0x38, &tc)`, then `car_add <id> <tc>` if tc+0xdd.
ctCross__halt 0x40de70: for halted materialized notify cars, `car_rem <id> <tc>`, clear +0xdc/+0x128.
All of these need the pool, per-frame step and materialize; they are the per-frame sim, not cardable
before 0x41a9d0/0x41b290/0x417c90 are traced.

## 5. Init.java recompile vs `Steam.isReady` (claim in notes/26)
Wrong on both counts:
- The `Steam.isReady()`/`runDLL` loop in `game/Init.java` lines 149-183 is inside a `/* ... */`
  comment (also `implements Runnable` is commented on line 11). `src/Init.java` is byte-identical.
- `isReady` exists: `system/scripts/util/Steam.class` has methods initAPI, neutralize, getPackage,
  setAchievement, resetAchievements, init, postInit, getPlayerName, **isReady**, finalize (script
  method, not a native). Only `STEAM_MODULE`/`getWorkshopNumSubscribedItems`/`setWorkshopItems`
  are missing, and they are in the comment too.
So a recompile of Init.java (src 04:29:26.19 vs class 04:29:28.00 on this copy: times differ, so the
0x5598f0 rule would recompile) cannot fail on these lines.

## 6. Card
- 210 Traffic__init + ground registration (§1). (b)/(c) deliberately not carded (unreachable /
  need the untraced per-frame sim). Next RE: type_ground vf13 0x4bac10 -> 0x41a9d0, 0x41b290,
  0x417c90; addTrafficN 0x4dc9e0 -> ctLane__spawnCar 0x40ce00; materialize 0x40d4e0; and a vmboot
  City run to make any of it observable.
