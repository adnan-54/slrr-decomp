---
id: 264
status: todo
branch:
review:
depends: [261, 262]
---
# traffic: instance pose push (0x4118f0 tail) + materialize take-over call (0x40d8fd)

## Goal
A taken-over traffic car (card 262) is moved only by the traffic step: port the tail of
TrafficCar__pushInstance 0x4118f0 that builds the 16-value pose block and hands it to the
instance's typed vf7 (type_car__vf7 0x4a99b0 = src `game::car_set_matrix`). Also add the
`getInfo(&car+0x130, 0x38, &car)` take-over that 0x40d4e0 does right after creating the
instance (0x40d8fd), in card 261's creator. Notes: `notes/42-traffic-410680-services.md` §16, §17.

## Exe evidence (asm 0x411a62..0x411ef6, notes/42 §17)
`dt` = `sim_time - car+0xe8` taken **before** `car+0xe8 = sim_time` (0x411950..0x41195d).
After `0x4102d0` (src `car_advance`):
```
sig = (car+0xb8 && car+0xc0 > -1) ? *(next_owner+0x1c + kc0*0x60 + 0x5c) : 0  // next cross path +0x5c
if (!sig) sig = 0x40d970(ECX = &car+0xc4)   // owner(+0xc4) && idx(+0xcc) >= 0 ? owner path[idx]+0x5c : 0
if (!sig) sig = car+0x128
p = (front_x, world_y, front_z)
if (!0x40da20(ECX = &car+0xc4, p.x, p.z, &p.y)) 0x40da20(ECX = &car+0xd0, p.x, p.z, &p.y)
f = p - world; n = |f|; if (n != 0) f *= 1.0f / n
s = (f.z, 0, -f.x); n = sqrt(s.x*s.x + s.z*s.z); if (n != 0) s *= 1.0f / n
u = f x s
if (car+0x138 == 0) return                                   // 0x411c49
pos = world + f * (car+0x18 * 0.5f)                          // 0x593590 = 0x3f000000
M columns s, u, f (0x567250 x3); ypr = 0x569970(M)
a = car+0xb4 * car+0x3c; if (|a| > car+0x34) a *= car+0x34 / |a|      // abs mask 0x5935c0
k = min(2*dt, 1.0f); car+0x158 = (a - car+0x158) * k + car+0x158       // MINSS with 0x593598 = 1.0
vel = -(car+0xb0) * f
h = atan2(-car+0xa0, -car+0xa4)                              // FPATAN, ST1 = -heading_x, ST0 = -heading_z
d = h - ypr[0]; if (d > 3.14159274f (0x40490fdb)) d -= 6.28318548f (0x40c90fdb);
                if (-3.14159274f (0xc0490fdb) > d) d += 6.28318548f
0x4eb3f0(&car+0x130, block "fffffffffffffffi" (0x5935e8):
         pos.xyz, ypr[0..2], vel.xyz, 0, 0, 0, 2*d, -(car+0xb0 * dt), car+0x158, sig)
```
The block is binary (0x56a050: `'f'` = {2, (float)double}, `'i'` = {1, int32}); no text round trip.
0x4a99b0 reads pos3, ypr3, vel3, angvel3, steer, wheelspin delta, pitch-add, int dir (notes/09 §514).

0x40d4e0 at 0x40d8fd (after `0x40cac0 ResRefLink__set(&car+0x130, obj)`):
`getInfo(ECX = 0x5fde90, &car+0x130, 0x38, &car)` = card 262's `car_traffic_ptr` with this car's handle.

## Design
- traffic.h `Car`: `k158` becomes `float` (+0x158 is a float, MOVSS 0x411d35). Graph gets
  `std::function<void(uint32_t inst, const float v[15], int32_t sig)> push_pose;` (INVENTED bridge
  for 0x4eb3f0, same style as `reparent`).
- traffic.cpp `car_push_instance`: keep `dt = t - car.e8` before the store; replace the
  `// shortcut: instance pose tail` with the pseudocode. Reuse `car_ref_height` (0x40da20, as in
  `materialize`), `game::mat_to_ypr` (0x569970) and the Mat34 column fill already in `materialize`
  (~3609). `sig`: next owner = `kc0 >= 0 ? cross_by_node(g, next_lane_a)` path `kc0`; current =
  `dir >= 0 ? cross_by_node(g, path_lane)` path `dir`, value `CrossPath::k5c` (= exe +0x5c,
  notes/19 §110); INVENTED null/bounds guards; else `(int32_t)car.k128`. Call `g.push_pose` if set.
- runtime.cpp `load_ground`: set `g->push_pose = [&rt](inst, v, sig) { if (Resource *i =
  rt.resolve(inst)) game::car_set_matrix(rt, i, v, v + 3); }`. `// shortcut:` vel/angvel/steer/
  spin/pitch-add/sig of 0x4a99b0 are dropped (src `car_set_matrix` takes the setMatrix 6 only).
- runtime.cpp, card 261's creator: right after the instance is created, call
  `game::car_traffic_ptr(*this, r, inst->traffic, handle)` with `handle = &car - inst->traffic->cars.data() + 1`
  (`// 0x40d8fd`); replace 261's `// shortcut:` naming card 262. Also drop the matching shortcut in
  traffic.cpp `materialize` ("getInfo(&car+0x130, 0x38, &car) type_car take-over (card 261)").

## Files
- edit: src/game/traffic.cpp, src/game/traffic.h, src/game/runtime.cpp
- read only: src/game/car.h (`car_set_matrix`, `car_traffic_ptr`, `mat_to_ypr`)

## Done when
- `// 0x4118f0` / `// 0x40d970` / `// 0x40d8fd` comments; `push_pose` and guards `INVENTED:`
- `make -C src check` green (incl. `--city`); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
