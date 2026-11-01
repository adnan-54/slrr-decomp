---
id: 261
status: todo
branch:
review:
depends: [258, 259]
---
# traffic: materialize creator — createScripted "traffic_car" under the road/cross stand-in

## Goal
Replace card 258's creator stub in runtime.cpp (`run_animate_callbacks`, the 0x4bac10 call
site of `flush_spawns`) with the instance side of TrafficCar__materialize 0x40d4e0's create
branch: resolve the car's type, create the scripted `traffic_car` instance with the params
string 258 already builds, return its id. No dependency on 262: Part__initInstance 0x4c1f90
sleeps the new car's root, so without the 0x38 take-over it stays at its spawn pose; card 264
adds the take-over call (0x40d8fd) and the pose push. Uses 259's parent stand-in. Notes: `notes/42-traffic-410680-services.md` §11, §15, §18.

## Exe evidence (0x40d4e0 create branch, asm-checked, notes/42 §11)
```
if (*budget == 0) return 0; (*budget)--;                       // already ported (258)
buf = sprintf("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d" (0x593614), pos, ypr, seed)   // already ported
obj = 0x523c80 GameObject__createScripted(&parent /*road +0x68 / cross +0x44 bound object*/,
          &car+0x144 /*type link, id +0x14c = Car::type_id*/, buf, "traffic_car" (0x593638))
0x40cac0 ResRefLink__set(ECX = &car+0x130, obj)                // inst_id = obj+0x80 (id)
getInfo(ECX = 0x5fde90, &car+0x130, 0x38, &car)                 // 0x40d8fd: card 264 hook
if (car.notify) queue(T+0x3130, 0, 0x10, "car_add %d %d" (inst_id, car), 0)   // sink not modelled
```
Same create call shape already in src (car.cpp bot_trigger, 0x483bd0):
`g_rt->instantiate(ty, b.self, "bot_trigger", str, true, nullptr)`.

## Design
Creator lambda (runtime.cpp, `inst` = the ground Resource being animated):
```
[this, inst](const traffic::Car &car, uint32_t parent, const std::string &params) -> uint32_t {
    Resource *ty = resolve(car.type_id);       // +0x144 type link (id +0x14c)
    // INVENTED: parent is a road/cross map record id in src (no map node instance, 0x5195a0
    // not ported); the ground instance stands in, the same choice as card 259's reparent hook.
    Resource *p = parent ? inst : nullptr;
    Resource *r = ty && p ? instantiate(ty, p, "traffic_car", params, true, nullptr) : nullptr;
    return r ? r->id : 0;                       // 0x40cac0: inst_id = obj+0x80
}
```
Leave `// shortcut: getInfo(&car+0x130, 0x38, &car) take-over (0x40d8fd) is card 264` right after
the create (264 replaces it).
Update the `// shortcut: instance creation is card 261` comment.

## Files
- edit: src/game/runtime.cpp (creator lambda at the flush_spawns call)
- read only: src/game/traffic.h (`Graph::Creator`), src/game/car.cpp (bot_trigger create, ~3274)

## Done when
- `// 0x40d4e0` / `// 0x523c80` comments on the create; the parent stand-in is `INVENTED:`
- `make -C src check` green (incl. `--city`); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
