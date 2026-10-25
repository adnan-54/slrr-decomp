# 24. Streamed ground nodes vs src's merged soup (RE 2026-10-25)

Question (CHECKPOINT "Known open spots"): src collides car hulls with one INVENTED merged PHYS soup
(`gather_ground_soup`, src/game/runtime.cpp; BVH in `CollisionMesh`), the exe walks resident map
nodes per object (`Collide__groundTree` 0x4fe6f0 -> `Collide__groundSoup` 0x4ff6b0 ->
`Collide__bodyVsTriangles` 0x4feb20) and streams nodes in. Does that change collision results?

**Answer: the streaming does not; one per-node gate does.** Same face set, same "one contact per body
per pass", synchronous loads for cars. The difference that stays is the per-map-node bounding-sphere
gate of 0x4feb20 (prev state, no sweep), which src replaced with a swept gate over its own BVH boxes.
Cards 207/208 port it. The rest is listed under "Not ported" with reasons.

## Call chain (who walks what)
`PhysicsWorld__dispatchCollisionEvents` 0x4fc610 (event phase): for each queued object node, filter
`Phys__eventFilter` 0x4f9750, find its interval manager (0x4e9ac0), `IntervalMgr__touchObject` 0x4edf80,
then `IntervalMgr__collidePass` 0x4ee460 once per distinct manager (max 16); then
`DAT_006028dc = DAT_005dadbc` (frame stamp of the last pass).

0x4f9750 (the object is collided only if all hold):
```
!(obj+0x84 & 0x200000)  &&  |obj half extents (+0xc0..+0xc8)|^2 <= 10000      // objects > 100 m skip the map
&& some type-2 child (not 0x200000) whose typed body (+0xb4) has (body+0x58 & 0xa0) == 0
```
0x4ee460 per object `O` (node +0x30 of the interval object):
```
bounds94 = {kind 1, c = O+0xb4, h = O+0xc0 + 5}                 // prefetch box only
Collide__setPassFlags 0x504440(io+0x24, O):  DAT_006028ec = traffic/ped stuff;
    DAT_006028e8 = (O is type 1 && typed(O)+0xc & 0x10000)      // only type_trigger 0x4d4700 sets 0x10000
for each interval cell (io+0x148 list), map node N = cell+0x30:
    skip unless |O.c.y - N.c.y| <= N.h.y + O.h.y               // x/z handled by the interval lists
    if !(N+0x94 & 1) (not resident): prio = DAT_006028e8 ? 10 : 0
        ResourceNode__requestLoad 0x518a80(N, N+0x9c, 0, prio)   // prio 0: loaded NOW (0x51e500 -> vf10
                                                                 // -> loadData 0x519940); prio 10: queued
    T = typed(N); if T->+0x18 (faces) && DAT_006028e8 == 0: n += Collide__groundSoup(O, N, T, ground)
    children of N: subdivide into new cells (0x4ee1a0) or Collide__groundTree(O, child, ground, 0, &bounds94)
if (DAT_006028f8 candidates) n += Collide__candidatesTOI 0x4fc7e0(ground)   // ONE call per object, after all nodes
```
0x4fe6f0 `Collide__groundTree(O, N, ground, depth, bounds94)`:
```
if O box and N box are disjoint on any axis (|N.c-O.c|_k > O.h_k + N.h_k):
    if DAT_005dadbc > DAT_006028dc (first pass this frame) and N overlaps bounds94:
        Collide__groundPrefetch 0x4fe550 (queue loads, prio 10; residency only)
    return 0
if not resident: requestLoad(N, N+0x9c, 0, DAT_006028e8 ? 10 : 0)   // synchronous for cars
if resident: T = typed(N); if T+0x28 == N+0x80: ground = body of getInfo(1) of the native (same groundbody)
    if DAT_006028e8 && T+0x30 (POLY): return 0
    n = T+0x18 ? Collide__groundSoup(O, N, T, ground) : 0
for child C of N (record order), skip C+0x84 & 0x200000:
    type 1 (and C.id != O.id): n += groundTree(O, C, ground, depth+1, bounds94)
    type 2: n += Collide__physInstPair 0x4ff9e0(O, C)
```
0x4ff6b0 `Collide__groundSoup(O, N, T, ground)`: for each type-2 child P of O: reject when
`N+0xb0 kind 0 -> Bounds__testSphere 0x516640` / `kind 1 -> Bounds__testBox 0x516450` (ECX = P+0xb0,
args N centre/half) returns 1 (disjoint); body B = typed(P)+0xb4, skip B or its def flagged 0x24; clear
the 64 face-cache slots (stride 0xa0 at 0x6029e4); then flag 0x800 / sensor capsule / else
`Collide__bodyVsTriangles(B, T verts, T faces, ground, N+0xb0, nf, mask 0)` (notes/19 §7).

0x4feb20 node gate (first thing, root and every compound child alike):
```
s = B's other state (+0x20c ^ 1: the step-start pose), br = B.def+0x90 shape +0xc (bounding radius)
if |s.pos.y - N.c.y| > N.h.y + br  or  |s.pos.x - N.c.x| > N.h.x + br  or  |s.pos.z - N.c.z| > N.h.z + br:
    return 0                                     // whole node skipped for this body
... face filters (0x4f9860 swept sphere vs face AABB, 0x4fc450 OBB SAT) on a per-node mask, children
    recurse with the same N and the mask (each child re-runs this gate with its own s/br) ...
```

## Node bounds = record extra block
`Rpk__readRecord` 0x51e6b0: `flags & 1` -> bounds `{kind 1, centre = extra f32[0..2], half = extra
f32[3..5]}` (bytes 24..47 unused here) handed to `Resource__createInstance` 0x518410 (-> node +0xb0..+0xc8).
`EXTP` (0x519940) keeps the centre and grows each half so the box covers every child's box.

## Corpus facts (scratch parse of every shipped .rpk with PHYS; tools: python, this session)
- Every type-1 node with PHYS has the bounds block (0 without, in every shipped .rpk with PHYS:
  the 4 maps, the race tracks, carlot, garage, paintbooth, dealer*, DerbyArena ...).
- **Every PHYS face lies inside its own node box** (0 faces out by > 1 mm, e.g. city 197,286 faces).
- **Every type-1 child box lies inside its parent box** (city 58,209 children, ROC 10,893); EXTP is on
  1 node in city, 0 in ROC, so its growth is a no-op.
- Max faces per node: 20 (city2), so the 64-slot face cache never overflows.

## Consequences
1. Streaming is not a collision effect for cars: `typed+0xc & 0x10000` is set only by `type_trigger`
   (0x4d4700, byte search `81 4x 0c 00 00 01 00`), so for cars/bots `DAT_006028e8 == 0`, every node the
   walk reaches is loaded synchronously and collided in the same pass. Prefetch/unload only change
   residency. (Trigger objects get prio-10 loads and skip PHYS soups. src's triggers never meet the soup.)
2. Hierarchical culling (groundTree box tests, 0x4ff6b0 instance test) never hides a face a body
   can touch, because faces sit inside their node box and child boxes inside parent boxes. This holds
   if the object/instance boxes (O+0xb4, P+0xb0) bound their bodies. **Not traced**: who refreshes those
   boxes for a moving car (0x4edf80 -> 0x4f0000/0x4efd80 likely). See the doubts below.
3. The candidate set over the pass equals the merged soup's, except for **the 0x4feb20 node gate**:
   the exe drops a node's faces when the body's step-start bounding sphere misses the node box, even
   if the swept motion reaches a face. src tests its own BVH boxes against the swept sphere
   (`collect_hull_candidates`, world.cpp ~934, "INVENTED: merged soup"), so in a fast approach
   (corner first, motion > gap) src gets a hull candidate one step before the exe. That changes the
   contact (TOI, depth) of the impact step. Port: cards 207 (node table on the ground mesh + corpus
   check) and 208 (gate per map node, in walk order).
4. Candidate order: exe = cell order then the DFS of record children; src after 208 = DFS record order
   (`gather_ground_soup`). Only `candidatesTOI` ties (equal d) could differ; cell subdivision order
   (0x4ee1a0) is not traced.

## Not ported (with reason)
- Root-level 0x4feb20 pass (root br gate + 0x4f9860/0x4fc450 with the compound OBB) before the
  children: src runs only the child level. A compound root's sphere/OBB encloses its children's, so
  the root filters drop nothing the child filters keep, *if* the root bounding radius covers the
  children (not checked in src). Doubt, no card.
- Sensor (wheel capsule) branch: no node gate in 0x4ff6b0 beyond the instance test (consequence 2).
  src `mesh_segment_cross` already tests every face; unchanged.
- 0x4f9750's 100 m half-extent filter and the 0xa0 body filter: no shipped car comes near 100 m.
- Streaming/unload of map nodes (residency, memory): no collision effect (consequence 1).

## Doubts
- O/P box refresh for moving objects not traced (consequence 2 assumes they bound the bodies).
- `IntervalMgr__collidePass` 0x4ee460 is only partly read (cell subdivision 0x4ee1a0/0x4f0b40, x/z
  interval culling): only its gate conditions and the single 0x4fc7e0 call are used here.
