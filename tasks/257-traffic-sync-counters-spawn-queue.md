---
id: 257
status: merged
branch: card/257
review: claude
depends: []
---
# traffic: syncInstance bookkeeping — counters 0x5e64f4/f8/fc, queueSpawn 0x41a500, inst_id clear

## Goal
Replace the counter / queue `shortcut:` lines in the three per-car instance helpers with the
exe's bookkeeping. No instance creation, reparent or parking (those stay `shortcut:`; cards
258/259). Notes: `notes/42-traffic-410680-services.md` §1, §3, §4, §5.

## Exe evidence (asm-checked; car fields as in src/game/traffic.h)
### 0x410680 TrafficCar__syncInstance (src `car_sync_instance`, traffic.cpp:1145)
```
type = car.type_id (+0x14c); inst = car.inst_id (+0x138); bound = link+8 != 0
if (type == 0) { ... unchanged (reparent only) ... }
if (!car.live || !bound) {                       // 0x410713
    if (!inst) return;
    ... car_rem (unchanged shortcut) ...
    car.e8 = sim_time;                           // 0x41076b
    // 0x417080 parkInstance stays a shortcut
    0x488080 ResRefLink__dtor(&car+0x130)        // 0x41077e: ALWAYS zeroes +0x138 -> car.inst_id = 0
    0x5e64f4 -= 1;                               // 0x410783 DEC
    0x5e64fc += 1;                               // 0x410789 INC
    return;
}
if (!inst) {                                     // 0x4106dd
    0x5e64f4 += 1;                               // 0x4106e7 INC
    0x5e64f8 += 1;                               // 0x4106ed INC
    0x41a500 Traffic__queueSpawn(T, car);        // 0x4106f4
    return;
}
```
### 0x41a500 Traffic__queueSpawn (ECX = T)
```
if (T+0x3110 == T+0x3114) { T+0x3114 += 0x400; T+0x310c = realloc(T+0x310c, cap*0x10) }
e = &T+0x310c[count]; count++;                   // 16-byte entry
0x40ca10 WeakCarRef__set(e, car): e+0 = car+0 (serial), e+4 = car   // e+8, e+0xc not written
```
The entry is exactly the `LeaderSlot` layout (traffic.h:94: serial, car, flag, dist) — reuse
that struct; `car` is the src 1-based car index; `flag`/`dist` stay at their value-init 0
(INVENTED: the exe leaves realloc'd bytes unwritten, src value-initializes them).
Ctor 0x416000 starts the queue empty. Nothing in this card removes entries (0x417c90 flush is
card 258).

### 0x40ed30 / 0x40ebb0 ctRoad/ctCross__bindObject (src `car_bind_object`, traffic.cpp:2442)
Same branch body as 0x410680 per car: dematerialize branch (only when inst != 0)
`0x5e64f4 -= 1` (`0x40ee9b` / `0x40ed0b`), `0x5e64fc += 1` (`0x40eea1` / `0x40ed11`);
typed + live + bound + no inst: `0x5e64f4 += 1` (`0x40edda` / `0x40ec4a`), `0x5e64f8 += 1`
(`0x40ede0` / `0x40ec50`), then `Traffic__queueSpawn(car)`.

### 0x40fae0 halt recycle (src `recycle_halted_car`, traffic.cpp:3248), typed branch
```
if (car+0x14c) { parkInstance (shortcut); +0x138 = 0; 0x5e64f4 -= 1 (0x40fb4a); 0x5e64fc += 1 (0x40fb50); remove }
```
Counters are **unconditional** here (no `+0x138` test), unlike 0x410680.

### Counters
Only reader: debug text `0x4d34b0` case 0xc `"TrafficCar: %d, %d, %d"` (f4, f8, fc). Model them
next to `live_count` in `Graph` as signed 32-bit ints, `// global 0x5e64f4; INVENTED: per graph`
(same convention as `live_count`/`live_lanes`, traffic.h:187/196).

## Files
- edit: src/game/traffic.h (Graph: three counters + `std::vector<LeaderSlot> spawn_queue;  // +0x310c/+0x3110`)
- edit: src/game/traffic.cpp (`car_sync_instance`, `car_bind_object`, `recycle_halted_car`;
  add `static void queue_spawn(Graph &g, uint32_t ci)  // 0x41a500`)

## Done when
- the counter and queueSpawn `shortcut:` lines in the three functions are gone; parkInstance,
  reparent, car_rem and the unlink-of-links shortcuts stay
- `car_sync_instance` sets `car.inst_id = 0` in the dematerialize branch (cite `0x41077e`)
- each change has its `// 0xADDR` comment; `make -C src check` green, race/city output unchanged
- no new unit tests or hand-made expected values; card frontmatter status implemented + branch
