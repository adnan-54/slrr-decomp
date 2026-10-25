# 29 — Traffic spawn side: addTrafficN / addTrafficCar, car pool, lane attach, event heap (RE 2026-10-25)

Follow-up of notes/27 §2-4 and notes/28 §3. Ghidra session, analysis/names.json applied; new
names added to names.json (31 entries, `0x40ce00` renamed `ctRoad__spawnCar`: its ECX is a road).
Asm-checked where marked. `T` = Traffic (0x3144 bytes), `car` = traffic car (0x164 bytes).

## 0. Big picture
A traffic car is an **abstract lane record**, not a game object. `addTrafficN` / `addTrafficCar`
only put records into lane lists. A game instance (`traffic_car` scripted object) is attached
later, per frame, by the spawn queue (`0x417c90` -> `0x40d4e0`), and only for cars on a *live*
lane (`lane+0xa8`) whose road object is streamed in (`road+0x68` link bound by the map's
`_type_road` objects, notes/19 §3.1). Off-camera lanes are simulated in coarse time steps by a
min-heap of lane events (`T+0`), on-camera ones per frame through the active lists (notes/28 §3).

In src nothing binds road objects (`road+0x68`) and nothing sets `lane+0xa8`, so in a vmboot City
run every spawned car stays abstract: no instance, no `car_add`. What *is* observable: return
values of the natives, the pool, lane lists, and the lane event heap.

## 1. Natives
### 0x4dc9e0 GroundRef.addTraffic(GameRef type, int n, float a, float b, float c) -> int (asm-checked)
Arg unpack 0x4b1af0 order: self, type, n, a, b, c. Locals before the unpack: n=1, a=1.0, b=4.0,
c=2.0, d=0.5 (d has no script arg).
```
G = instanceToHandle(self); T = getInfo(G, 0x39)
if (n < 1 || !T) return 0
density = Config__getFloat("trafficDensity")
make type resident (res+0x7c != 1 -> vf5(1.0); then vf3(1.0))
getInfo(type, 0x40, &c)        // type_car vf15: overwrites c (see §4), 2.0 without wheels
getInfo(type, 0x46, &d)        // overwrites d
notify = getInfo(type, 0x45, 0) // (M+0x2ec0 >> 1) & 1  (src CarData::notify)
k = min(n, (int)(density * 20.0 /*0x593854*/))
repeat k: inst = createScripted(type, "0,-10000,0,0,0,0", "traffic_car"); T.parkInstance(type.id, inst)  // 0x417080
spawned = 0
repeat n:                      // NOT k: abstract cars do not need an instance each
    r = rand15 * (1/32768) * (float)(nRoads - 1); road = roads[(int)r]   // last road never chosen
    car = ctRoad__spawnCar(road, 1.0, 2.0, 4.0, 1.0)                     // constants asm-checked 0x4dcbfc..
    if (!car) continue
    car.type_link(+0x144).set(type)   // +0x14c type id, +0x150 type resource
    car+0x154 = (rand15 * 0xffff) >> 15                                  // instance seed
    car+0x2c  = rand15 * 0.00023735895 + 13.611111                       // max speed m/s (49 km/h)
    car+0x30  = rand15 * 1.2207031e-05 + 0.7                             // accel
    car+0x18 = c; car+0x1c = d; car+0x14 = a; car+0x20 = b; car+0x24 = 1.0
    car+0xdd = notify != 0
    car+0xa8 = (rand15 * (1/32768)) * 2 - 1                              // lateral offset
    TrafficCar__refreshInstance(car)   // 0x4107a0 -> 0x410680(road+0x68)
    spawned++
return spawned
```
rand15 = `msvc_rand() & 0x7fff` (0x571110, src `msvc_rand`). Every rand call above is in this order.

### 0x4dd090 GroundRef.addTrafficCar(GameRef inst, Vector3 pos) -> int
```
if (!inst) { log "vm_ground_addtrafficcar: Traffic car instance is null in \"<file>\" at line <n>"; return 0 }
G = instanceToHandle(self); if (!getInfo(G, 0x39)) return 0
car = 0
if (!pos) {
    m = #roads whose first lane (road+4) has +0xa8 == 0 (not live)
    if (m) { r = (int)(rand15/32768 * m) % m; road = r-th such road (road order); car = spawn(road) }
    else   { r = (int)(rand15/32768 * (nRoads-1)); car = spawn(roads[r]) }      // also when nRoads == 0: guarded by address != 0
} else car = Traffic__spawnCarNear(pos.x, pos.y, pos.z, 0.0, 1.0, 2.0, 4.0, 1.0)
if (car) {
    car.inst_link(+0x130).set(inst)          // +0x138 instance id, +0x13c instance
    getInfo(inst, 0x38, &car)                // type_car vf15 0x38: car taken over (notes/27 §3)
    car+0xdd = getInfo(inst, 0x45) != 0
    0x4ec210(G)                              // reparent (ECX not asm-checked: the instance under the ground)
    car+0x2c = rand15 * 0.00033908422 + 19.444445   // 70 km/h
    car+0x30 = rand15 * 1.2207031e-05 + 0.7
    car+0xa8 = rand15/32768 * 2 - 1;  car+0x154 = 0
    TrafficCar__refreshInstance(car)
}
return car   // raw pointer
```
spawn(road) = `ctRoad__spawnCar(road, 1.0, 2.0, 4.0, 1.0)`. Unlike addTrafficN, the type link
(+0x144) stays empty: the instance is the script's own car.

### 0x416800 Traffic__spawnCarNear(x, y, z, dist, p1..p4) (asm-checked, RET 0x20)
```
X = Traffic__nearestCross(x, y, z, dist, 0); if (!X) return 0
N = X.nlinks(+0x18); free = #links with !lane.halted(+0xc4); live = #links with !halted && lane+0xa8
if (free == 0) return 0
if ((free == N && (live == 0 || live == N)) || (free != N && live == N /*impossible*/)) {
    lane = links[rand15 % N].lane          // may be a halted lane
} else if (live == 0) {
    lane = (rand15 % free)-th free link lane
} else {
    lane = (rand15 % live)-th link with lane+0xa8 && !halted
}
return ctRoad__spawnCar(lane.road(+0x10), p1..p4)    // the cross only selects a road
```

## 2. Car creation on a road
### 0x40eef0 TrafficCarPool__alloc (pool = T+0x70: +0 page table, +4 pages, +8 count, +0xc cap, +0x10 free head)
Free head first (next stored in car+0); else slot `count++` in pages of 0x400 cars (0x59000 bytes).
### 0x40c400 TrafficCar__ctor (+ 0x40e040)
`car+0 = ++g_car_serial (DAT_005e6508)`; zero: +4, +8, +0xc, +0x10, +0x44, +0xb4, +0xb8, +0xbc,
+0xc4, +0xc8, +0xd0, +0xd4, +0xf4, +0xf8, +0x104 (byte), +0x124, +0x128, +0x12c, +0x130..+0x13c,
+0x140 (byte), +0x144..+0x150, +0x158, +0x15c, +0x160, +0xdd (byte); +0x1c = 0.5; +0x28 = 1.2;
+0x34 = 0.06; +0x38 = 6.0; +0x3c = 0.01; +0x40 = -1; 4 x 16-byte slots at +0x48 (ctor 0x40c710). 0x40e040: +0xdc = 0; +0xe0 = +0xe4 = +0xe8 = T.sim_time; +0xac, +0xb0, +0x8c..+0x9c = 0.
### 0x40c730 TrafficCar__release
0x40d3c0 (unlink +0x130 and +0x144 links, unlink ref +0xec, `car+0 = 0`), then unlink +0x144 and
+0x130 again (no-ops), dtor of the 4 slots. Caller pushes the car on the pool free list.
### 0x40ce00 ctRoad__spawnCar(road, p1, p2, p3, p4)
```
n = #lanes of road (road+4 first, lane+0x14 next) with !halted(+0xc4); if (!n) return 0
lane = (rand15 % n)-th non-halted lane
car = alloc + ctor
car+0x2c = 15.0; +0x30 = 1.0; +0x34 = 0.06; +0x3c = 0.01; +0x158 = 0
car+0x14 = p1; +0x18 = p2; +0x20 = p3; +0x24 = p4
car+0xb8 = car+0xbc = 0; car+0xc4 = lane; +0xc8 = &lane.path(+0x3c); +0xcc = -1; +0xd0 = +0xd4 = 0
car+0xa8 = 0; car+0xe0 = T.sim_time
// free length of the lane: walk its cars in list order (lane+8 first, car+8 next; ascending +0x88)
free = 0; edge = p1
for c in lane.cars:
    back = c+0x88 - c+0x14 - ((c+0xb0 - 2)*0.5 + 1) * c+0x24 - p3
    if (edge < back) free += back - edge
    edge = c+0x20 + c+0x88 + p4 + p1
tail = lane.length(+0x74) - p3; if (edge < tail) free += tail - edge
if (free <= 0) { release(car); push free list; return 0 }
r = rand15 * (1/32768) * free
acc = 0; edge = p1
for c in lane.cars:
    back = (same)
    if (edge < back) { nacc = back + acc - edge; if (r <= nacc) break; acc = nacc }
    edge = c+0x20 + c+0x88 + p4 + p1
car+0xb0 = 0 (speed); car+0xac = 0
d = (r - acc) + edge; car+0x88 = d
p = CarPathRef__posOffset(car+0xc4, d, car+0xa8)   // dir -1: ctPath__posOffsetAt(lane.path, d, 0)
car+0x8c..+0x94 = p
v = CarPathRef__dir(car+0xc4, d)                   // ctPath__dirAt(lane.path, d)
car+0x98 = p.x + car+0x18 * v.x; car+0x9c = p.z + car+0x18 * v.z   // front point (xz)
ctLane__insertCar(lane, car)                         // 0x40d1e0
```
Positions count *down* to the lane end: the first car of a lane (`lane+8`) is the nearest to the end.
### 0x40d1e0 ctLane__insertCar(lane, car)
Insert before the first car `c` (list order) with `car.pos < c.pos` (strict); else append at the
tail (`lane+0xc`). Sets car+4 prev, +8 next, +0xc lane. Then `lane->vf3(car, -1)` (0x40cbd0).

## 3. Lane attach and the lane event heap
### 0x40cbd0 ctLane__vf3(lane, car) — attach
```
if (lane+0xb4 == car && (lane+0xb4 == 0 || lane+0xb0 == car+0)) lane+0xb0 = lane+0xb4 = 0, lane+0xb8 = 0   // weak ref {serial, car, byte}
if (car+0xc != lane) append car at the tail of lane (car+0xc = lane)
if (!lane+0xa8) {                                  // lane not live
    if (car+0xdc) { live_count(DAT_005e6514)--; if (!car+0x14c && car+0x138 && car+0xdd) queue(T+0x3130, 0x10, "car_rem %d %d", car+0x138, car); car+0xdc = 0; car+0x128 = 0 }
    if (lane+8 == car) {                           // new first car
        t = TrafficCar__timeToLaneEnd(car)          // 0x40d990
        if (lane+0xac == 0) { Traffic__scheduleLane(t, lane); lane+0xac++ }
    }
} else if (car+0xdc != 1) {
    live_count++; if (!car+0x14c && car+0x138 && car+0xdd) queue(T+0x3130, 0x10, "car_add %d %d", car+0x138, car)
    car+0xdc = 1
    if (car+0x44 == 0) { car+0x44 = T+0x88; T+0x88 = car }   // live-car list (head insert)
}
TrafficCar__syncInstance(car, lane.road(+0x10) + 0x68)        // 0x410680
if (lane+0x34 == -1) car+0x124 = lane+0x90 ? lane+0x90 : lane+0x94
lane+0xc8 += car+0x24 + car+0x14 + car+0x20                   // occupied length
```
### 0x40d990 TrafficCar__timeToLaneEnd(car)
```
TrafficCar__step(car)            // 0x4107c0: returns at once when T.sim_time <= car+0xe0 (fresh car)
stop = car+0xbc ? 0 : T.cross_stop_offset(+0x3140)
v = max(TrafficCar__targetSpeed(car), 10.0)
return max(0, (car+0x88 - car+0x14 - stop) * (2 / v))
```
### 0x40dc00 TrafficCar__targetSpeed(car)
```
s = car+0x2c; if (car+0xc4 && lane.vf6(car+0xcc) < s) s = lane.vf6(..)   // ctLane vf6 0x40dd50 = lane+0x98
f = 0
if (!car+0xbc) { if (car+0x124) s = (car+0x124->vf6(-1) + s) * 0.5 }
else { f = car+0x2c; if (car+0xb8 && car+0xb8->vf6(car+0xc0) < f) return max(car+0xb8->vf6(..), s) }
return max(f, s)
```
(+0xb8/+0xbc/+0x124 are 0 for a fresh car. vf6 of a cross object not traced.)
### Event heap (T+0 entries {float time, lane*}, +4 count, +8 capacity, grows by 0x400)
- 0x417050 `Traffic__scheduleLane(dt, lane)`: push {T.sim_time + dt, lane}.
- 0x41a950 push: `i = count++; while (i) { p = (i-1)/2; if (h[p].time <= e.time) break; h[i] = h[p]; i = p } h[i] = e`.
- 0x41a810 pop root: `n = --count; if (n) { i = 0; c = 1; while (c < n) { r = 2i+2;
  if (r < n && h[r].time < h[c].time) { if (h[n].time <= h[r].time) break; h[i] = h[r]; i = r }
  else { if (h[n].time <= h[c].time) break; h[i] = h[c]; i = c } c = 2i+1 } h[i] = h[n] }`.
- consumer, in Traffic__step 0x41b290: `while (count && h[0].time < t) { ctLane__timedEvent(h[0].lane); pop(); }`.
### 0x40fcf0 ctLane__timedEvent(lane) (not carded: needs the per-car step)
```
lane+0xac = max(0, lane+0xac - 1)
if (lane+0xa8) return
for c in lane.cars: TrafficCar__step(c)
f = lane+8
if (f && !f+0xbc) {
    t = timeToLaneEnd(f)
    if (t <= 0.01) { if (!0x40e460(f, 0, lane+0x38) /*move to next lane*/) 0x40d380(1.0) }
    else { if (lane+0xac) return; scheduleLane(t, lane); lane+0xac++ }
}
if (!lane+0xac && lane+8) { scheduleLane(1.0, lane); lane+0xac++ }
```

## 4. type_car vf15 values used (0x4a15c0; M = car type data)
- 0x40: `if (out) { *out = M+0x1fb4 /*wheels*/ ? ((z2 + z3) - (z0 + z1)) * 0.5 : 2.0 }`,
  `z_i = M+0x1fc0 + i*0xc8` = wheel template i mount z (`M+0x1fb8 + i*0xc8 + 8`, notes/13 §6;
  src `CarData::wheels[i].template_pos[2]`). Wheels 0..3 are read whatever the count.
- 0x46: `if (!out) return 0; *out = wheels ? (z2+z3) / ((z2+z3) - (z0+z1)) : 2.0`.
- 0x45: `(M+0x2ec0 >> 1) & 1` (src `CarData::notify`).
- 0x4d falls into 0x40 when `0x4ca120()` is 0. 0x38: take-over, notes/27 §3.
`Game__getInfo` 0x47c640 on a *type* handle (kind 8) calls the type's vf15 directly, so these
read the type data only.

## 5. Instance side (map only; needs road objects, not carded)
- 0x4107a0 `TrafficCar__refreshInstance` = `0x410680(car, lane.road + 0x68)`.
- 0x410680 `TrafficCar__syncInstance(car, roadlink)` (notes/27 §4 shape): with type set, a car on a
  live lane whose road link is bound and without instance -> counters, `Traffic__queueSpawn`
  0x41a500 (append {car serial, car, 0, score} 16-byte entry to T+0x310c, count +0x3110, cap +0x3114).
  Live -> not live: `car_rem`, park the instance (0x417080), unlink.
- 0x417c90 `Traffic__flushSpawns(T, budget=1, cam)`: `DAT_005fe3bc` != 0 -> unlimited (-1).
  Score valid entries (0x40e6e0: car && serial matches) by `-|pos - cam|^2`, 0x40c270 heapsort
  (invalid entries last, nearest first), then `TrafficCar__materialize` in order; failures kept.
- 0x40d4e0 `TrafficCar__materialize(car, &budget)` (asm-checked): needs live, type, no instance;
  parent = lane.road+0x74 (cross+0x50 on a cross path); y of the front point from 0x40da20
  (car+0xc4 ref, then car+0xd0); basis from (front - pos); centre = pos + fwd * car+0x18 * 0.5;
  reuse a parked instance (0x418f60, key car+0x14c) -> `getInfo(inst, 0x38, &car)`, matrix
  "ffffff", reparent, `getInfo(inst, 0x3c, car+0x154)`; else if budget: budget--,
  `createScripted(type, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d" (pos, euler, seed), "traffic_car")`,
  `getInfo(inst, 0x38, &car)`; else return 0 (stay queued). Then `car_add` if +0xdd.
- 0x417080 `Traffic__parkInstance(typeId, inst)`: `getInfo(inst, 0xf) < 0x5a` -> queueDelete; else
  new 16-byte link, `getInfo(link, 0x38, &T+0x3118)` (the pseudo-car holds every parked instance),
  matrix at y = -10000, reparent under T+0x311c, insert in the 0x400-bucket map T+0xe0 keyed by type id.
  type_car vf15 0xf = `(int)(inst+0xb4 * 100)` (meaning unknown).

## 6. Per-car step 0x4107c0 — entry points only
Callers: 0x40d990 timeToLaneEnd, 0x40daa0, 0x40e150 lane init, 0x40e710 ctLane vf4, 0x40f4e0
ctLane__activeStep, 0x40fcf0 ctLane__timedEvent, 0x4102d0. Head: returns if `car+0xc == 0` or
`T.sim_time <= car+0xe0`; `dt = sim_time - car+0xe0; car+0xe0 = sim_time`; then the 4 obstacle
slots (+0x48, sorted by 0x40c270), lane-end/next-lane distance, the 682-line rest not read.

## 7. Correction to notes/28 §3 (active lists in 0x41b290)
`T+0x88` is the **live-car** list (next `car+0x44`, step 0x40f350 `TrafficCar__liveStep`, which
emits `car_rem` when the car leaves a live lane and calls 0x4118f0), not roads. `T+0x8c` is the
active-lane list (next `lane+0xa4`, step 0x40f4e0: while live and `sim_time - lane+0xc0 <= 0.5`,
reschedule each car without next lane via 0x40e460; else 0x40f6d0(0) and drop).

## 8. Cards
- 213 car pool + ctRoad__spawnCar + insert (traffic.h/.cpp).
- 214 lane attach 0x40cbd0 (not-live branch, counters/list for the live branch) + timeToLaneEnd +
  event heap push (traffic.h/.cpp).
- 215 natives addTraffic / addTrafficCar / spawnCarNear on top (natives.cpp, traffic, vmboot).
Not carded: instance side (§5: road objects never bound in src), heap consumer + lane timed
event (needs 0x4107c0), type_car 0x38 take-over, park pool.
