# 31 — Map-placed objects: creation at load, road keep-alive drivers (RE 2026-10-25)

Follow-up of notes/30 §2 "Why this is not carded" (a) and (b). Ghidra session with the traffic /
resource subset of analysis/names.json applied; 10 new names added (ResourceEngine__touch 0x518e90,
ResourceEngine__updateTouched 0x518f80, ResourceEngine__processLoads 0x517f60,
GameInstData__typedUpdate 0x51a5f0, Camera__prepareFrame 0x45f880, cObjectDummy__ctor 0x45d260,
ResourceNode__loadTyped 0x4608e0, ResRef__getTyped 0x468b00, Render__releaseInstance 0x45f030,
GameRef__setNodeFlags 0x4ebdd0). R = resource node of a game instance (ResourceInstance_Game,
vtable 0x5a7d70), T = its typed data R+0x108 (so T+0x40 = R+0x148 gametype id, T+0x44 = R+0x14c type
node, T+0x4c = R+0x154 native object returned by the type's create).

**Correction to notes/30 §2.4**: there is no "instance vf5 -> typed vf5" forwarding. Instance vf5
0x51a740 / vf6 0x51a6f0 only record a load priority. The typed vf5 (`type_road__vf5` 0x4d44d0) is
reached on two other paths, the physics collision walk (§3, f = 1.0) and the resource touch list
(§4, camera LOD f). Both are traced below.

## 1. Corpus (scratch python parse of maps/city.rpk; the record layout is notes/01, the blob is notes/19 §7)
- All 755 `gametype 0x00000015` (`_type_road`) objects are **embedded type-1 leaf nodes** of the
  spatial tree. They sit at depth 4..18 (most at 9..14), and none has children or PHYS.
  **Every one has the bounds block** (flags & 1): a box around its road or cross, y half 4.25..11.2
  m, x/z half 8..564 m. Params examples: `1;1,225` (road), `3;1,149` (light cross).
- 1,799 `0x352` (`_type_walk`) and many scripted gametypes (0x2xxxx) are embedded the same way.
- src: resman counts the road/walk params with an INVENTED raw byte scan for "gametype ...\r\nparams"
  (resman.cpp ~527). Runtime never instances an embedded child: `Runtime::mount_package` walks the
  record table only, and `gather_ground_soup` (runtime.cpp ~200) reads only their PHYS/bounds into the
  merged collision mesh. No `_type_road` create exists. Nothing in src is map-specific here, and
  card 217 keeps it that way.

## 2. Creation: residency runs the native create
`ResourceInstance_Game__load` 0x5195a0 (vf10, ECX = R), when R becomes resident:
```
parent = R+0x24; if (parent && !resident(parent)) { if (parent kind != 1) parent->vf5(1.0); getTyped; parent->vf3(1.0) }
if (!resident(R)) {
    loadData(R, data, size)   // 0x519940: embedded child records become NOT resident nodes under R
                              // (made resident at once only when R+0x84 & 0x80000), RSD -> gametype/params
    setLoaded(1); ...
}
if (R+0x154 == 0 && R+0x148 /*gametype*/) {
    type = ResRef__getTyped(R+0x148 node)          // 0x468b00, the gametype record's typed data
    R+0xec/+0xe8 = type+0x18/+0x1c                 // load priority scale/bias (used by vf5/vf6)
    if (type+0xc /*native handler*/) R+0x154 = handler->vf3(&link, params R+0x150); free params
    if (type+0x10 /*script class*/) THRD-CREATE <init> ...   (notes/19 §7)
    neither -> "load: Undefined GameType Constructor! %08X"
    Game__registerByInfo(&link); if (handler) handler->vf16(R+0x154)
}
```
So an embedded `_type_road` gets `type_road__vf3` 0x4d42a0 (notes/30 §2.1) **the first time its node
is made resident**. Map load itself does not create it. Two callers make embedded nodes resident:

### 2a. Physics (synchronous for cars)
`Collide__groundTree` 0x4fe6f0 (notes/24), for each node N whose box overlaps the colliding object
O's box (O+0xb4 centre, O+0xc0 half; N+0xb4/+0xc0):
`if (!resident(N)) requestLoad(N, N+0x9c, 0, DAT_006028e8 ? 10 : 0)`. Prio 0 loads now (0x518a80 ->
0x51e500 -> vf10), so the road object exists before the next step of the same walk (§3).
`IntervalMgr__collidePass` 0x4ee460 does the same for the cell nodes.
### 2b. Render (queued)
`Render__collectInstance` 0x45f180 -> `ResourceNode__loadTyped` 0x4608e0(0x80000000, 1.0) ->
`Resource__getTyped` 0x51f4f0(flags, 0, 10.0) -> `requestLoad(N, N+0x9c, 0, 10.0)`: queued, done in
`ResourceEngine__processLoads` 0x517f60 (main loop, budget DAT_005db02c per frame), only for nodes
whose camera LOD (`Camera__instanceLod` 0x461980 -> vf5) passes `N+0x9c > DAT_005f06d4`.

## 3. Road keep-alive driver 1: the collision walk (asm-checked, 0x4fe852..0x4fe8ac)
In `Collide__groundTree`, right after the residency step, for a resident N:
```
T = N->vf3(1.0)                                      // typed data
if ((N+0x84 & 0x100) && T+0x4c && (T+0xc & 0x4000)) {
    ty = ResRef__getTyped(ECX = T+0x38, 0x20000000, 1.0, 0, 0)   // the type node T+0x44, no touch
    if (ty && ty+0xc) ty+0xc->vf5(T+0x4c, 1.0)       // handler vf5(obj, f = 1.0)
}
```
`type_road__vf3` sets both flags: `T+0xc |= 0x4000` (via R->vf3) and `R+0x84 |= 0x100`
(0x4ebdd0), and it does that only when `Traffic__bindObject` found the road/cross. With f = 1.0,
`type_road__vf5` skips its LOD test and calls `obj+0x1c->vf1()` (road 0x40f8e0 -> every lane vf7
0x40eed0: `ctLane__setLive(lane, 1); lane+0xc0 = T.sim_time`). `IntervalMgr__collidePass` has the same
inlined block at 0x4ef475 (bytes `test [edi+0x84],0x100; cmp [esi+0x4c],0; test [esi+0xc],0x4000;
esi = [esi+0x44]; ... call [eax+0x14]` with 1.0).
**So the lanes of every road/cross whose box overlaps a colliding object's box are kept live every
collision pass, with no camera.** That is exe behaviour, not a headless invention.

## 4. Road keep-alive driver 2: camera touch list (render only)
- `type_road__vf3` -> R+0x84 |= 0x100. `Render__collectInstance` on such a kind-1 node without render
  data allocates a `cObjectDummy` (0x45d260) and registers it in the camera list (0x459b30).
- `Camera__prepareFrame` 0x45f880: for every registered entry: `Camera__instanceLod(N, 0)` (vf5/vf6 ->
  N+0x9c/N+0xa0 = scale*f + bias, frame stamps N+0xa8/N+0xac); if `N+0x84 & 0x100`:
  `ResourceEngine__touch(N, N+0xa0, 0, 20.0)`.
- 0x518e90 `ResourceEngine__touch(N, f, a, b)`: `if (N->vf4(f, a, b) && !(N+0x84 & 0x4000000))`
  append N to the touched list (E+0x16d0) and set N+0x84 |= 0x4000000.
  Game vf4 0x51a0f0: `if (!(N+0x84 & 0x400) && (a || b)) { if (!(N+0x84 & 0x4000000)) { T = vf3(f);
  if (!T || T+0xc & 0x4000 || f < T+0xa4 || f > T+0xa8) { if (f > 0) vf5(f); return 1 } } } else vf11(f, 0); return 0`.
- Main loop 0x47d760 order per iteration: physicsTick (only when > 15 ms passed) ... `processLoads`
  0x517f60, `updateTouched` 0x518f80, render (0x44e980/0x47b5b0), then `DAT_005dadbc++` (frame).
- 0x518f80: for each touched entry: `N->vf11(N+0x9c, list)`; nonzero -> drop the entry, clear 0x4000000.
  Game vf11 0x51a5a0: `if (N+0xa8 == frame) return 0; GameInstData__typedUpdate(T, N+0xa0); N+0xa8 = frame; return 1`.
  0x51a5f0: `handler = typed(T+0x44)+0xc; if (handler && T+0x4c) { handler->vf5(T+0x4c, f);
  if (f < T+0xa4 || f > T+0xa8) handler->vf6(T+0x4c, &T+0xa4, &T+0xa8, f) }` ("unload: Undefined
  GameType Update method!" when there is neither a handler nor a script class).
  So a road seen by the camera in frame n gets `type_road__vf5(obj, LOD f)` in the next loop
  iteration, and the 0x4d44d0 LOD threshold applies (notes/30 §2.5).
- `Resource__getTyped` 0x51f4f0 also touches any 0x100 node it is asked for (unless flag
  0x20000000): `ResourceEngine__touch(N, N+0x9c, a, b)`.

**Not carded: needs an invented rule.** Driver 2 needs the camera position/LOD (cam+0x350, +0x4a0..
+0x4b0), the render scene walk and its culling (0x463b90/0x462750, not traced) and the render-side
registration. Headless src has none of these. Any "visible" rule would be INVENTED. Driver 1 is the
only one a headless rebuild can port faithfully.

## 5. `Traffic__step` 0x41b290 verified
```
g_traffic = T; T+0x84 = t; if (T+0xc46*4 /*0x3118*/) its +0xe0/+0xe4/+0xe8 = t + 1
while (heap not empty && heap[0].time < t) { ctLane__timedEvent; TrafficHeap__pop }
for list, link in [(T+0x90 cross_eq active, +0x54, 0x415c40), (T+0x94 cross_light active, +0x60, 0x415d60),
                   (T+0x88 live cars, +0x44, TrafficCar__liveStep 0x40f350),
                   (T+0x8c active lanes, +0xa4, ctLane__activeStep 0x40f4e0)]:
    p = &list; while (*p != T) { if (step(*p)) p = &(*p)+link; else { n = (*p)+link; (*p)+link = 0; *p = n } }
while (T+0xc8 != T) { c = pop head (link c+0x10, cleared); if (c+0x138) Game__queueEvent(c+0x130, 0, 1, 0, 0) }
```
Every walk ends at the sentinel T, and a step that returns false unlinks the node and zeroes its link.
That matches card 216's end marker. src `Graph::step` (traffic.cpp:986) only sets sim_time
(shortcut comment): the four walks and the notify queue are not ported.

## 6. What a faithful src driver needs (order)
1. Embedded map objects with identity: per node gametype + params + bounds (card 217).
2. `Traffic__bindObject` + road/cross object links (card 218). `type_road__vf3`'s glue (flags 0x4000 /
   0x100, Traffic from ground getInfo 0x39) comes with the driver.
3. Live chain: `ctLane__setLive` 0x40f6d0, road vf1 0x40f8e0, lane vf7 0x40eed0, `type_road__vf5`
   0x4d44d0, `ctLane__activeStep` 0x40f4e0 expiry + the T+0x8c walk in Graph::step. Not carded yet:
   setLive must be re-decompiled for card evidence, and without (4) it is dead code.
4. Driver 1 (§3): per collision pass, each colliding object's box vs map node boxes in groundTree
   order (src already has the node table, cards 207/208). On the first overlap, create the node's
   native object (residency -> vf3). Then call vf5(obj, 1.0) for road nodes. **Blocked on the object
   box O+0xb4/+0xc0**: who sets it for a car game object is not traced (notes/24 doubt).
   `PhysBody__updateBounds` 0x504180 writes the swept AABB + bounding radius
   (`def+0x90 shape +0xc`) into `body+0x54`'s node and touches it (0.1, 0, 1.0). It is called from
   0x5001c0 / 0x5030b0 (body integrate / compound sync), 0x4e1e40 (PhysicsRef.setMatrix) and 0x488f30.
   Whether `body+0x54` is O (type 1) or its type-2 instance P is not traced. Next RE: who sets
   body+0x54, and O+0xb4 for a type-1 object.
Spawned traffic objects (notes/29 §5) are the only gameplay effect: live lanes -> `car_add`
events / spawn queue -> real `traffic_car` objects. vmboot `--city` stops inside Valocity, so no
current check observes it.

## 7. Cards
- 217 map spatial objects: RSD gametype/params per embedded node, replacing resman's raw scan.
- 218 Traffic__bindObject + ctRoad/ctCross bindObject + object links, corpus-checked on city.rpk.
