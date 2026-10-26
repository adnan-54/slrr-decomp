# 36 — Physics keep-alive walk: map node -> road object -> lanes live (RE 2026-10-26)

Follow-up of notes/31 §3, notes/32 §4 and notes/35 §5 (2). Card 229 gave src the object box
(`RigidBody::obj_kind/obj_centre/obj_half`, filled per event phase by `update_object_boxes`);
card 219 gave `Graph::keep_road_live`, card 220 the `Graph::step` active-lane walk. This note
closes the three gaps between them. Decompiled: 0x4fe6f0, 0x4ee460, 0x4fc610, 0x4d42a0, 0x4d44d0,
0x4ea900, 0x504440. No new names (all already in analysis/names.json).

## 1. Collision dispatch is always the interval manager
`PhysicsWorld__dispatchCollisionEvents` 0x4fc610: for every queued object O (0x501da0 queue, notes/32
§2): clear `O+0x84 & 0x10000000`; `Phys__eventFilter` 0x4f9750 (|O.half|² <= 10000 and an awake
type-2 child); manager = `GameObject__intervalMgr` 0x4e9ac0 via `O+0xfc`; `IntervalMgr__touchObject`
0x4edf80; collect up to 16 distinct managers. Then `IntervalMgr__collidePass` 0x4ee460 per manager,
then `DAT_006028dc = frame`. There is **no direct groundTree call from the dispatcher**: every
map-node visit starts in collidePass.

### 0x4ee460 IntervalMgr__collidePass (relevant part, lines ~600-1072 of the decompile)
For each interval node I of the manager touched this pass (`I+0x198 == mgr+0x1f0`), O = I+0x30:
```
prefetch box = { kind 1, centre = O.centre, half = O.half + 5 }        // local_94
Collide__setPassFlags 0x504440(I+0x24, O)   // DAT_006028e8 = (typed(O)+0xc & 0x10000) != 0,
                                            // DAT_006028ec = traffic-car info (getInfo 0x3e)
for each sweep-and-prune pair p of I (list I+0x148):
    N = pair other end (+0x30); stop if N's manager id != O+0xf8
    if (|O.y - N.y| > N.half.y + O.half.y) continue      // x/z already overlap by the SAP
    residency: if (!(N+0x94 & 1)) requestLoad(N, N+0x9c, 0, DAT_006028e8 ? 10 : 0)
    T = N->vf3(1.0); if (!T) continue
    0x4ef475: if ((N+0x84 & 0x100) && T+0x4c && (T+0xc & 0x4000) && T+0x44)
                  typed(T+0x44)+0xc ->vf5(T+0x4c, 1.0)                // road keep-alive
    if (T+0x18 /*faces*/ && !DAT_006028e8) Collide__groundSoup 0x4ff6b0(O, N, T, ...)
    children of N:
      - faceless plain type-1 container (pair flag 4 clear): its type-1 children are added to
        the manager as their own SAP entries (IntervalMgr__addNode, 0x4f0b40) = lazy explode;
      - otherwise: for each child C (skip +0x84 & 0x200000):
            type 1 and C+0x80 != O+0x80 -> Collide__groundTree(O, C, ..., 0, &prefetch)
            type 2 -> Collide__physInstPair(O, C)
```
### 0x4fe6f0 Collide__groundTree(O, N, ..., depth, prefetch)
```
if (O.half.x + N.half.x < |N.x - O.x| || O.half.z + N.half.z < |N.z - O.z| ||
    O.half.y + N.half.y < |N.y - O.y|) {                    // miss (touching = overlap)
    if (frame > DAT_006028dc && N overlaps the prefetch box) Collide__groundPrefetch(...)
    return 0                                                // children NOT visited
}
if (!(N+0x94 & 1) && !(N+0x94 & 0x10000000) && (!(N+0x84 & 0x2000000) || prio == 0))
    requestLoad(N, N+0x9c, 0, prio = DAT_006028e8 ? 10.0 : 0.0)   // prio 0 = load now
if (N+0x94 & 1) {
    T = N->vf3(1.0)
    0x4fe852: if ((N+0x84 & 0x100) && T+0x4c && (T+0xc & 0x4000) &&
                  (ty = ResRef__getTyped(T+0x38, 0x20000000, 1.0)) && ty+0xc)
                  ty+0xc->vf5(T+0x4c, 1.0)                     // road keep-alive
    ... faces: Collide__groundSoup 0x4ff6b0 when T+0x18
}
for each child C of N (skip +0x84 & 0x200000):
    type 1, C+0x80 != O+0x80 -> groundTree(O, C, ..., depth + 1, prefetch)
    type 2 -> Collide__physInstPair(O, C)
```
`Collide__setPassFlags` 0x504440 sets `DAT_006028e8 = 1` only for objects whose typed `+0xc` has
0x10000; those also skip face collision, so it is never set for the player car (which collides
with the map faces). For cars: prio 0, the node is made resident (0x5195a0 -> native vf3) inside
the same visit, before the keep-alive test.

### Which nodes get the keep-alive (set semantics)
A node is reached only after its parent overlapped O's box (groundTree returns before the
children; the SAP explode adds children only when the container pair overlapped). **Corpus fact
(maps/city.rpk, scratch program over `RpkArchive::parse_instance`): every one of the 58,210 type-1
nodes has a record box inside each ancestor's box (0 violations; all 755 road nodes included).**
So, for shipped data, "node box overlaps O box" (each node on its own) and "node and all ancestors
overlap" give the same set. The exe **order** inside one pass is the SAP pair order, which src
does not model (no interval manager): src uses the DFS order of `CollisionMesh::map_nodes`
(INVENTED order). Order only matters for head-insert order of `Graph::active_lanes` / live cars and
the `car_add` notify order; the set and the per-pass `kc0 = sim_time` stamps are exact.

## 2. Map node -> road object (creation and the link)
`ResourceInstance_Game__load` 0x5195a0 (notes/31 §2) runs the gametype's native vf3 the first time
the node is resident and stores the result in `T+0x4c`. The embedded road node's gametype is
`_type_road` (0x15, `native road`).

### 0x4d42a0 type_road__vf3(link, params)
```
obj = new 0x20 bytes (zeroed); obj+0xc = link to the node's game instance (param_1+0xc)
ground = 0x4ea900(obj link)          // the instance's container link +0xfc (the map/ground)
if (Game__getInfo(ground, 0x39) == 0)   obj+0x1c = 0                 // no Traffic: never rebinds
else { obj+0x1c = Traffic__bindObject 0x41a3e0(params, obj)            // node pointer or 0
       if (obj+0x1c) { typed(node)+0xc |= 0x4000; GameRef__setNodeFlags 0x4ebdd0(0x100) } }
return obj
```
### 0x4d44d0 type_road__vf5(obj, f)
```
if (!obj+0x1c || !obj+0xc) return
if (f != 1.0) { LOD test on the node box (notes/30 §2.5), may return }   // skipped for f = 1.0
obj+0x1c ->vf1()          // road 0x40f8e0 = keep every lane live; cross 0x40e380 = query only
```
So the runtime link is: map node -> (created once, on first overlap) road object -> bound
Traffic node. Both flags the walk tests (`N+0x84 & 0x100`, `T+0xc & 0x4000`) are set exactly when
`bindObject` returned non-zero, so in src "bound node id != 0" stands for all three tests.
src ids: `Graph::bind_object` returns eq+light+idx+1 for a road, idx+1 / eq+idx+1 for crosses;
a road id maps back to `keep_road_live(id - 1 - eq - light)`; a cross id does nothing (vf1 0x40e380
only reads the refcount).

## 3. src today (what is missing)
- `CollisionMesh::map_nodes` (physics/mesh.h) holds every type-1 node of the ground soup in DFS
  order with its record box and `end` (index past the subtree). It has no gametype/params and no
  object. `gather_ground_soup` (runtime.cpp ~200) reads the RSD of each node via `parse_instance`
  already, but drops it.
- The ground body (`Resource::ground_body`) has no `owner`, so physics cannot find the Resource.
- `Resource::traffic` is created lazily on the first `ground_traffic` lookup (INVENTED timing).
- No walk calls anything per map node except the face broad phase (`collect_hull_candidates`,
  0x4feb20 gate on the step-start sphere: a different test, not the O box).

## 4. Smallest exe-faithful port (cards 230, 231)
1. **Card 230 (runtime):** per map node of a ground Resource, keep `{record id, gametype, params,
   created, traffic node}` in DFS order parallel to `map_nodes` (INVENTED storage). New
   `Runtime::map_node_touched(ground, ni)` = residency/create (0x5195a0 -> 0x4d42a0, only for
   `native road` gametypes) + keep-alive (0x4fe852 -> 0x4d44d0(f = 1) -> road vf1). Not called yet.
2. **Card 231 (physics walk):** in `World::step`'s event phase right after
   `update_object_boxes`, for every awake dynamic root with records (= queued O) that passes
   `|obj_half|² <= 10000`, walk every static body's `map_nodes` with the 0x4fe6f0 overlap test and
   subtree skip on miss, calling a new `World::map_node_touch` hook; the Runtime wires the hook to
   `map_node_touched` via `ground_body->owner`.
Not ported (shortcut comments): SAP pair order (src: DFS order); prefetch branch
(`Collide__groundPrefetch`, async residency in a +5 m box: would only create objects earlier);
creation of non-road map objects (walk 0x352, scripted 0x2xxxx gametypes); node unload / object
destroy; the camera driver (notes/31 §4, render only).

## 5. Effect on vmboot
- `--race`: the career race map has **no** `_type_road` node and no `.trc` (ROC_track/test_track:
  0 hits for `gametype 0x00000015`; only city.rpk has 755). The walk visits nodes but binds nothing,
  so race numbers (659.414 s) should not move; any change is a bug or a side effect of the new
  ground `owner` and must be reported.
- `--city`: if the player car is awake in Valocity, every road whose box overlaps the car's O box
  is bound on first touch and kept live each event phase: `Graph::set_lane_live` puts its lanes on
  `active_lanes`, the two crosses' refcounts go up, the lane cars go live (`car_add` notifications),
  and `Graph::step` (card 220) now walks a non-empty active list. Lanes that the car leaves expire
  after 0.5 s of traffic time and get a lane event scheduled (0x417050), which can change the
  "one pending event per non-empty lane" city check after long runs (the current check runs only
  3 frames, under 0.5 s). Implementers must report live-lane / live-car counts and every check
  delta.
