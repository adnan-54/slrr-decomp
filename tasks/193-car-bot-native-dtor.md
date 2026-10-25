---
id: 193
status: todo
branch:
depends: []
---
# runtime: type_car / type_BOT native dtors on unload (destroyed cars leave the physics world)

## Goal
`Runtime::unload` (src/game/runtime.cpp ~963, the "+0x154 native instance dtor" step) only
removes ground and trigger bodies. After a race, Gamemode.destroyBots -> Bot.deleteCar ->
`car.destroy()` (Gamemode.java:678-708, Bot.java:192-204) leaves the 4 bot cars active in the
process world (vmboot probe: 5 active roots + 9 sleeping destroyed Garage/CarMarket Vehicles in
the EventList, notes/18 s9 line 287). Port the physics part of the type_car dtor and drop the
bot payload in the type_BOT dtor, then add one vmboot `--race` check. Background: notes/18 s9.

## Exe evidence (cleaned)
### 0x49beb0 type_car vf4 (dtor), T = car typed, R = its type record
```
if ((R.flags+0x2ec0 & 2) && (inst.id >> 16) != 0xffff && inst.parent)   // traffic cars only
    queue(parent+0xf0, 0x10, "car_rem %d %d" % (inst.id, T+0x2054))     // not ported (traffic)
... release wheel / part / render handles (no src state) ...
b = T+0x13ec                       // "part_comp" body = CarData::chassis_body
while (b->flags+0x58 & 4) b = b->parent(+0x20)    // child -> its parent: car_root (root_body)
0x4fadf0(b)                        // world remove of the root; recursively deletes children
                                   // (+0x24 child list), contacts (0x4f6850)
delete callback lists +0x20f8/+0x2144/+0x2190; free T (0x2284 bytes)
```
### 0x4fadf0 PhysicsWorld remove body (src `World::remove_body`)
```
unlink b from its parent's child list (+0x20 parent, +0x24 head, +0x28 next)
for each child c: c.next = 0; c.parent = 0; 0x4fadf0(c)        // children die first-in-list
unlink handle links +0x38.. / +0x48..; if b+0x30 contacts: 0x4f6850(b)
free b (0x218); world.count(+0x54)--
```
### 0x4805a0 type_BOT vf4 (dtor)
```
release 9 handles (0x4e9450); 0x47f280 base dtor; free the 0x2730 typed
```
(0x47f280's per-tick list handling is not traced: erasing the src payload stands in for it.)
### 0x4c3a50 type_part vf4: `0x4fadf0(getPhysBody(handle)->+0xb4)`. src parts own no root
body (installed parts are compound children of the car), so nothing to port; say so in a comment.

## Steps
1. car.h: declare `void instance_dtor(Runtime &rt, Runtime::Resource *r);` (comment: `0x49beb0
   type_car vf4 / 0x4805a0 type_BOT vf4 / 0x4c3a50 type_part vf4`).
2. car.cpp `instance_dtor`:
   - `g_cars.find(r)`: if found and it has bodies: `World &w = process_world();`
     `if (c->chassis_body) w.remove_body(c->chassis_body);` then
     `if (c->root_body) w.remove_body(c->root_body);` (children first, as 0x4fadf0 recurses;
     remove_body already unlinks a child from its parent, 0x501d00), then set
     `c->chassis_body = c->root_body = nullptr`. Keep the CarData entry and its storage:
     WheelData::car points at it (car.h:39). INVENTED: the payload stays allocated (the exe frees
     T); null bodies make drive_cars / control_bots skip it (they already test `root_body`).
   - `g_bots.erase(r)` (0x4805a0 frees the bot typed).
   - comment for 0x4c3a50 (no src part body) and for `car_rem` (traffic only, not ported).
3. runtime.cpp `Runtime::unload`: call `game::instance_dtor(*this, r);` right after the trigger
   block (same "+0x154 native instance dtor" step); update the shortcut comment below it so
   car/bot are no longer listed as missing.
4. car.cpp opponent search (~2808, `for (const auto &e : g_cars) if (e.first->id == b.opponent)`):
   skip payloads with `!e.second->root_body` (a destroyed car's ref resolves to nothing in the
   exe). Also `car_body_instances()` already counts only root_body != null: unchanged.
5. vmboot (`--race`, after "no failed thread after the finish key", ~src/tools/vmboot.cpp:3420):
   collect the bot car ids seen at race start (the `native bots:` block ~1499 prints them; keep
   them in a vector) and check
   `c.check(n == 0, "after the finish key no world body belongs to a destroyed bot car (Bot.deleteCar, 0x49beb0; got N)")`
   counting roots in `process_world().bodies` and `.sleeping` whose `owner_id` is one of them.
   Also print `  world roots after the finish key: active %zu, sleeping %zu`.

Note: the 9 destroyed Garage/CarMarket Vehicles were sleeping in the world during race 1 too, so
removing them may shift race-1 numbers (bot distances, rpTotal); the existing checks are
bounds, report any that move.

## Files
- edit: src/game/car.cpp (new instance_dtor, opponent search), src/game/car.h (decl),
  src/game/runtime.cpp (Runtime::unload)
- edit (check only): src/tools/vmboot.cpp
- read only: src/physics/physics.h (World::remove_body/unlink), src/physics/world.cpp:224-270

## Done when
- `// 0x49beb0`, `// 0x4805a0`, `// 0x4c3a50` comments on the ported parts; INVENTED on the kept
  payload and the bot-erase stand-in
- `make -C src clean check` green (both vmboot modes); the new check passes
- card frontmatter: status implemented, branch name
