# 30 — remTrafficCar / notifyTrafficCar, road objects and live lanes (RE 2026-10-25)

Follow-up of notes/29 (§5 instance side, §8 not carded). Ghidra session with the traffic subset of
analysis/names.json applied; 10 new names added (Traffic__bindObject, ctRoad/ctCross__bindObject,
ctLane__setLive, TrafficCar__setLive/dropLive/unlinkLane, Camera__instanceLod,
Render__collectInstance). `T` = Traffic, `car` = traffic car (notes/29).

## 0. Script callers (shipped `.java`, same in `src/`)
| native | callers | vmboot `--city` |
|---|---|---|
| notifyTrafficCar(id, 1) | Valocity.java:164 (right after each of the 45 addTrafficCar calls); Bot.java:299 (on `ai_info_entertraffic`) | yes, 45 calls |
| remTrafficCar(id) | Valocity.java:266 (leaving Valocity for a non-RaceSetup section: every speedyman dummycar); City.java:889 (race bot back to traffic); Bot.java:394 (leaveTraffic) | no (the run stops inside Valocity) |
| setTrafficCarBehaviour(id, mode) | Bot.java:363 | no (Bot traffic ids come from the native AI_GoToTraffic path) |
Ids are the raw car pointer in the exe; src hands out `index + 1` (card 213).

## 1. Natives
### 0x4de520 GroundRef.remTrafficCar(int car) (asm of TrafficCar__remove checked)
```
if (car) {
    if (car+0x138 /*instance id*/) { zero = 0; Game__getInfo(&car+0x130 /*instance link*/, 0x38, &zero) }  // type_car take-over release
    TrafficCar__remove(car)       // 0x40f290, ECX = car
}
```
No Traffic lookup: the remove uses the global `g_traffic` (DAT_005e652c).

### 0x4de5f0 GroundRef.notifyTrafficCar(int car, int flag) -> int
```
flag = 0 (default before getArgs)
if (!car) return 0
if (flag >= 0) car+0xdd = flag != 0
return car+0xdd                   // byte
```
### 0x4e6280 GroundRef.setTrafficCarBehaviour(int car, int mode): `if (car && car+0x138) car+0x160 = mode` (notes/27, src comment).

### 0x40f290 TrafficCar__remove(car) (asm-checked)
```
if (car+0xc == 0) return                       // not on a lane: nothing (also not freed)
TrafficCar__unlinkLane(car)                    // 0x40ca40
if (car+0xc4) car+0xc4->vf4(car, car+0xcc)     // PathRef object: lane (0x40e710) or cross
car+0xb8 = car+0xbc = car+0xc4 = car+0xc8 = car+0xd0 = car+0xd4 = 0; car+0xc = 0
if (car+0x44) {                                // in the live list (end = sentinel T, so the last car is != 0 too)
    unlink car from T+0x88 (walk the +0x44 chain from the head)
    car+0x44 = 0
}
TrafficCar__release(car)                       // 0x40c730
car+0 = pool.free_head(T+0x70+0x10); pool.free_head = car
```
### 0x40ca40 TrafficCar__unlinkLane(car) — doubly linked lane list (lane+8 head, lane+0xc tail; car+4 prev, +8 next, +0xc lane)
Plain unlink fixing head/tail, then `car+4 = car+8 = car+0xc = 0`.
### 0x40e710 ctLane__vf4(lane, car, dir) — detach (RET 8; dir unused)
```
lane+0xc8 -= car+0x24 + car+0x14 + car+0x20; if (lane+0xc8 < 0) lane+0xc8 = 0
f = lane+8                                     // new first car (car already unlinked)
if (f) {
    TrafficCar__step(f)
    stop = f+0xbc ? 0 : T+0x3140
    v = max(TrafficCar__targetSpeed(f), 10.0)  // (<= 10 -> 10)
    if (lane+0xac == 0) { Traffic__scheduleLane(max(0, (f+0x88 - f+0x14 - stop) * (2/v)), lane); lane+0xac++ }
}
if (lane+0x34 == -1) car+0x124 = 0
```
= `timeToLaneEnd(f)` (0x40d990) inlined; no live check (a live lane also gets the event).

**Live list sentinel** (Traffic__ctor 0x416000: `T+0x88 = T+0x8c = T+0x90 = T+0x94 = T`,
`T+0xc8 = T`; Traffic__step 0x41b290 walks `while (node != T)`). So `car+0x44 == 0` means "not in
the list", and the last car holds T. src (card 214 `lane_attach`) uses 0 as the list end, so its
last live car looks unlisted: a second attach would re-insert it, and remove would skip the unlink.
Card 216 fixes this with an end marker.

## 2. What makes a lane live (and so real traffic_car objects)
Chain, every link decompiled:
1. **Map-placed road objects.** city.rpk spatial records with gametype 0x15 (`_type_road`, 755
   objects, notes/19 §3.1). Their typed create **0x4d42a0** (`type_road` vf3, vtable 0x5a005c):
   alloc 0x20-byte typed data, link +0 to the instance, `ground = parent`, `T = getInfo(ground, 0x39)`;
   `obj+0x1c = Traffic__bindObject(T, params, obj)`; when found, set typed flag `|= 0x4000` and 0x4ebdd0(0x100).
2. **0x41a3e0 Traffic__bindObject(params, obj)**: `sscanf("%d;%d,%d", kind, graph, idx)`, `graph == T+0xc`:
   kind 1 -> `ctRoad__bindObject` 0x40ed30 on roads[idx], returns the road; kind 2/3 ->
   `ctCross__bindObject` 0x40ebb0 on cross_eq/cross_light[idx], returns cross+0x10 (its second
   vtable). (asm-checked ECX for all three.) The parser part is src `pedestrians.cpp` (card 100).
3. **0x40ed30 ctRoad__bindObject(road, obj)**: `ResRefLink(road+0x68).set(obj)` (ECX not asm-checked),
   then for every car of every lane the notes/27 §4 shape: no type -> reparent its instance under obj
   (live) or the CarContainer T+0x311c; typed + (not live or obj id 0) -> `car_rem` + park + unlink;
   typed + live + no instance -> counters + `Traffic__queueSpawn` 0x41a500; else reparent.
   0x40ebb0 is the same for the cars on a cross (cross+8 chain).
4. **Per frame visibility**: render scene walk **0x45f180** -> **0x461980 Camera__instanceLod**
   (`f = min(1, sqrt(k*r^2/d^2)) + cam+0x4ac`, r from the instance bounds +0xb0 kind, d to camera
   pos cam+0x350; 1.0+bias when bounds kind 2) -> instance `vf5(f)` and `vf6(f)` -> typed vf5.
   The instance-vf5 -> typed-vf5 forwarding is inferred, not traced.
5. **0x4d44d0 type_road vf5(obj, f)**: `if (!obj+0x1c || !obj+0xc) return; if (f != 1.0) { r = bounds radius
   (kind 1: |half extents +0xc0..+0xc8|, kind 0: +0xc0, else 0); x = r*0.7/(DAT_005da348*f) - r;
   if (x >= 0 && 2.1/x <= DAT_005f06d4 /*LOD threshold*/) return }  obj+0x1c->vf1()`.
6. road vf1 **0x40f8e0**: every lane `vf7` **0x40eed0**: `ctLane__setLive(lane, 1); lane+0xc0 = T.sim_time` (keep-alive stamp).
7. **0x40f6d0 ctLane__setLive(lane, on)** (if changed): global live-lane count; on: both end crosses
   (road+8, road+0xc) refcount +0x38 (0 -> cross vf10 = push on T+0x90/T+0x94 active list, <0 -> 1),
   every car not live -> live (count, `car_add` to the sink when untyped + instance + notify, live list),
   lane on the T+0x8c active list; off: cross refcounts -1 (0 -> -1), every car 0x40fa50 (drop live,
   `car_rem`), first car -> timeToLaneEnd + scheduleLane when no event is pending. Then
   `TrafficCar__syncInstance(car, road+0x68)` for every car (queues the spawn of typed cars).
8. **0x40f4e0 ctLane__activeStep** (per frame, T+0x8c list): live and `sim_time - lane+0xc0 <= 0.5` ->
   per car without next lane: step + 0x40e460 (not traced); else `setLive(0)` and drop from the list.
   So a lane stays live only while its road object passes the vf5 test at least every 0.5 s.
9. The spawn queue (notes/29 §5) then materializes `traffic_car` objects in 0x417c90 -> 0x40d4e0.

Road object destroy: road vf0 **0x410250**: unlink road+0x68, `setLive(0)` on every lane, sync every car.

### Why this is not carded
src has neither link of the driver: map RPK spatial records are not instanced as game objects
(no `_type_road` create; resman only counts their params), and there is no render scene walk
(0x45f180/0x461980 run per camera in the renderer). Porting setLive/bindObject alone would be dead
code; a headless "visible" rule would be INVENTED behaviour. Next RE, in order: (a) who creates
map spatial-record instances on map load / streaming (start at the ground's spatial tree, notes/24);
(b) instance vf5 -> typed vf5 forwarding; then card bindObject + setLive + activeStep expiry + a
vmboot driver from (a)/(b). 0x40e460 (next-lane move) and 0x4107c0 stay with the per-car step.

## 3. Card
- 216 remTrafficCar / notifyTrafficCar / setTrafficCarBehaviour + TrafficCar__remove (+ unlinkLane,
  ctLane vf4) + live-list end marker.
