---
id: 249
status: todo
branch: card/249
depends: []
---
# traffic: ctCross vf1 (live_refs gate) and the Cross+0x40 route-block writer

## Goal
Two facts from the exe (notes/40 §4.6 and §4.2), one of them a real behaviour fix:

1. `ctCross__vf1 0x40e380` is a pure read: `return cross->live_refs (+0x38) != 0`, no
   writes and no calls (3 instructions). The exe dispatches it through vtable slot `+0x04`
   on a car's **current ref owner** in `car_live_step 0x40f350`: a car with no next ref
   stays live while its owner's vf1 is true. The owner is a lane (lane vf1 `0x40e390` =
   `lane+0xa8` live) or a cross (`0x40e380`). `src/game/traffic.cpp` `car_live_step`
   currently treats every cross owner as not live (the `shortcut:` at line ~1360), so a
   car holding a cross current ref is dropped from the live list as soon as it has no next
   ref, where the exe keeps it while `live_refs != 0` (`-1`, the dying state, is also
   true). Port the vf1 and use it there.

2. `Cross+0x40` has exactly **one** writer in the whole image: `ctCross__releaseAll
   0x40de70`, store `0x40de90` = `cross->k40 = T.sim_time (+0x84) + delay`. Its only two
   callers are `0x419980` (cross halt from the traffic-signal/green-wave path `0x4196f0`,
   delay `0.001f`) and the script native `0x4de360` (delay read from a script float). Both
   callers need services src does not have (see Exe evidence), so **do not port the
   eviction body and do not add a caller**: `Cross::k40` stays `-1.0f` on every src path
   (behaviour-equal, the gate `0x40e400` never blocks). Resolve the field comment instead.

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x40e380 ctCross__vf1 (vtable +0x04; ECX = cross)
Raw body (8 bytes):
```
0x40e380 CMP dword ptr [ECX + 0x38], 0x0
0x40e384 SETNZ AL
0x40e387 RET
```
=> `bool vf1() { return cross->live_refs != 0; }`
- Reads **only** the int32 at `+0x38` (`Cross::live_refs`). No other field, no write, no
  call. The caller reads only `AL` (`TEST AL,AL`), so the upper EAX bits do not matter.
- `-1` (dying) returns true; only `0` returns false.
- Image bytes at the three cross vtables all carry `0x40e380` at `+0x04`
  (`read_bytes 0x5936bc`, `0x59377c`, `0x5937c0`: first entries `d0db4000 80e34000`).

Callers of slot `+0x04` (the calls are indirect, xrefs only reach the vtables):
- `0x40f350 car_live_step`, at `0x40f3f3..0x40f402`:
  ```
  if (car->next_path (+0xbc) != 0) skip;
  owner = *(Car* + 0xc4);              // current ref owner pointer: Lane* or Cross*
  if (owner->vtable[+0x04]() == 0 && car->live (+0xdc)) drop live;
  ```
  This is the site the src shortcut stands for (see 0x40f350 below).
- `0x40f950 car_set_ref`, at `0x40f963..0x40f96e`: the same slot on the **new** owner;
  src already has it in `cross_path_ref` (`traffic.cpp` `if (c.live_refs != 0)
  car_set_live(...)`, `// cross vf1 0x40e380`). Nothing to change.
- `0x4157a0 routeLive`, at `0x4158b2..0x4158b7`: `piVar2` is `cross->links[b].node`, i.e. a
  **lane**, so this is lane vf1 `0x40e390` (`return lane+0xa8`), *not* cross vf1. Do not
  cite cross vf1 for the routeLive scoring.

### 0x40f350 car_live_step, the vf1 gate
```
if (*(char *)(car + 0xdc) == 0) { car_push_instance(); return 0; }   // 0x4118f0
... (cross-claim branches)
if (car->next_path (+0xbc) == 0) {                                    // 0x40f3e6
    cVar = (**(code **)(**(int **)(car + 0xc4) + 4))();               // 0x40f3f3..0x40f3fe
    if (cVar == 0 && car->live (+0xdc) != 0) {                        // 0x40f402
        --DAT_005e6514;
        ... "car_rem" log when !car+0x14c && car+0x138 ...
        car->live (+0xdc) = 0;
        if (car->k128 (+0x128)) car->k128 = 0;
    }
}
car_push_instance();                                                   // 0x4118f0
return 1;
```
`*(car+0xc4)` is the current owner pointer; in src the same convention is already used:
`dir (+0xcc) < 0` => `path_lane (+0xc4)` is a lane index, `dir >= 0` => `path_lane` is a
cross node id (step 6 copies `next_lane_a`; see `traffic.cpp` `car_advance` at the
`cross_vf6 0x40dd00` branch, line ~1039, and `car_target_speed` line ~1066).

### 0x40e400 ctCross__route, the +0x40 gate (already ported, keep as is)
```
0x40e403 MOV EAX,[0x005e652c]              ; Traffic singleton
0x40e408 MOVSS XMM0,dword ptr [ECX + 0x40]
0x40e40d COMISS XMM0,dword ptr [EAX + 0x84] ; sim_time
0x40e414 JBE 0x0040e41c                    ; k40 <= sim_time -> continue
0x40e416 XOR EAX,EAX                       ; k40 >  sim_time -> return 0
0x40e418 POP EBP; RET 0xc
0x40e41c ... dispatch vf by live_refs sign
```
=> blocks routing iff `cross->k40 > g.sim_time`. NaN: COMISS is unordered (CF=ZF=1), JBE is
taken, so no block. `src/game/traffic.cpp` `cross_dispatch` line `if (c->k40 > g.sim_time)
return false;` already matches exactly.

### 0x40de70 ctCross__releaseAll — the only writer of Cross+0x40
`ECX = cross`, args `float delay ([EBP+0x8])`, `char all ([EBP+0xc])`, `RET 8`.
Store (0x40de73..0x40de90):
```
MOV EAX,[0x005e652c]
MOVSS XMM0,dword ptr [EAX + 0x84]     ; T.sim_time
ADDSS XMM0,dword ptr [EBP + 0x8]      ; + delay
MOVSS dword ptr [EDI + 0x40],XMM0     ; cross->k40 = sim_time + delay
```
Then (cleaned, offsets are the exe's):
```
// 1) evict every car on the cross's own list (+0x8 head, car+0x8 next)
while (cross->cars (+0x8) != 0) {
    car = cross->cars (+0x8);
    if (car->notify (+0xdd) && car->live (+0xdc)) {
        --DAT_005e6514;                                    // live car count
        if (!car->k14c && car->k138 && car->notify)
            log("car_rem %d %d", car->k138, car);          // 0x528030 / 0x47d550
        car->live (+0xdc) = 0;
        if (car->k128) car->k128 = 0;
    }
    car_release(ECX = cross, car);                         // 0x40fae0
}
// 2) evict cars from the link lanes: links = cross+0x14, count = cross+0x18,
//    each 8-byte entry {Lane *node; uint8 flag (+4)}
for (i = 0; i < cross->link_count (+0x18); i++) {
    if (all == 0 && cross->links[i].flag (+4) == 0) continue;
    lane = cross->links[i].node;
    for (car = lane->cars (+0x8); car; car = car->next (+0x8)) {
        if (all == 0 && car->next_path (+0xbc) == 0) continue;
        ... same release block as 1) ...
        car_release(ECX = cross, car);                     // 0x40fae0
    }
}
```
`0x40fae0` (helper, not ported): if `car+0x14c != 0` unlink the instance link with
`0x417080`, clear `+0x130/+0x134/+0x13c/+0x138`, `--DAT_005e64f4`, `++DAT_005e64fc`,
`0x40f290`; else spawn/rebuild through `0x416800 -> 0x40cac0 -> 0x410680` and copy
`+0x2c/+0x30/+0xa8/+0x154/+0xdd`. These are the instance services notes/40 §3 lists as
untraced/missing in src, so the eviction body stays unported.

Callers of 0x40de70 (both unmodelled, verified by xrefs `0x41999e`, `0x4de408`):
- `0x419980` (ECX = cross, one arg): `FUN_0040de70(cross, 0.001f /*0x3a83126f*/, 1)`
  (0x419992..0x41999e), then for every link sets `lane->halted (+0xc4) = 1` and collects
  lanes through `lane+0x10` / `lane+0x18` for `0x4196f0`'s later `0x419ab0` pass. Called
  from `0x4196f0` at 0x4198a3/0x4198e3 (nearest-cross + green-wave setup via `0x419050`).
- `0x4de360` (script native): `FUN_0040de70(ECX = 0x419050(...nearest cross...),
  delay = third out of 0x4ecc70, 1)` at 0x4de400..0x4de408; the native also reads script
  object floats `"x"`, `"y"`, `"z"` (0x5a37a0, 0x5a3788, 0x5a377c; read_bytes shows the
  C strings). Void return (plain `RET`).

### 0x40de70 raw instruction anchors (for the card reviewer)
- 0x40de8c `CMP dword ptr [EDI+0x8],0`; 0x40de90 `MOVSS dword ptr [EDI+0x40],XMM0`.
- 0x40df2d `CMP dword ptr [EDI+0x8],0`, 0x40df31 `JNZ 0x40dea0` (cross list loop).
- 0x40df3c `CMP dword ptr [EDI+0x18],ECX` (link count), 0x40df57 `CMP byte [EAX+ECX*8+4],DL`
  (link flag), 0x40df67 `MOV ESI,dword ptr [EAX+0x8]` (lane car head),
  0x40e022 `CMP ECX,dword ptr [EDI+0x18]`.

## Changes
1. `src/game/traffic.cpp`, above `car_live_step` (0x40f350): add
   ```cpp
   // 0x40e380 ctCross__vf1 (vtable +0x04; ECX = cross): CMP [ECX+0x38],0;
   // SETNZ AL; RET. Reads only live_refs (+0x38); no writes, no calls.
   // -1 (dying) is active, only 0 is not.
   bool cross_active(const Cross &c) { return c.live_refs != 0; }
   ```
2. Same function, replace the `shortcut:` block + the INVENTED "not on a lane => not live"
   line with:
   ```cpp
   // 0x40f3f3: owner = *(Car+0xc4), vf1 dispatched on its vtable +0x04:
   // lane vf1 0x40e390 = lane+0xa8 live; cross vf1 0x40e380 = +0x38 live_refs.
   bool cur_live;
   if (car.dir < 0) {
       cur_live = car.path_lane < g.lanes.size() && g.lanes[car.path_lane].live;
   } else {
       const Cross *c = cross_by_node(g, car.path_lane, nullptr);  // INVENTED null guard
       cur_live = c != nullptr && cross_active(*c);
   }
   ```
   Keep the rest of `car_live_step` (next-ref skip `!car.next_lane_b`, live_count--,
   notify shortcut, `car_push_instance`) unchanged.
3. `src/game/traffic.h`, `Cross::k40` comment: replace "writer UNRESOLVED" with the traced
   facts:
   ```cpp
   float k40 = -1.0f;  // +0x40 route-block deadline: 0x40e400 returns 0 while
                       // k40 > sim_time (0x40e408 COMISS/JBE); ctor 0x40c590 stores
                       // -1.0f; sole writer 0x40de70 stores sim_time + delay (0x40de90),
                       // reached only from 0x419980 (0.001f) and native 0x4de360.
   ```
4. Optionally cite the writer on the gate line in `cross_dispatch`
   (`// 0x40e408; writer 0x40de70`). Do not add a `cross_block`/release helper and do not
   call it: no src path invokes 0x40de70, so k40 must stay -1 (behaviour-equal).

## Files
- edit: src/game/traffic.cpp (`cross_active`, `car_live_step`)
- edit: src/game/traffic.h (`Cross::k40` comment)
- read only: notes/40-traffic-cross-chain.md §4.2/§4.6

## Done when
- `// 0x40e380` on the new helper and `// 0x40f3f3`/`// 0x40f402` on the car_live_step
  branch; `Cross::k40` comment cites `0x40e408` (gate) and `0x40de90` (writer), no
  `UNRESOLVED` left for k40.
- No writer call/helper is added; `cross_dispatch`'s `k40 > sim_time` stays byte-for-byte
  the same condition.
- `make -C src check` green; no new unit tests or hand-made expected values. If a city
  check changes its pass/fail only because a cross-owner car now stays live longer, stop
  and report it on the card (do not edit the check or its expected values).
- card frontmatter: status implemented, branch name.

## UNRESOLVED
- `0x419980`'s second half (only needed if the cross-halt path is ever ported): the chain
  `lane+0x10 -> +0x8+idx*4 -> object with +0x14/+0x18/+0x1c/+0x20` and the global list
  `DAT_005e6538/653c/6540` consumed by `0x419ab0` are not identified. Raw lines:
  `0x4199c6 MOV EAX,dword ptr [EAX+0x10]`; `0x4199c9 MOV ECX,dword ptr [EAX+ECX*4+8]`;
  `0x4199fe MOV EBX,dword ptr [ECX+0x20]`.
- `0x4de360`'s script-native identity: `FUN_004ecc70(...,0x7a2620,...)` with
  `DAT_007a2620` being zeroed `.data` (read_bytes 0x7a2620 = 96 zero bytes) and the
  `"wait"`/`"notify"` strings next to the `"x"`/`"y"`/`"z"` field names are not resolved
  to a native name/index. Raw lines: `0x4de376 CALL 0x004b1af0`;
  `0x4de37b PUSH 0x007a2620`; `0x4de384 CALL 0x004ecc70`. Not needed for this card (the
  writer itself is unambiguous).
