---
id: 230
status: todo
branch:
depends: [229]
---
# Runtime: map node -> road object link (0x5195a0 create, type_road vf3/vf5, keep-alive call)

## Goal
Give each map node of a ground Resource its runtime object state, in the same DFS order as
`CollisionMesh::map_nodes`, and add `Runtime::map_node_touched(ground, ni)`: what the exe does when
the physics walk overlaps node `ni` (notes/36 §1-§2): make the node "resident" (create its native
object once: only `native road` gametypes, via `Graph::bind_object`), then the keep-alive
(`type_road__vf5(obj, 1.0)` -> road vf1 = `Graph::keep_road_live`). Nothing calls it yet (card 231
adds the walk), so no behaviour changes in this card; race 659.414 s must stay unchanged.
Background: notes/36, notes/31 §2-§3.

## Exe evidence (cleaned pseudocode)
### 0x5195a0 ResourceInstance_Game__load (vf10), create part — first residency only
```
if (R+0x154 /*native object*/ == 0 && R+0x148 /*gametype*/) {
    type = ResRef__getTyped(R+0x148)
    if (type+0xc /*native handler*/) R+0x154 = handler->vf3(&link, params R+0x150)
    ... (script class / "Undefined GameType Constructor!" not relevant here)
}
```
### 0x4d42a0 type_road__vf3(link, params)
```
obj = new (0x20 bytes, zeroed)
ground = 0x4ea900(obj)                 // the node's container (+0xfc) = the map ground
if (Game__getInfo(ground, 0x39) == 0) obj+0x1c = 0      // no Traffic graph: stays unbound forever
else {
    obj+0x1c = Traffic__bindObject 0x41a3e0(params, obj)    // node or 0
    if (obj+0x1c) { typed(node)+0xc |= 0x4000; node+0x84 |= 0x100 }   // 0x4ebdd0
}
```
### 0x4fe852 (in Collide__groundTree 0x4fe6f0), same block at 0x4ef475 in 0x4ee460
```
T = N->vf3(1.0)
if ((N+0x84 & 0x100) && T+0x4c /*obj*/ && (T+0xc & 0x4000) && handler)
    handler->vf5(T+0x4c, 1.0)
```
Both flags are set exactly when bindObject returned non-zero -> in src: `traffic_node != 0`.
### 0x4d44d0 type_road__vf5(obj, f)
```
if (!obj+0x1c || !obj+0xc) return
if (f != 1.0) { LOD test }             // skipped: the walk always passes 1.0
obj+0x1c ->vf1()                        // ctRoad 0x40f8e0 -> keep_road_live; ctCross 0x40e380 -> no effect
```
src node ids (Graph::bind_object, traffic.cpp ~1366): road = eq + light + idx + 1, cross_eq = idx + 1,
cross_light = eq + idx + 1.

## Pseudocode to implement
runtime.h, in `struct Resource` next to `ground_body`:
```
// INVENTED storage: per map node of ground_body's soup (same DFS order as
// CollisionMesh::map_nodes): its RSD and the native object created on first
// residency (0x5195a0). traffic_node = obj+0x1c (Traffic__bindObject result).
struct MapObject { uint32_t record = 0, gametype = 0; std::string params;
                   bool created = false; uint32_t traffic_node = 0; };
std::vector<MapObject> map_objects;
```
and a public `void map_node_touched(Resource *ground, size_t ni);` on Runtime.

runtime.cpp:
- `gather_ground_soup`: add an out param `std::vector<Runtime::Resource::MapObject> *objs`; push one
  entry **right next to `nodes->push_back(node)`** (same order): `record = rec.id`, gametype/params
  from `rpk_rsd_fields(inst.rsd, ...)` (0 / empty when there is no RSD). Pass it through the
  recursion; `create_ground_body` passes `&r->map_objects` (clear it first).
- Generalise `native_ground_type(rt, gametype, base)` into `native_type_is(rt, gametype, base, name)`
  (same body, `ref.native == name`); call sites pass "ground".
- New:
```
// 0x4fe6f0 residency + 0x5195a0 create (type_road__vf3 0x4d42a0), then the
// 0x4fe852 keep-alive (type_road__vf5 0x4d44d0 with f = 1.0 -> node vf1).
void Runtime::map_node_touched(Resource *ground, size_t ni)
    if (!ground || ni >= ground->map_objects.size()) return
    MapObject &m = ground->map_objects[ni]
    if (!m.created) {
        if (!m.gametype) m.created = true                    // 0x5195a0: no gametype, no create
        else if (!native_type_is(*this, m.gametype, ground->package, "road"))
            m.created = true   // shortcut: only native road objects are created; walk/scripted
                               // map gametypes (notes/31 §1) need their own create, port then.
        else if (!ground->traffic) return   // INVENTED: src builds the graph lazily (load_ground);
                                            // retry until it exists instead of binding 0 forever.
        else { m.created = true; m.traffic_node = ground->traffic->bind_object(m.params, m.record) }
    }
    if (!m.traffic_node) return
    traffic::Graph &g = *ground->traffic
    base = g.cross_eq.size() + g.cross_light.size()
    if (m.traffic_node > base) g.keep_road_live(m.traffic_node - 1 - base)   // ctRoad vf1 0x40f8e0
    // cross node: vf1 0x40e380 only reads the refcount -> nothing
```
(`bind_object`'s object argument is the record id, like resman's card-218 use; nothing reads it back.)

## Files
- edit: src/game/runtime.h (MapObject, map_objects, map_node_touched decl),
  src/game/runtime.cpp (gather_ground_soup, native_type_is, create_ground_body, map_node_touched)
- read only: src/game/traffic.h (Graph::bind_object, keep_road_live, cross_eq/cross_light)

## Done when
- each ported piece has a `// 0xADDR Name` comment; storage comment says INVENTED; the shortcut above
- `map_objects.size()` equals `ground_body`'s `map_nodes.size()` by construction (same push site)
- `make -C src clean check` green; **report the race time** (expected 659.414 s unchanged, nothing
  calls the new function yet) and any check delta; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
