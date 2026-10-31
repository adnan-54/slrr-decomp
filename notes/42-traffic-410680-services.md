# 42 — The four services of TrafficCar__syncInstance 0x410680 and the counters 0x5e64f4/f8/fc

Traced 2026-10-31. Context: notes/27 §4 (shape), notes/29 §5, notes/40 §5 (open item 4).
`T` = the Traffic object (`DAT_005e652c`). Car link `+0x130` is a 16-byte `ResRefLink`
(`+0x130` prev, `+0x134` next, `+0x138` id = src `Car::inst_id`, `+0x13c` object pointer;
src `k130/k134/inst_id/k13c`, traffic.h:158).

## 1. 0x410680 TrafficCar__syncInstance (asm-checked)
ECX = car (EDI), `[EBP+8]` = road/cross link (`lane.road+0x68` / `cross+0x44`), `EDX = link+8`
(bound id), `EAX = car+0x138`. Every reparent call has **ECX = car+0x130** (`0x4106b8`,
`0x410701`); `0x41a500` and `0x417080` have **ECX = T** (`0x4106e1`, `0x410771`).
```
if (car+0x14c == 0) {                          // 0x41068c
    if (!inst) return;                         // 0x4106a0
    if (!live(+0xdc) || !bound) { 0x4ec210(&car+0x130, T+0x311c); return }   // 0x4106c4
    goto reparent_link                          // 0x4106b1 -> 0x410700
}
if (!live || !bound) {                          // 0x4106d0/0x4106d9 -> 0x410713
    if (!inst) return;                          // 0x410715
    if (car+0xdd) queue(T+0x3130, 0, 0x10, "car_rem %d %d" (inst, car), 0)  // 0x41074d, ECX = 0x5fde90
    car+0xe8 = T+0x84;                          // 0x41076b
    0x417080 Traffic__parkInstance(T, car+0x14c, &car+0x130);   // 0x410777
    0x488080 ResRefLink__dtor(&car+0x130);      // 0x41077e  -> clears +0x138 (inst_id) too
    0x5e64f4 -= 1; 0x5e64fc += 1; return        // 0x410783 DEC, 0x410789 INC
}
if (!inst) { 0x5e64f4 += 1; 0x5e64f8 += 1; 0x41a500 Traffic__queueSpawn(T, car); return }  // 0x4106e7/0x4106ed/0x4106f4
reparent_link: 0x4ec210(&car+0x130, link)       // 0x410707
```
**Correction (src gap):** `0x488080` always zeroes the id word `+0x8` = `car+0x138`, so the
dematerialize branch leaves `inst_id == 0`. src `car_sync_instance` (traffic.cpp:1145) does not
clear it; `car_bind_object` (the same body inlined in 0x40ed30/0x40ebb0) already does.

## 2. 0x488080 ResRefLink__dtor (ECX = link)
```
if (link+0xc) { if (link+0 == 0) (link+0xc)+0x78 = link+4; else (link+0)+4 = link+4;
                if (link+4) *(link+4) = link+0;  link+0xc = link+0 = link+4 = 0 }
link+8 = 0                                       // unconditional
```
Unlink from the target object's weak-link list (head at `obj+0x78`). 0x40ed30, 0x40fae0,
0x4177a0, 0x418f60 inline the same sequence.

## 3. 0x41a500 Traffic__queueSpawn (ECX = T, arg car) — asm-checked
```
if (T+0x3110 == T+0x3114) { T+0x3114 += 0x400; T+0x310c = 0x5679b0(T+0x310c, cap*0x10) }  // grow
e = T+0x310c + count*0x10; T+0x3110 = count + 1;
0x40ca10 WeakCarRef__set(e, car)                 // ECX = e (0x41a54c)
```
`0x40ca10`: `car ? (e+0 = car+0 /*serial*/, e+4 = car) : (e+0 = e+4 = 0, byte e+8 = 0)`.
`e+8`/`e+0xc` are not written by the set. The entry has the **same 16-byte layout as the
leader slots** (src `LeaderSlot {serial, car, flag, dist}`, traffic.h:94); `+0xc` is the score
written by flush. Ctor 0x416000 zeroes `T+0x310c/0x3110/0x3114` (`local_14[0xc43..0xc45]`).
Nothing but 0x417c90 shrinks the count (0x4177a0 Traffic__clear does not touch it).
`0x40e6e0 WeakCarRef__valid(e)`: `e+4 && e+0 == *(e+4)` -> 1; else if `e+4` clear all three,
return 0.

## 4. Counters 0x5e64f4 / 0x5e64f8 / 0x5e64fc (all xrefs)
Writers: 0x40ebb0 ctCross__bindObject (`0x40ec4a` f4++, `0x40ec50` f8++, `0x40ed0b` f4--,
`0x40ed11` fc++), 0x40ed30 ctRoad__bindObject (`0x40edda`/`0x40ede0` ++/++, `0x40ee9b`/`0x40eea1`
--/++), 0x40fae0 halt recycle (`0x40fb4a` f4--, `0x40fb50` fc++), 0x410680 (above).
**Only reader:** 0x4d34b0 (callee of type_BOT vf13 0x4d1fe0) case **0xc** (two-level jump table
`0x4d3ae0`/index bytes `0x4d3b38`, case 0xc -> index 0x0a -> `0x4d37d5`):
`PUSH [0x5e64fc]; PUSH [0x5e64f8]; PUSH [0x5e64f4]; PUSH 0x59fe50 "TrafficCar: %d, %d, %d"`.
So they are debug-text counters: f4 = materialized minus parked (net), f8 = spawns queued
(cumulative), fc = instances parked (cumulative). Case 0xb next to it prints
`"Traffic: %d, %d"` with 0x5e6500 (src `live_lanes`) and 0x5e6514 (src `live_count`).
No game logic reads them.

## 5. 0x40fae0 halt recycle (callee of 0x40de70) — counters in the typed branch
```
if (car+0x14c) {                                 // typed: no inst test before parking
    0x417080 parkInstance(car+0x14c, &car+0x130); inline 0x488080 (+0x138 = 0);
    0x5e64f4 -= 1; 0x5e64fc += 1; TrafficCar__remove(car); return
}
new = 0x416800 spawnCarNear(cross+0x24.., 100.0 /*0x42c80000*/, car+0x14, +0x18, +0x20, +0x24);
ResRefLink__set(new+0x130?, car+0x13c); inline dtor of car+0x130; copy +0x2c/+0x30/+0xa8/+0x154/+0xdd;
getInfo(new+0x130, 0x38, &new); 0x410680(new, new.lane(+0xc4).road(+0x10)+0x68);
if (new+0xdd && new+0xdc) queue "car_add %d %d"; TrafficCar__remove(car)
```
UNRESOLVED (asm not read): ECX of the `0x417080` call (decompile shows `in_ECX` = cross; the
callee needs T) and of `ResRefLink__set`. The counters are unconditional in the typed branch
(even when `car+0x138 == 0`), unlike 0x410680.

## 6. 0x417080 Traffic__parkInstance(ECX = T, typeId, link)
```
if (getInfo(link, 0xf, 0) < 0x5a) { 0x4e9450 Handle__queueDelete(link?); return }   // UNRESOLVED ECX
p = 0x51df40() /* 16-byte pool alloc, free list +0x10, pages of 0x400 */; p[0..3] = 0;
ResRefLink__set(p, link+0xc); getInfo(p, 0x38, T+0x3118 /*pseudo-car*/);
matrix "fffffffffffffff" with y-translation 0xc0c3880000000000 = -10000.0 (double); 0x4eb3f0(&m);
0x4ec210(p?, T+0x311c);  ParkMap__insert(T+0xe0 (0x400 buckets x 12), &typeId, &p)
```
`0x4171d0 ParkMap__insert`: bucket `typeId & 0x3ff`; empty bucket -> store key, alloc value
cell (0x41a560), next = 0; else prepend a chained node (0x41dfb0). Reader: `0x418f60
Traffic__takeParked(typeId, &dest)` (pops the cell, moves the instance link into dest, puts
the cell on the free list `T+0xdc`) — only called by materialize 0x40d4e0.

## 7. 0x4ec210 ResRefLink reparent (ECX = instance link, arg = parent link)
Already documented as GameRef.setParent (notes/18 §306; src `Runtime::reparent`,
runtime.cpp:887). Returns 0 when the parent link is unbound (`+8 == 0`), 1 when already
under that parent. In traffic it is only reached with a real instance (`+0x138 != 0`).

## 8. T+0x311c CarContainer (0x416000 Traffic__ctor)
`ResRefLink(T+0x311c) = GameObject__createScripted(&0x5fde50 /*game root*/, &nulltype,
DAT_0059380d, "CarContainer")`; `obj+0x84 |= 0x10`; `0x51f7f0(link)`; then `0x417350()`.
UNRESOLVED: `DAT_0059380d` text, the 0x10 flag, `0x51f7f0`, `0x417350`.

## 9. Consumer: 0x417c90 Traffic__flushSpawns(ECX = T, budget, cam) and 0x40d4e0
Called once per frame by type_ground vf13 0x4bac10 (`budget = 1`, cam = camera pos 0x5fe110 when
`*0x5f1e44 > 0`, else null; `DAT_005fe3bc` (src `load_priority`, natives.cpp:167) -> budget -1).
```
if (!count) return;
for e in queue: if (valid(e)) e.score = cam ? -|0x40dde0(e.car, e.car+0x88, e.car+0xa8) - cam|^2 : 0
0x40c270(queue, count)          // == src slot_sort (traffic.cpp:1762): nearest first, empty last
kept = 0
for i: if (!valid(e[i])) break;                       // stale tail dropped
       if (!0x40d4e0(e[i].car, &budget)) { e[kept++] = e[i] }   // compact the failures
       else e[i] = {0,0,0}
count = kept
```
0x40d4e0 materialize returns **1 (dropped)** when `!live(+0xdc) || !type(+0x14c) ||
inst(+0x138)` or when the parent link (`dir(+0xcc) == -1 ? lane(+0xc4).road(+0x10)+0x74 :
cross(+0xc4)+0x50`) is unbound; **0 (kept)** only when no parked instance and `budget == 0`;
else creates/reuses the instance (`createScripted(type, "%.3f,...,%d", "traffic_car")` or
`takeParked` + matrix "ffffff" + reparent + `getInfo(0x3c, seed)`), `getInfo(inst, 0x38, &car)`,
`car_add`, returns 1.
Resolved 2026-10-31 (§11-§13): road `+0x74` / cross `+0x50` are the `+0xc` object word of
the `+0x68` / `+0x44` ResRefLinks that 0x41a3e0 Traffic__bindObject binds (no separate writer);
0x40da20/0x4136e0/0x413b00 are the path height probes, 0x40ef70 a normalized copy, 0x567250
Mat34 set-column; the camera position is a main-loop copy of the first camera snapshot.

## 10. Consequence for src
- Cardable now (card 257): the counters, the spawn queue append, and the `inst_id = 0` fix.
  Pure Graph bookkeeping; typed City cars (addTrafficN) on live, bound roads do reach the
  queue branch in `--city`.
- Without the flush the queue only grows. Cards: 260 height probes (§12) + load_priority
  accessor, 258 flush + materialize decision/pose with an INVENTED instance-create hook,
  261 (draft) the hook in Runtime (createScripted under the road/cross object, getInfo 0x38/0x3c).
- The reparent/park side (card 259, draft) only acts on real instances: type-0 script cars
  (addTrafficCar) and materialized cars. Needs `T+0x311c` CarContainer and a Graph -> Runtime
  bridge (traffic::Graph holds ids only).

## 11. 0x40d4e0 TrafficCar__materialize, asm-checked (ECX = ESI = car, arg &budget)
```
if (!car.live(+0xdc) || !car.type_id(+0x14c) || car.inst_id(+0x138)) return 1   // 0x40d501..0x40d52d
parent = {0,0,0,0} ResRefLink (EBP-0x40)
0x40cac0 ResRefLink__set(&parent, dir(+0xcc) == -1 ? [[car+0xc4]+0x10]+0x74 : [car+0xc4]+0x50)  // 0x40d54c..0x40d572
if (parent.id(+8, EBP-0x38) == 0) return 1                                           // 0x40d577 -> 0x40d948
p = {front_x(+0x98), world_y(+0x90), front_z(+0x9c)}                                 // EBP-0x24/-0x20/-0x1c
if (!0x40da20(ECX = &car+0xc4 {path_lane, path, dir}, front_x, front_z, &p.y))      // 0x40d5ba
    0x40da20(ECX = &car+0xd0 {kd0, prev_path, prev_dir}, front_x, front_z, &p.y)    // 0x40d5d9, result ignored
d = p - world(+0x8c/+0x90/+0x94)                                                     // EBP-0x68
f = 0x40ef70 normalizedCopy(d)                                                       // EBP-0x30
s = (f.z, 0, -f.x); n = sqrt(s.x^2 + s.z^2); if (n != 0) s *= 1.0f / n               // 0x593570 sign mask, 0x593598 = 1.0f
u = f x s = (f.y*s.z - s.y*f.z, s.x*f.z - s.z*f.x, s.y*f.x - f.y*s.x)               // EBP-0x74/-0x70/-0x6c
M (EBP-0xa4, Mat34): 0x567250 column 0 = s, column 1 = u, column 2 = f               // 0x40d6e9/0x40d6fa/0x40d70b
h = car.k18(+0x18) * 0.5f (0x593590 = 0x3f000000); pos = world + f * h              // EBP-0x18/-0x14/-0x10
ypr = 0x569970 Math__matToYpr(M)                                                     // EBP-0x58/-0x54/-0x50
if (0x418f60 Traffic__takeParked(ECX = T, type_id, &car+0x130)) {                    // 0x40d77c
    getInfo(ECX = 0x5fde90, &car+0x130, 0x38, &car_ptr)                              // 0x40d798
    params = 0x56a000("ffffff", pos.x, pos.y, pos.z, ypr[0], ypr[1], ypr[2])        // fmt 0x59360c, heap block at EBP+8
    0x4eb3f0(ECX = &car+0x130, &params)   // typed(0xa0000000) -> typed+0x44 type typed(0x80000000)->+0xc mgr vf7(+0x1c)(typed+0x4c, params)
    0x4ec210(ECX = &car+0x130, &parent)   // reparent
    getInfo(&car+0x130, 0x3c, seed(+0x154)); free params (0x569f10)
} else {
    if (*budget == 0) return 0                                                       // 0x40d84a..0x40d851 (kept)
    (*budget)--
    buf = sprintf("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d" (0x593614), pos.x, pos.y, pos.z, ypr[0], ypr[1], ypr[2], seed)
    obj = 0x523c80 GameObject__createScripted(&parent, &car+0x144 /*type link*/, buf, "traffic_car" (0x593638))
    0x40cac0 ResRefLink__set(ECX = &car+0x130, obj)                                  // inst_id = obj+0x80
    getInfo(&car+0x130, 0x38, &car_ptr)                                              // 0x40d8fd type_car take-over
}
if (car.notify(+0xdd)) queue(T+0x3130, 0, 0x10, sprintf("car_add %d %d" (0x593644), inst_id, car), 0)  // 0x40d943
return 1   // 0x40d948; 0x488080 dtor of the parent link on every exit past 0x40d52f
```
- `0x40cac0 ResRefLink__set(ECX = link, obj)`: unlink from the old target (`obj+0x78` list),
  then `link+0xc = obj; link+8 = obj+0x80` (id) and head-insert into `obj+0x78`; obj 0 clears.
  So road `+0x74` = road `+0x68` link `+0xc` (the bound object) and cross `+0x50` = cross
  `+0x44` link `+0xc`: written by 0x41a3e0 Traffic__bindObject (src `road.object` /
  `cross.object`, traffic.cpp:2487/2499). "Bound" = `obj+0x80 != 0` = src `object != 0`.
- The owner is chosen by `dir`, like 0x40dde0/0x40dd60. src notes (traffic.cpp:1385) that
  0x4107c0 step 7 can leave `dir >= 0` on a lane owner; the exe then reads lane `+0x50` =
  lane.path(+0x3c) `+0x14` = Path end[0] (a float) as an object pointer. No evidence the
  materialize path meets that state; a port needs an INVENTED guard there.
- The y probe writes `p.y` whenever the underlying probe finds a segment, even when the
  `|lat| < 10` test then fails, so the fallback probe and the final p.y can come from a
  failed first probe. With both failing p.y keeps `world_y`.
- `0x40ef70 Vec3__normalizedCopy(ECX = src, out)`: `out = src; n = |out|; if (n != 0) out *= 1/n`.
- `0x567250 Mat34__setColumn(ECX = M, i, v)`: `M.m[i] = v[0]; M.m[4+i] = v[1]; M.m[8+i] = v[2]`.
- `0x569f00` zeroes a pointer, `0x569f10` frees it (thunk 0x575a7e): the "ffffff" param block.
- `0x56a000(&block, fmt, ...)`: frees the old block, sizes (0x56a050(0, fmt, 0)), allocs
  (0x545e50), fills (0x56a050(block, fmt, va)). Format "ffffff" = six floats; not traced deeper.

## 12. Path height probes (ECX-checked from asm)
### 0x40da20 CarPathRef__heightAt(ECX = ref {owner, path, dir}, x, z, float *y) -> bool
```
if (!ref.path(+4)) return 0
ok = ref.dir(+8) < 0 ? 0x4136e0(ECX = path, x, z, y, &lat) : 0x413b00(ECX = (Segment *)path, x, z, y, &lat)
return ok && |lat| < 10.0f            // 0x5935c0 abs mask, 0x59373c = 0x41200000 = 10.0f, COMISS/SETA
```
### 0x413b00 ctSegment__heightAt(ECX = seg, x, z, float *y, float *lat) -> bool (asm read in full)
```
if (!seg.curved(+0x38)) {                                   // 0x413de6
    n = sqrt(m0.x^2 + m0.z^2); dx = m0.x/n; dz = m0.z/n     // no zero guard
    t = dz*(z - p0.z) + dx*(x - p0.x); L = seg.len(+0x30) * n
    if (t < 0.0f (0x593580) || L < t) return 0              // COMISS/JC
    t /= L; *y = (1 - t)*p0.y + p1.y*t; *lat = |dx*(z - p0.z) - dz*(x - p0.x)|; return 1
}
curved: identical to src segment_project's curved branch (traffic.cpp:350..388; same constants
1e-5 double 0x593588, 0.01 double 0x593560, 0.9f 0x59370c, 0.1 double 0x593568, k = 1.0f) with
outputs *lat = (x - pos.x)*side.x + (z - pos.z)*side.z (signed) and *y = pos.y (pos from the
last 0x4134e0 evaluation, side = 0x4131a0 at the final d); return 1.
```
Note the straight branch differs from 0x414480: it uses `len * |m0.xz|` (not flat_len) and has
no 1e-5 guard.
### 0x4136e0 ctPath__heightAt(ECX = path {count +0, segs +4, stride 0x54}, x, z, y, lat) (decompile)
```
best = -1
for i in segs: body of 0x413b00 inlined on seg i -> (yi, lati) when it succeeds;
    if (best == -1 || |lati| < |*lat|) { *lat = lati; *y = yi; best = i }
return best != -1
```
No endpoint fallback (unlike 0x414830 path_project).

## 13. Camera position for flushSpawns (0x5fe110, count 0x5f1e44)
`0x5fe110..0x5fe118` (12 bytes) is written only in Game__mainLoop 0x47d760 (`0x47dbab` MOVQ +
the +8 dword): when `DAT_005f1e44 > 0` it copies `0x5fe130..0x5fe138` = the first camera
follow snapshot's position (entries 0x58 @0x5fe120, notes/08), the same value the listener
uses (notes/14). The copy runs **after** `0x47ba10` (the animate pass that reaches type_ground
vf13 0x4bac10), so flushSpawns scores against the previous frame's camera. Second reader
0x4bad6a (the 0x4bac10 call site). src has no camera snapshots (boot.cpp:222: the
`0x5f1e44 <= 0` branch, listener at the origin), so in src `cam = null` and every score is 0.0f.
