# 32 — Colliding object's box (O+0xb4) and the lane live-state chain (RE 2026-10-25)

Follow-up of notes/31 §6 (4) "blocked on the object box". Ghidra session, traffic/physics subset of
analysis/names.json. New names: `PhysBody__queueCollideObject` 0x501da0,
`GameObject__refreshBoundsTree` 0x504090, `ctCross__isActive` 0x40e380 (cross vf1),
`GameObject__intervalMgr` 0x4e9ac0, `interval_node__ctor` 0x4edbc0; **renamed** 0x503c20
`GameObject__placeBegin` -> `GameObject__boundsFromInstances` (it computes a box, it places nothing).

## 1. Who is who: O, P, body+0x44/+0x54
`ResRefLink__set` 0x40cac0 (ECX = link, 4 dwords: prev, next, id = target+0x80, **target at +0xc**).
`Vehicle__createCar` 0x4992e0, asm 0x4999bf..0x4999dc and 0x49a143..0x49a159:
```
P = GameObject__createPhysInstance 0x524620(car, 0, body, "car_root" | "wheelbone" | 0x59c0a8 | "a_arm")
(body+0x48).set(P)      // body+0x54 = P (type-2 "_physinst" node), body+0x50 = P id
(body+0x38).set(car+0xc) // car_root only: body+0x44 = O, the car's game-instance node (type 1)
```
So `body+0x54` is **P**, the type-2 instance node, not O. O is the game object's type-1 node; its
type-2 children (O+0x30 list) are the P nodes of all its bodies (car_root, each wheelbone, a_arm,
0x59c0a8 part). 0x524620 creates P under O (`0x5188e0/0x518880(registry, O id, {type 2})`, flag
0x200), sets typed(P)+0xb4 = body (refcounted back list at body+0/+4) and an initial box
`P+0xb0 = 0 (sphere)`, centre = body active pos (`body + 0x64 + 0xd4*body[0x20c]`), radius
`def+0x90 shape +0xc` (bounding radius). createCar also sets O's first box (asm 0x49a5dd):
`O+0xb0 = 0, centre = chassis active pos, half = 10, 10, 10`.

## 2. Per physics step: P box, then O box, then the walk
1. `PhysBody__integrateTree` 0x5001c0 (notes/08 D/I), per integrated body, after the RK4 and children:
   ```
   if (body+0x50 /*P id*/) PhysBody__updateBounds(body)       // 0x504180
   root = body; while (root+0x58 & 4) root = root+0x20         // compound root
   PhysBody__queueCollideObject(root+0x38)                     // 0x501da0
   ```
   0x504180 (P = body+0x54; a = active state, o = other state, `+0x64` pos, stride 0xd4,
   br = `body+0x5c -> +0x90 shape -> +0xc`):
   ```
   P+0xb0 = 1                                   // box kind
   P.centre = (a.pos + o.pos) * 0.5             // +0xb4..+0xbc
   P.half   = |a.pos - o.pos| * 0.5 + br        // +0xc0..+0xc8, per axis
   if (P+0x84 & 0x100) ResourceEngine__touch(P, 0.1, 0, 1.0)
   ```
   0x501da0 (link = root+0x38, O = link+0xc): `if (O && !(O+0x84 & 0x10000000)) { O+0x84 |=
   0x10000000; append a 0x34-byte ResourceRefDLNode(link) to the collide queue }` (tail at 0x605258).
2. Event phase (notes/08 H). `PhysicsWorld__purgeEvents` 0x5040f0 walks the queue (head
   `[0x605230]`): entry dead (+0x2c id 0) -> drop; else `GameObject__refreshBoundsTree` 0x504090(O):
   ```
   for each type-1 child C of O: 0x504090(C)
   GameObject__boundsFromInstances 0x503c20(O):  // AABB union of every type-2 child with kind != 2
       if (any) { O+0xb0 = 1; O.centre/half = union box of (child.centre ± child.half) }
   GameObject__placeInContainer 0x503de0(O)       // reparent O in the scene tree + IntervalMgr__addNode
   return O+0x84                                   // 0x200000 -> drop the queue entry
   ```
   notes/11 only listed the drop; the box refresh is the main job.
3. `dispatchCollisionEvents` 0x4fc610: per queue entry, clear `O+0x84 & 0x10000000`, filter
   `Phys__eventFilter` 0x4f9750 (|O.half|² <= 10000 and a type-2 child whose body has
   `(body+0x58 & 0xa0) == 0`), manager `GameObject__intervalMgr` 0x4e9ac0 (O+0xfc link -> typed with
   gametype 0x35 -> native +0xe4), `IntervalMgr__touchObject` 0x4edf80 (interval_node per O; the
   node keeps a link to O at +0x24/+0x30 and its x/y/z interval endpoints from O's box, 0x4efd80),
   then `IntervalMgr__collidePass` 0x4ee460 per manager -> `Collide__groundTree` 0x4fe6f0(O, N, ...)
   with **O's union box** (notes/31 §3: overlapped resident road node -> `type_road__vf5(obj, 1.0)`).

So the box the walk uses is: union over all P of O of `[mid(start, end pos) ± (|Δpos|/2 + br)]`,
recomputed every event phase from the bodies integrated in that sub-iteration.

## 3. What src has (src/physics, src/game/car.cpp)
- `RigidBody`: one pose (`pos`, `rot`); the step-start state lives only in `world.cpp`'s local
  `StepState` (BodyState pos/vel/momentum/rot) during `World::step`. No P/O node, no box, no queue.
- Bounding radius: `Shape::bounding_radius()` per shape; compound hulls are
  `RigidBody::collision_children` (INVENTED storage, card 160) on car_root, whose own shape is
  `SHAPE_NONE` (car.cpp ~1431). **No wheelbone / a_arm / part bodies exist** (shortcut at car.cpp
  ~1444), and those are exactly the extra P nodes the exe unions into O's box.
- Map nodes: the ground body's `CollisionMesh::map_nodes` (cards 207/208) hold every type-1 node's
  record box in groundTree order, including the 755 road nodes (no faces). Map objects are known by
  identity only in resman (card 217 `RpkArchive::map_objects`) and `Graph::bind_object` (card 218).

## 4. Why driver 1 (the physics keep-alive) is not carded
A faithful O box needs the P box of every body the exe gives a `_physinst`: car_root plus each
wheelbone / a_arm / 0x59c0a8 body (createCar) and the part bodies (0x4bd040 / 0x4c1f90 / 0x4e8a50
also call 0x524620, not traced). src has none of the wheel/part bodies, and car_root's bounding
radius differs (SHAPE_NONE + collision_children). Any src O box = **INVENTED** (e.g. "chassis swept
box"). Per the task rule this is not carded. The traced formulas above are ready for a card once src
has per-part bodies (or once a decision accepts the approximation). Also open: the runtime has no
link from a map node index to its bound road object (card 217 data lives in resman only).

## 5. Lane live-state chain (asm-checked 0x40f6d0)
T = g_traffic 0x5e652c. `ctRoad__vf1` 0x40f8e0: `for (l = road+4 /*node[0]*/; l; l = l+0x14) l->vf7()`.
`ctLane__vf7` 0x40eed0: `ctLane__setLive(lane, 1); lane+0xc0 = T+0x84 /*sim_time*/`.
`ctCross__isActive` 0x40e380 (cross vf1): `return cross+0x38 != 0` — so `type_road__vf5` on a
**cross** object keeps nothing alive; only road objects do.

### 0x40f6d0 ctLane__setLive(lane, on) (ECX = lane, RET 4)
```
if (lane+0xa8 == on) return
g_live_lanes(0x5e6500) += on ? 1 : -1
lane+0xa8 = on
road = lane+0x10
if (on) {
    for c in (road+8, road+0xc):                 // the road's two crosses (eq or light)
        if (c+0x38 == 0) { c->vf10(); c+0x38 = 1 }   // vf10: activate (below)
        else if (c+0x38 < 0) c+0x38 = 1
        else c+0x38 += 1
    for car in lane list (lane+8, next car+8):
        if (car+0xdc != 1) {
            g_live_cars(0x5e6514)++
            if (car+0x14c == 0 && car+0x138 && car+0xdd) queue "car_add %d %d"(car+0x138, car) to T+0x3130
            car+0xdc = 1
            if (car+0x44 == 0) { car+0x44 = T+0x88; T+0x88 = car }    // live-car list head insert
            // (car+0xdc == 0 && car+0x128) car+0x128 = 0  -- dead: +0xdc was just set to 1
        }
    if (lane+0xa4 == 0) { lane+0xa4 = T+0x8c; T+0x8c = lane }        // active-lane list head insert
} else {
    for c in (road+8, road+0xc): if (--c+0x38 == 0) c+0x38 = -1
    for car in lane list: TrafficCar__dropLive 0x40fa50(car)
    if (lane+8) { t = TrafficCar__timeToLaneEnd 0x40d990(ECX = first car)
                  if (lane+0xac == 0) { Traffic__scheduleLane 0x417050(t, lane); lane+0xac++ } }
}
for car in lane list: TrafficCar__syncInstance 0x410680(car, road+0x68)
```
### 0x40fa50 TrafficCar__dropLive(car)
```
if (car+0xbc == 0 && car+0xdc) {
    g_live_cars--
    if (car+0x14c == 0 && car+0x138 && car+0xdd) queue "car_rem %d %d" to T+0x3130
    car+0xdc = 0
    if (car+0x128) car+0x128 = 0
}
```
### Cross vf10 (activate): 0x415f20 eq / 0x415f40 light
`if (c+0x54 == 0) { c+0x54 = T+0x90; T+0x90 = c }` (eq) — light uses `+0x60` / `T+0x94`.
The list end is the sentinel T (Traffic__ctor 0x416000), like the live-car list (notes/30 §1).
### 0x40f4e0 ctLane__activeStep(lane) -> keep listed?
```
if (lane+0xa8 == 0) return 0
if (T+0x84 - lane+0xc0 <= 0.5) {
    for car in lane list (next fetched before the body):
        if (car+0xbc == 0) {
            TrafficCar__step 0x4107c0(car)
            stop = (car+0xbc ? 0 : T+0x3140)                     // always T+0x3140 here
            v = max(TrafficCar__targetSpeed 0x40dc00(car), 10)
            t = max(((car+0x88 - car+0x14) - stop) * (2 / v), 0)
            0x40e460(lane, car, t, lane+0x38)                     // hand-over to the next lane
        }
    return 1
}
ctLane__setLive(lane, 0); return 0
```
So a lane stays live only while something calls road vf1 at least every 0.5 s of traffic time.
Cross steps 0x415c40 (eq) / 0x415d60 (light, "change_lamp" events) read `+0x38`: negative = going
inactive (eq step returns 0 when nothing holds the cross); not ported here.

### Traffic__step 0x41b290 walk (notes/31 §5) for the lane list
`p = &T+0x8c; while (*p != T) { if (activeStep(*p)) p = &(*p)+0xa4; else { n = (*p)+0xa4; (*p)+0xa4 = 0; *p = n } }`.

## 6. src mapping (traffic.h/.cpp)
- `Lane::road` = lane+0x10 (raw node id), `Road::node[0..2]` = road +4 (first lane) / +8 / +0xc
  (crosses, ids 1..eq = cross_eq, then cross_light), `Lane::node14` = +0x14 next lane in road.
- `Cross::k38` (+0x38) is the live refcount (signed); today "only zeroed". `Lane::live` +0xa8,
  `kc0` +0xc0, `pending_events` +0xac, `Car::live` +0xdc, `live_next` +0x44, `next_lane_b` +0xbc,
  `notify` +0xdd, `type_id` +0x14c, `inst_id` +0x138, `k128` +0x128. `Graph::live_cars` = T+0x88,
  `live_count` = 0x5e6514. Missing: lane +0xa4 / T+0x8c active-lane list, cross +0x54/+0x60 and
  T+0x90/+0x94 active-cross lists, the live-lane counter 0x5e6500.
- `Graph::step` is a shortcut (sim_time only).

## 7. Cards
- 219: ctLane__setLive + dropLive + lane vf7 + road vf1 + cross vf10 (active lists, refcount).
- 220: Graph::step active-lane walk + ctLane__activeStep (0.5 s expiry; car movement stays a shortcut).
- Not carded: driver 1 (§4, INVENTED O box), cross steps 0x415c40/0x415d60, TrafficCar__step 0x4107c0,
  hand-over 0x40e460, the lane event heap pop (ctLane__timedEvent).
