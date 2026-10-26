---
id: 229
status: merged
branch: main (card/229 timed out; committed from the working tree)
depends: []
---
# Physics: per-root P records and the object box O+0xb4 (updateBounds + boundsFromInstances)

## Goal
Give each compound root a list of its "P records" (the exe's type-2 `_physinst` nodes under the
car's game object: car_root, the car's part_comp, the car's bbone, one wheelbone per wheel) and
compute the object box the exe builds every event phase: per record the swept box of
`PhysBody__updateBounds` 0x504180, then their AABB union `GameObject__boundsFromInstances` 0x503c20.
Nothing reads the box yet (the road keep-alive walk is a later card), so no behaviour changes; the
race numbers must stay identical. Background: notes/35 (§1 table, §2, §4), notes/32 §2.

## Exe evidence (cleaned pseudocode)
### 0x524620 GameObject__createPhysInstance(H, 0, body, name) — seed of a P record
```
P = new type-2 instance under H's game instance, P+0x84 |= 0x200
typed(P)+0xb4 = ref(body)
P.kind (+0xb0) = 0
P.centre (+0xb4..+0xbc) = body active pos
P.half   (+0xc0..+0xc8) = br, br, br        // br = body def shape +0xc, 0 if no shape record
```
br values (0x4f7d60 Physics__createBox; src `Shape::bounding_radius()` already matches):
car_root box 1,1,1 -> sqrt(3)+0.05; part_comp (type 6, no mesh) -> 0; mesh -> max|v|+0.05;
sphere -> r+0.05.

### 0x5001c0 PhysBody__integrateTree — compound children are rigid
```
if ((body+0x58 & 0x81) != 1) return                   // asleep or inactive: no box update
if (body is a compound child, +0x58 & 4):
    o.pos = parent.o.pos + R(parent.o.rot) * rec.off   // rec = body+0x1c, off at +4..+0xc
    o.rot = parent.o.rot * rec.rot
else integrate (RK4)
for each child: integrateTree(child)
if (body+0x50 /*has P*/) PhysBody__updateBounds(body)
queue the compound root's object (root+0x38)
```
So every record's start/end world position = root start/end pose applied to its root-frame offset.

### 0x504180 PhysBody__updateBounds(body)   (a = active/step-start state, o = integrated state)
```
P.kind = 1
P.centre = (a.pos + o.pos) * 0.5                       // per axis
P.half   = |a.pos - o.pos| * 0.5 + br                  // per axis, fabs
```
### 0x503c20 GameObject__boundsFromInstances(O)   (event phase, via 0x5040f0 -> 0x504090)
```
first = true
for each direct child C of O with C+0x7c == 2 (type 2) and C.kind != 2:
    lo = C.centre - C.half; hi = C.centre + C.half
    if first: min = lo, max = hi, first = false
    else: min = min(min, lo), max = max(max, hi)        // per axis
if (!first) { O.kind = 1; O.half = (max - min) * 0.5; O.centre = min + O.half }
// no children: O keeps its old box
```
### 0x4992e0 Vehicle__createCar tail (0x49a5dd) — first O box
```
O.centre = part_comp active pos; O.half = (10, 10, 10); O.kind = 0
```

## src mapping
- Root = `RigidBody` in `World::bodies` (car_root = `CarData::root_body`). Step-start state =
  `StepState::start` (world.cpp `World::step`), integrated pose = `body.pos/rot` after
  `integrate_body`. The event phase is the `if (converged || it == 19)` branch in `World::step`
  (exe: 0x502060 H -> purgeEvents/boundsFromInstances -> dispatch).
- Records live on the root, offsets in the root frame (INVENTED storage, like
  `collision_children`): the exe keeps real child bodies; flattening is exact because 0x5001c0
  places children rigidly.

## Pseudocode to implement
physics.h, in `struct RigidBody` (next to `collision_children`):
```
// INVENTED storage: the exe's type-2 P nodes of this root's game object, flattened to
// root-frame offsets (children are rigid, 0x5001c0). br = shape +0xc (0x524620).
struct InstRecord { Vec3 offset; float br; };
std::vector<InstRecord> inst_records;
// O+0xb0 kind, +0xb4 centre, +0xc0 half (0x503c20).
int obj_kind = 0; Vec3 obj_centre{0,0,0}; Vec3 obj_half{0,0,0};
```
world.cpp, new static helper, called in `World::step` inside `if (converged || it == 19)` **before**
`add_hull_contacts`:
```
// 0x504180 per record + 0x503c20 union, for every awake dynamic root with records.
static void update_object_boxes(const std::vector<StepState> &states)
    for s in states:
        b = *s.body
        if (b.inst_records.empty() || b.asleep || !is_dynamic(b)) continue
        R0 = pose_rotation(Pose{s.start.pos, s.start.rot}); R1 = b.rotation()
        first = true
        for r in b.inst_records:
            p0 = s.start.pos + mat_vec(R0, r.offset); p1 = b.pos + mat_vec(R1, r.offset)
            c = (p0 + p1) * 0.5
            h = (fabsf(p1.x-p0.x)*0.5 + r.br, fabsf(p1.y-p0.y)*0.5 + r.br, fabsf(p1.z-p0.z)*0.5 + r.br)
            lo = c - h; hi = c + h
            union as in 0x503c20 (first / min / max per axis)
        if (!first) { b.obj_kind = 1; b.obj_half = (max-min)*0.5; b.obj_centre = min + b.obj_half }
```
car.cpp:
- In `rebuild_compound`, after `com` is final (next to the `collision_children` rebuild), refill
  `root->inst_records` (car_root = `car->root_body`):
  ```
  root->inst_records.clear()
  {offset (0,0,0), br = root->def.shape is SHAPE_NONE in src, so use the exe value
       sqrtf(3.0f) + 0.05f}                     // "car_root", createBox(1,5,1,1,1) 0x4992e0
  {offset = -com, br = 0}                       // "part_comp" (car frame origin), 0x4c1f90
  if the car cfg added at least one CompoundChild (the first `body` line, M+0xff0):
      {offset = that child's pos - com, br = that child's shape.bounding_radius()}   // "bbone"
      (remember children.size() before/after add_body_lines for the car cfg)
  for each wheel w with w.bone_mass > 0:        // "wheelbone", one per wheel
      {offset = hub - com, br = w.radius + 0.05f}
      // shortcut: create-time offset; type_car__vf7 0x4a99b0 moves the bone records with
      // suspension/steer every call (notes/35 §3), port vf7 to upgrade.
  // shortcut: "hub"/"a_arm" (template rec[-0x1a]) and "steer"/"cbone" records are not
  // parsed by src; add them when the cfg fields are (notes/35 §4).
  ```
- In `create_car_bodies`, after the first `rebuild_compound(rt, inst)` call: seed the O box
  (0x4992e0 tail): `root_body->obj_kind = 0; obj_centre = chassis_body->pos; obj_half = (10,10,10)`
  (only if `car->root_body`).

## Files
- edit: src/physics/physics.h (RigidBody fields), src/physics/world.cpp (update_object_boxes + call),
  src/game/car.cpp (rebuild_compound records, create_car_bodies seed)
- read only: nothing else needed (StepState at world.cpp ~341, is_dynamic ~435, World::step ~1406,
  rebuild_compound ~866, create_car_bodies ~1363)

## Done when
- each ported piece has a `// 0xADDR Name` comment; the storage comment says INVENTED; the two
  shortcuts above are `shortcut:` comments
- no other behaviour change: nothing reads `obj_*` yet
- `make -C src clean check` green, race time unchanged (659.414 s in the check output); no new unit
  tests or hand-made expected values
- card frontmatter: status implemented, branch name
