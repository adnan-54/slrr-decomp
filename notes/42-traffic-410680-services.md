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
UNRESOLVED: the writer of road `+0x74` / cross `+0x50` (not in 0x41a3e0 Traffic__bindObject,
which only binds `+0x68`/`+0x44`), `0x40da20`, `0x40ef70`, `0x567250`, and the camera
position source in src.

## 10. Consequence for src
- Cardable now (card 257): the counters, the spawn queue append, and the `inst_id = 0` fix.
  Pure Graph bookkeeping; typed City cars (addTrafficN) on live, bound roads do reach the
  queue branch in `--city`.
- Without the flush the queue only grows; the flush (card 258, draft) is blocked on
  materialize's parent link (`road+0x74`/`cross+0x50`) and instance creation.
- The reparent/park side (card 259, draft) only acts on real instances: type-0 script cars
  (addTrafficCar) and materialized cars. Needs `T+0x311c` CarContainer and a Graph -> Runtime
  bridge (traffic::Graph holds ids only).
