---
id: 231
status: implemented
branch: card/231
depends: [230]
---
# Physics: event-phase map node walk with the object box (groundTree overlap -> road keep-alive)

## Goal
Port the part of the collision dispatch that keeps roads alive (notes/36 §1): every event phase,
each queued colliding object O (a root with P records, card 229) whose box passes the event filter
visits the map nodes its box overlaps, in tree order with subtree skip on a miss, and for each
overlapped node the runtime runs `map_node_touched` (card 230: create road object once + road vf1).
With this, `Graph::step`'s active-lane walk (card 220) gets live lanes in the city.
**Race numbers may move. Report the race time (was 659.414 s) and every vmboot check delta
(--race, --city), plus the number of live lanes/cars if you can print them without a new check.**
The race map has no road nodes (notes/36 §5), so a race change is unexpected: say so if it happens.

## Exe evidence (cleaned pseudocode)
### 0x4fc610 PhysicsWorld__dispatchCollisionEvents (per queued object O)
```
O+0x84 &= ~0x10000000
if (!Phys__eventFilter 0x4f9750(O)) skip     // |O.half|^2 <= 10000 and an awake type-2 child
mgr = GameObject__intervalMgr(O); IntervalMgr__touchObject(O); ... IntervalMgr__collidePass(mgr)
```
### 0x4ee460 IntervalMgr__collidePass -> 0x4fe6f0 Collide__groundTree(O, N, ...)
```
miss = O.half.x + N.half.x < |N.x - O.x| || O.half.z + N.half.z < |N.z - O.z| ||
       O.half.y + N.half.y < |N.y - O.y|          // touching counts as overlap
if (miss) return 0                                // N's children are NOT visited
make N resident (prio 0 for cars: now), then the keep-alive block 0x4fe852 (card 230)
... faces (already ported elsewhere, do not touch)
for each type-1 child C of N: groundTree(O, C, ...)
```
The exe order inside one pass is the interval manager's sweep-and-prune pair order; src has no
interval manager. Corpus fact (notes/36 §1): in city.rpk every node box lies inside all its
ancestors' boxes, so the overlapped SET is the same as the exe's; only the order is INVENTED.

## src mapping
- O = an awake dynamic root with `inst_records` (cars); O box = `obj_centre/obj_half`, set by
  `update_object_boxes` (world.cpp ~1409) in the event phase (`if (converged || it == 19)` in
  `World::step`, ~1522). Exe O box and node boxes are world frame.
- Map nodes: `World::statics` bodies whose `def.shape.mesh` has `map_nodes` (physics/mesh.h
  `CollisionMesh::MapNode`: `centre/half` record box in the body frame, `end` = index past the
  subtree, DFS order). The existing face gate in `collect_hull_candidates` (world.cpp ~964) shows
  how to put a node box in world space (`gp.pos + mat_vec(gr, centre)`, boxes not rotated).
- Runtime side: `Runtime::map_node_touched(Resource *ground, size_t ni)` from card 230.

## Pseudocode to implement
physics.h, in `struct World` next to `trigger_event`:
```
// 0x4fe6f0 / 0x4ee460: an object's box overlapped map node `node` of the
// static `ground` body; the game runtime makes it resident and runs the
// keep-alive (0x4fe852). Unset = nothing happens.
std::function<void(RigidBody *ground, size_t node)> map_node_touch;
```
world.cpp, new static helper, called in `World::step` right after `update_object_boxes(states)`:
```
// 0x4fc610 dispatch -> 0x4ee460 collidePass -> 0x4fe6f0 groundTree, map node part.
// INVENTED order: DFS over map_nodes instead of the interval manager's SAP order
// (same overlapped set, notes/36 §1).
// shortcut: Collide__groundPrefetch (+5 m box, async residency) not ported.
static void walk_map_nodes(World &w, const std::vector<StepState> &states)
    if (!w.map_node_touch) return
    for s in states:
        b = *s.body
        if (b.inst_records.empty() || b.asleep || !is_dynamic(b) || b.obj_kind != 1) continue
        if (dot(b.obj_half, b.obj_half) > 10000.0f) continue        // 0x4f9750 size part
        for g in w.statics:
            if (!g->def.shape.mesh || g->def.shape.mesh->map_nodes.empty()) continue
            nodes = g->def.shape.mesh->map_nodes; gp = g->pose(); gr = g->rotation()
            for (ni = 0; ni < nodes.size();)
                c = gp.pos + mat_vec(gr, nodes[ni].centre); h = nodes[ni].half
                if (b.obj_half.x + h.x < fabsf(c.x - b.obj_centre.x) ||
                    b.obj_half.z + h.z < fabsf(c.z - b.obj_centre.z) ||
                    b.obj_half.y + h.y < fabsf(c.y - b.obj_centre.y)) {
                    ni = max(nodes[ni].end, ni + 1); continue          // children skipped
                }
                w.map_node_touch(g, ni); ni++
```
(Use whatever dot/vec helpers world.cpp already has; `end` is past the last descendant.)

runtime.cpp:
- `create_ground_body`: set `b->owner = r` (body+0x38 resource ref, like the trigger body at
  runtime.cpp ~372); leave `owner_id` 0.
- `Runtime::Runtime`: next to `trigger_event`, set
  `process_world().map_node_touch = [this](RigidBody *g, size_t ni) { map_node_touched(static_cast<Resource *>(g->owner), ni); };`

## Files
- edit: src/physics/physics.h (World::map_node_touch), src/physics/world.cpp (walk_map_nodes + call),
  src/game/runtime.cpp (owner on the ground body, hook in the ctor)
- read only: src/physics/mesh.h (MapNode), src/game/runtime.h (map_node_touched from card 230)

## Done when
- `// 0xADDR Name` comments on the walk; INVENTED order + prefetch shortcut comments as above
- `make -C src clean check` green or every failing check explained; **report race time and all
  check deltas** (race numbers may move: say by how much and in which check); no new unit tests or
  hand-made expected values
- card frontmatter: status implemented, branch name
