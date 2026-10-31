---
id: 247
status: merged
branch: card/247
review: deepseek-pass
depends: []
---
# traffic: cross vf4 0x4883e0 is a no-op; vf5 0x46acd0 returns 0; vf6 0x40dd00 is the path-lane speed limit

## Goal
Close the last two "not traced" marks on the cross vtable slots that card 244 left
(`v4 0x4883e0` and `v6 0x40dd00`) and document slots `+0x10..+0x18` of the three
cross-family vtables. **The premise that vf4 is the release/detach half of the cross
car list is wrong**: 0x4883e0 is `RET 8` with an empty body (writes no car/cross field,
calls nothing); the car is already unlinked from the cross at car+0x4/+0x8/+0xc by
`0x40ca40` (src `lane_unlink`), which `0x40f950` calls immediately before the
old-owner vf4 call. `0x488080` is a different unlink helper and is not involved.
Port the one slot with behaviour: vf6 0x40dd00 in `car_target_speed` 0x40dc00.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x40f950 CarPathRef__set (ECX = Car) — the vf4 call site, asm-verified
```
0x40f9aa CALL 0x40ca40                 ; ECX = car: unlink car+0x4/+0x8/+0xc from its owner
0x40f9af CMP dword [ESI+0xc8], 0
0x40f9b6 JZ  0x40f9ca
0x40f9b8 MOV ECX, [ESI+0xc4]           ; ECX = old owner (lane or cross)
0x40f9be PUSH dword [ESI+0xcc]         ; arg2 = dir
0x40f9c4 PUSH ESI                      ; arg1 = car
0x40f9c5 MOV EAX, [ECX]
0x40f9c7 CALL dword [EAX+0x10]         ; old owner vf4(car, dir)
```
So vf4 takes `this=owner, (Car*, int dir)` and is called only after 0x40ca40 has
already detached the car. For a cross owner vf4 is 0x4883e0, for a lane owner it is
0x40e710 (`lane_detach`), already ported.

### 0x4883e0 ctCross__vf4 — the whole body, raw bytes
```
0x4883e0: C2 08 00        RET 8          ; 3-byte body; no prologue, no callee, no write
```
Writes no field on the car or the cross. The cross car list is fully maintained by
0x40cb60 (append) and 0x40ca40 (unlink):
`car+0x4 = prev, car+0x8 = next, car+0xc = owner; owner+0x8 = head, owner+0xc = tail`.
0x488080 is a different list (see below), **not** a callee of 0x4883e0.

### 0x488080 ResourceRefDLNode__unlink (ECX = node), 65 bytes, RET
Not part of the cross list: different node layout and different owner field.
```
node+0 = next, node+4 = prev, node+8 = serial/id, node+0xc = owner; owner+0x78 = tail
if (owner != 0) {
    if (next == 0) owner+0x78 = prev; else next+4 = prev;
    if (prev != 0) prev+0 = next;
    owner = next = prev = 0;
}
node+8 = 0;
```
This is the handle/instance link unlink already ported as `Runtime::unlink`
(`src/game/runtime.cpp:770`, cited there as 0x488080) and still a shortcut for the
car's `+0x130` instance link in `car_sync_instance` 0x410680. Do not touch it here.

### Cross vtables: 0x5936bc (ctCross), 0x59377c (ctCrossEq), 0x5937c0 (ctCrossLight)
Bytes (4-byte little-endian slots):
```
0x5936bc: 40dbd0 40e380 477320 40cb60 4883e0 46acd0 40dd00 56ee6f
0x59377c: 40dbd0 40e380 477320 40cb60 4883e0 46acd0 40dd00 4157a0 ...
0x5937c0: 40dbd0 40e380 477320 40cb60 4883e0 46acd0 40dd00 4157a0 ...
```
All three share slots +0x0c..+0x18 (refs to each slot confirmed: xrefs to 0x40cb60 are
0x5936c8/0x593788/0x5937cc, to 0x4883e0 include 0x5936cc/0x59378c/0x5937d0, to
0x40dd00 are 0x5936d4/0x593794/0x5937d8):
| slot | fn | behaviour |
|---|---|---|
| +0x00 vf0 | 0x40dbd0 | not traced here |
| +0x04 vf1 | 0x40e380 | live_refs test; notes/40 §4.6 |
| +0x08 vf2 | 0x477320 | src calls it an empty RET 4 (not re-verified here) |
| +0x0c vf3 | 0x40cb60 | tail append (card 244, `cross_attach`) |
| +0x10 vf4 | 0x4883e0 | **no-op `RET 8`, writes/calls nothing** |
| +0x14 vf5 | 0x46acd0 | `XOR EAX,EAX; RET 8` → returns 0 (int), writes nothing |
| +0x18 vf6 | 0x40dd00 | max of two link lanes' +0x98 (float), `RET 4`, takes the path index |
Base ctCross has `0x56ee6f` (`_purecall`: abort stub) at +0x1c/+0x20/+0x28; ctCrossEq
overrides +0x1c..+0x2c with 0x4157a0/0x4159d0/0x40e590/0x415f20/0x415500 (route vfs +
dtor), ctCrossLight with 0x415f40/0x415570. 0x59377c is stored in ctCrossEq's
dtor 0x415500 (xrefs 0x415525), 0x5937c0 in ctCrossLight's ctor 0x4154b0 (xrefs
0x4154cf/0x415595); the class names are Ghidra's, the addresses are read from the
image.

### 0x40dd00 ctCross__vf6 (ECX = Cross; arg uint32_t path index) — 80 bytes, RET 4
```
p     = cross+0x1c + idx*0x60          // CrossPath array (the src CrossPath)
a     = *(u8 *)(p + 0)                 // paths[idx].a
b     = *(u8 *)(p + 1)                 // paths[idx].b
laneA = *(Lane **)(cross+0x14 + a*8)   // links[a].node (a Lane* in the exe)
laneB = *(Lane **)(cross+0x14 + b*8)   // links[b].node
return laneB->k98 < laneA->k98 ? laneA->k98 : laneB->k98;   // COMISS + JBE (NaN -> B)
```
It reads only `cross+0x14` (links), `cross+0x1c` (paths) and `lane+0x98`; writes nothing.
(`0x40dd50` is the lane's vf6 of the same shape: `return lane+0x98`, index ignored.)

### 0x40dc00 TrafficCar__targetSpeed (ECX = Car) — src `car_target_speed`
```
local_8 = car+0x2c                                // max_speed
if (car+0xc4 != 0) {                              // current owner present
    v = owner->vf6(car+0xcc);                     // dir: -1 lane -> k98; >=0 cross path idx
    if (v < local_8) local_8 = v;
}
fVar2 = 0.0;
if (car+0xbc == 0) {                              // no next ref
    if (car+0x124 != 0)                           // shortcut stays: owner at +0x124 not modelled
        local_8 = (car+0x124->vf6(0xffffffff) + local_8) * 0.5f;
} else {                                          // next ref exists
    fVar2 = car+0x2c;                             // max_speed
    if (car+0xb8 != 0) {                          // next owner
        v = next_owner->vf6(car+0xc0);            // next dir
        if (v < fVar2) { fVar2 = v; return max(fVar2, local_8); }
    }
}
return max(fVar2, local_8);                       // == max(local_8, min(max_speed, vf6))
```
The src ref tag decides which vf6: `kc0 < 0` = lane owner (lane index in
`next_lane_a`), `kc0 >= 0` = cross owner (cross node id in `next_lane_a`, path index in
`kc0`). The current ref uses `dir < 0` / `dir >= 0` the same way, with the owner id in
`path_lane`.

## Changes
1. `src/game/traffic.cpp`
   - change `cross_by_node`'s first parameter to `const Graph &` (all callers pass a
     non-const Graph and still bind); return `Cross *` with a `const_cast` on the
     element, comment `// INVENTED: the exe stores mutable Cross*`.
   - change `car_target_speed(const Graph &g, ...)` to `car_target_speed(Graph &g, ...)`
     (all four call sites already pass a non-const Graph).
   - new helper before `car_target_speed`:
     ```
     // 0x40dd00 ctCross__vf6: max of the two link lanes' +0x98 for path idx.
     // INVENTED: missing index/node guards (the exe indexes and dereferences unchecked).
     float cross_vf6(const Graph &g, const Cross &c, size_t idx)
     {
         if (idx >= c.paths.size()) return 0.0f;
         const CrossPath &p = c.paths[idx];
         if (p.a >= c.links.size() || p.b >= c.links.size()) return 0.0f;
         const Lane *a = g.lane_by_node(c.links[p.a].node);
         const Lane *b = g.lane_by_node(c.links[p.b].node);
         const float va = a ? a->k98 : 0.0f;
         const float vb = b ? b->k98 : 0.0f;
         return vb < va ? va : vb;   // COMISS + JBE: keep vb on NaN
     }
     ```
   - rewrite the body of `car_target_speed` to the exe order above:
     ```
     float s = car.max_speed;
     if (car.path) {                          // INVENTED proxy for car+0xc4 != 0
         float v = s; bool have = false;
         if (car.dir < 0) {                   // lane vf6 0x40dd50
             if (car.path_lane < g.lanes.size()) { v = g.lanes[car.path_lane].k98; have = true; }
         } else {                             // cross vf6 0x40dd00
             Cross *c = cross_by_node(g, car.path_lane, nullptr);
             if (c && (size_t)car.dir < c->paths.size()) { v = cross_vf6(g, *c, (size_t)car.dir); have = true; }
         }
         if (have && v < s) s = v;
     }
     float f = 0.0f;
     if (!car.next_lane_b) {
         if (car.k124) {
             // shortcut: +0x124 owner vf6(-1) not modelled (src has no owner there)
             s = (0.0f + s) * 0.5f;
         }
     } else {
         f = car.max_speed;
         float v = 0.0f; bool have = false;
         if (car.kc0 < 0) {                   // lane next owner = lane index
             if (car.next_lane_a < g.lanes.size()) { v = g.lanes[car.next_lane_a].k98; have = true; }
         } else {                             // cross next owner = cross node id
             Cross *c = cross_by_node(g, car.next_lane_a, nullptr);
             if (c && (size_t)car.kc0 < c->paths.size()) { v = cross_vf6(g, *c, (size_t)car.kc0); have = true; }
         }
         if (have && v < f) f = v;
     }
     return f <= s ? s : f;
     ```
     This also fixes the old `if (car.next_lane_a)` test, which treated a lane index as
     a cross and missed lane 0; keep behaviour exact otherwise.
   - `car_set_ref` (`// shortcut: cross vf4 0x4883e0 not traced...`): replace the
     comment with `// 0x4883e0 ctCross__vf4: no-op RET 8 (writes and calls nothing);
     // the car left the cross list in lane_unlink 0x40ca40 above.` No code change.
   - `cross_path_ref`: in the existing comment, change `vf4 0x4883e0 is not traced` to
     `vf4 0x4883e0 is a no-op RET 8`.
   - add the vtable table above as a block comment next to `cross_attach`.
2. `src/game/traffic.h`
   - `Cross::cars` comment: `// +0x8/+0xc intrusive car list (tail append 0x40cb60,
     detach 0x40ca40; vf4 0x4883e0 is a no-op); INVENTED: car indices, tail = back()`.
   - `Car::next_lane_a`/`kc0` comment: note that the next-owner vf6 is vf6 0x40dd00 for
     a cross (kc0 >= 0) and lane k98 0x40dd50 for a lane (kc0 < 0).

## Files
- edit: src/game/traffic.cpp (cross_by_node const, cross_vf6, car_target_speed, comments)
- edit: src/game/traffic.h (Cross::cars and Car next-ref comments)

## Done when
- `// 0x40dd00` on `cross_vf6`, `// 0x40dc00` on `car_target_speed`; the vf4 sites cite
  `0x4883e0` as a no-op; the only remaining shortcut in `car_target_speed` is the
  `+0x124` owner, marked `// shortcut:`/`INVENTED:`
- no field or callee of 0x4883e0 is modelled (there are none); `lane_unlink` 0x40ca40
  is the only cross-list detach
- `make -C src check` green; no new unit tests or hand-made expected values. The city
  probe (card 246) must still report a hand-over; if its frame/position numbers move,
  report the delta, do not edit the expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- vf5 0x46acd0 returns int 0 and has no call site outside the three vtables; its purpose
  is unknown (nothing to port).
- vf2 0x477320 ("empty RET 4") is not re-verified in this card.
- The `car+0x124` owner in the no-next-ref branch of 0x40dc00 is still unmodelled; the
  existing `shortcut:` stays.
- `0x40dd00` dereferences null lane links; src returns 0.0f for a missing node (INVENTED
  guard, the exe would fault).
