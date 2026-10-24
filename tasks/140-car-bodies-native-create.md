---
id: 140
status: implemented
branch: card/140
depends: [139]
---
# Chassis native create: car_root + part_comp bodies into the process world, asleep

## Why
`GameRef.create` on a car type record (`native car <cfg>`, e.g. Axis.rdb
`native car cars\racers\Axis_Data\scripts\_main_200S.cfg`, super chain to parts.rdb 0x640
Chassis) runs the native type manager's vf3 = `Vehicle__createCar` 0x4992e0 (`_type_car`
vtable 0x59dcd8 slot 3) before the script ctor. That is where the car's physics bodies enter
the process world. src `Runtime::instantiate` ignores the native line, so no car has a body.
RE: notes/08 "Car body creation, sleep/wake".

## Exe functions (5)
- `GameObject__createScripted` 0x523c80 / `GameObject__createNative` 0x5240e0 (the hook only):
```
inst = instantiate(type, parent, alias); typed = inst->typed
T = typed data of the type record (0x468b00(0x80000000))
if (T+0xc /*native manager*/) typed+0x4c = (T+0xc)->vf3(&ctx, params)   // BEFORE the <init>(I) thread
```
- `Part__initInstance` 0x4c1f90 (body part only):
```
Str__scan(params, "%f,%f,%f,%f,%f,%f,%d", &x,&y,&z, &yaw,&pitch,&roll, P+0x58)   // Vehicle.java:111 passes "0,0,0,0,0,0"
mat = ypr_to_matrix(yaw,pitch,roll)                                              // 0x5666d0
if (M+0xff0 >= 0) {                     // set by the cfg `body` line handler 0x4bf4e0; -1 = no body line
  def  = Physics__createBox(1.0f, 6, 0,0,0, 0)                                   // 0x4f7d60
  body = addBody(def, pos, mat); createPhysInstance(P, 0, body, "part_comp")    // 0x4e8a50 -> 0x4f7830 + 0x524620
  PhysBody__sleep(PhysBody__root(body))                                          // 0x506d90 / 0x480700
  body+0x58 |= 8
  // child shapes per M bone record (addChildBody 0x4f78d0 "bbone"/"steer"): NOT this card
}
```
- `Vehicle__createCar` 0x4992e0 (body part only):
```
car = Part__initInstance(ctx, params, new Vehicle)
car+0x13ec = getPhysBody(car)->+0xb4               // part_comp body (0x4eabc0)
if (car+0x13ec) {
  root = PhysBody__root(car+0x13ec)
  if (root asleep /*+0x58 & 0x80*/) PhysBody__wake(root)                        // 0x507060
  def = Physics__createBox(1.0f, 5 /*box*/, 1.0f, 1.0f, 1.0f, 0)
  cr  = addBody(def, root.pos, root.rot)            // active state of root (+0x64/+0xa4 + i*0xd4)
  createPhysInstance(car, 0, cr, "car_root")
  PhysBody__unlink(root); PhysBody__attachChild(root, cr)                       // 0x501d00 / 0x4f7b20
  // wheels (addChildBody "wheelbone"/"a_arm", suspension 0x4915d0): NOT this card
}
if (car+0x13ec) 0x5032d0(car+0x13ec)                // compound mass refresh: NOT this card
r = PhysBody__root(car+0x13ec); if (r && !(r asleep)) PhysBody__sleep(r)
```
- `PhysicsWorld__addBody` 0x4f7830: already ported (`World::add_body`, head insert into the
  active root `w+0x84`); add the counter `w+0x54 += 1`.
- `PhysBody__sleep` 0x506d90 / `PhysBody__wake` 0x507060 (root-level only):
```
sleep(b): if (!(b.flags & 0x80)) {
  if (b.parent == w+0x84 /*active root*/) { unlink(b); attachChild(b, w+0x88 /*sleeping root*/) }
  b.flags |= 0x80; zero vel, angular momentum, force, torque, pos correction; other state = active
  for each child c: sleep(c) }
wake(b): if (b.flags & 0x80) {
  b.flags &= ~0x80
  if (!(b.flags & 4)) { b+0xc = 0; b+0x10 = 100000.0f; b+0x14 = 0 }   // 0x47c35000
  if (b.parent == w+0x88) { unlink(b); attachChild(b, w+0x84) }
  for each child c: wake(c)
  // contact/listener requeue (w+0x134 -> w+0xbc): NOT this card }
```

## Files (3 + headers)
- src/physics/world.cpp + physics.h: `World` gets `std::vector<RigidBody *> sleeping` (= `w+0x88`
  child list, head insert like `bodies`), `int body_count` (`w+0x54`, incremented in
  `add_body`), `RigidBody::asleep` (flag 0x80) and a `RigidBody *parent` / `children` link
  (`+0x20/+0x24/+0x28`); `World::sleep(RigidBody*)`, `World::wake(RigidBody*)` per the
  pseudocode (move between `bodies` and `sleeping` only for root-level bodies). `step` keeps
  integrating `bodies` only. Bodies with a parent are not stepped on their own; mark that
  `shortcut: child bodies are carried but not integrated as a compound (0x4f7990/0x5032d0), upgrade with the compound card`.
  The `+0xc/+0x10/+0x14` wake fields: add only if a matching src field exists, else one
  `INVENTED:` comment saying they are not modelled.
- src/game/car.cpp + car.h: `CarData` gets `physics::RigidBody *chassis_body` (car+0x13ec,
  part_comp) and `physics::RigidBody *root_body` (car_root), owned by the car payload
  (unique_ptr storage next to `g_cars`). New `void create_car_bodies(Runtime &rt,
  Runtime::Resource *inst, Runtime::Resource *type, const std::string &params)`: parse the
  params with the 0x4c1f90 format (missing fields stay 0), run the 0x4c1f90 + 0x4992e0 body
  parts above on `process_world()`. `M+0xff0 >= 0` = the type's car cfg (`load_native_cfg(rt,
  type, "car", ...)`) has at least one `body` line (check the shipped parser's line list;
  if `CfgFile` has no body list for car cfgs, count lines with key `body`). Shape: part_comp =
  `SHAPE_MESH` without mesh, mass 1.0 (`INVENTED:` Poly box template of a mesh-less type 6
  not ported); car_root = `set_box({1,1,1}, 1.0f)`. Bind `car_data(rt, inst)` to the bodies.
- src/game/runtime.cpp `Runtime::instantiate`: after creating the node and before the
  THRD-CREATE spawn (0x523c80 order), if the type chain has a `native car` cfg ref, call
  `game::create_car_bodies(*this, r, type, params)`; delete the `(void)params` line. Other
  native kinds (part, ground, camera, …) stay unhandled: one `shortcut:` line.
- src/tools/vmboot.cpp (check only): after the run, `player.car.chassis` (Vehicle.java:48)
  resolves to a resource whose `CarData` has `root_body != nullptr`, `chassis_body->parent ==
  root_body`, and `root_body` is in `process_world().sleeping` (asleep: createCar ends with
  `PhysBody__sleep`); `process_world().body_count >= 2` and equals
  `2 * (number of car instances with bodies)`; print the counts.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, the new
  checks ok, still `failed: 0` and `final actualState: java.game.Track`. Report the car count
  (CarMarket creates the market cars on its map, CarMarket.java:491/792) and body_count.

## Next (not this card)
Card 141: native command handler (`type_car__vf14` 0x4a2e60 "wakeup"/"suspend"/"start"/
"brake"/"reset") through `Game__dispatchEvent` 0x47c760 step 2, so the car wakes and falls.
Later: car_root child shapes/wheels (0x4f78d0, suspension), compound mass 0x5032d0, native
ground `type_ground__vf3` 0x4bade0 for contacts.
