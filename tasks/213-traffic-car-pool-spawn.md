---
id: 213
status: implemented
branch: card/213
depends: [212]
---
# Traffic: car pool + ctRoad__spawnCar + lane insert

## Goal
Give `traffic::Graph` the traffic-car pool (Traffic+0x70) and port the road spawn that every
traffic native goes through: `ctRoad__spawnCar` 0x40ce00 picks a free lane of a road, finds a
random free gap along it and inserts an abstract car record into the lane's sorted car list
(0x40d1e0). No natives change in this card (card 215); the lane attach call at the end of the
insert is card 214. Source: notes/29 §2.

## Exe evidence (offsets mapped to the new field names)
Car record (0x164 bytes in the exe). New `struct Car` in traffic.h, fields named by offset:
```
+0x00 serial (++g_car_serial, global 0x5e6508; also the free-list link in the exe)
+0x04 prev, +0x08 next, +0x0c lane          (lane car list; src: see Files)
+0x14 k14, +0x18 k18, +0x1c k1c, +0x20 k20, +0x24 k24, +0x28 k28
+0x2c max_speed, +0x30 accel, +0x34 k34, +0x38 k38, +0x3c k3c, +0x40 k40
+0x44 live_next (0)
+0x88 pos (distance on the lane path; counts DOWN to the lane end)
+0x8c..+0x94 world pos[3], +0x98 front_x, +0x9c front_z
+0xa8 lateral, +0xac kac, +0xb0 speed, +0xb4 kb4, +0xb8/+0xbc next-lane ref (0), +0xc0 kc0
+0xc4 lane (PathRef: +0xc4 lane, +0xc8 path = &lane.path, +0xcc dir), +0xd0/+0xd4 second ref (0)
+0xdc live (byte), +0xdd notify (byte), +0xe0/+0xe4/+0xe8 step times
+0x124 k124 (0), +0x128 k128 (0), +0x130 instance link (id +0x138), +0x144 type link (id +0x14c)
+0x154 seed, +0x158 k158, +0x15c, +0x160 behaviour
```
### 0x40eef0 TrafficCarPool__alloc / free list
```
if (pool.free_head) { c = free_head; free_head = c.link; return c }
return new slot (pages of 0x400 cars)          // src: stable storage, see Files
```
### 0x40c400 TrafficCar__ctor + 0x40e040
```
serial = ++g_car_serial
k1c = 0.5; k28 = 1.2; k34 = 0.06; k38 = 6.0; k3c = 0.01; k40 = -1; everything else listed = 0
live = 0; e0 = e4 = e8 = T.sim_time; kac = 0; speed(+0xb0) = 0; pos[] / front_* (+0x8c..+0x9c) = 0
```
### 0x40ce00 ctRoad__spawnCar(road, p1, p2, p3, p4)   (ECX = road)
```
n = #lanes of road with !lane.halted        // road+4 = first lane, lane+0x14 = next lane of the road
if (!n) return 0
lane = (rand15 % n)-th non-halted lane       // rand15 = msvc_rand() & 0x7fff (already & 0x7fff)
car = alloc + ctor
car.max_speed = 15.0; accel = 1.0; k34 = 0.06; k3c = 0.01; k158 = 0
car.k14 = p1; k18 = p2; k20 = p3; k24 = p4
car.next-lane refs = 0; car.lane = lane; path = lane.path; dir = -1; second ref = 0
car.lateral = 0; car.e0 = T.sim_time
free = 0; edge = p1
for c in lane.cars (list order = ascending pos):
    back = c.pos - c.k14 - ((c.speed - 2) * 0.5 + 1) * c.k24 - p3
    if (edge < back) free += back - edge
    edge = c.k20 + c.pos + p4 + p1
tail = lane.path.length(+0x74) - p3; if (edge < tail) free += tail - edge
if (free <= 0) { release car (0x40c730: unlink links, serial = 0); push on free list; return 0 }
r = (float)rand15 * 3.0517578e-05f * free
acc = 0; edge = p1
for c in lane.cars:
    back = (same as above)
    if (edge < back) { nacc = back + acc - edge; if (r <= nacc) break; acc = nacc }
    edge = c.k20 + c.pos + p4 + p1
car.speed = 0; car.kac = 0
car.pos = (r - acc) + edge
p = path_pos_offset_at(lane.path, car.pos, car.lateral /*0*/)   // 0x40dde0, dir -1 -> 0x4133f0
car.world = p
v = path_dir_at(lane.path, car.pos)                             // 0x40dd60, dir -1 -> 0x412ed0
car.front_x = p.x + car.k18 * v.x; car.front_z = p.z + car.k18 * v.z
ctLane__insertCar(lane, car)
return car
```
All arithmetic is float (SSE). Order of rand calls: one for the lane, one for r.
### 0x40d1e0 ctLane__insertCar(lane, car)
```
for c in lane.cars: if (car.pos < c.pos) { insert car before c; goto attach }
append car at the tail
attach: lane->vf3(car, -1)    // 0x40cbd0 = card 214
```

## Files
- edit: src/game/traffic.h — `struct Car` (fields above, only those used here plus the listed
  zeros; name them by meaning where given, else `kXX`), `Graph::cars` as `std::deque<Car>`
  (stable addresses) + `std::vector<uint32_t> free_cars`, `uint32_t car_serial` (INVENTED: per
  graph, the exe's counter is process-wide), per-lane `std::vector<uint32_t> cars` (car indices,
  list order; INVENTED: replaces the exe's intrusive prev/next), and
  `uint32_t spawn_on_road(size_t road, float p1, float p2, float p3, float p4)` returning
  car index + 1 (0 = none; INVENTED handle form).
- edit: src/game/traffic.cpp — the functions above with `// 0xADDR Name` comments. Lane of a
  road: `Road::node[0]` (+4) is the first lane's node id, `Lane::node14` (+0x14) the next one;
  node ids are 1-based over [cross_eq, cross_light, roads, lanes]. Use `msvc_rand()`
  (declared like src/game/car.cpp line 33). Leave one line
  `// shortcut: lane vf3 0x40cbd0 attach is card 214` at the insert.
- read only: src/game/traffic.h existing helpers `path_pos_offset_at`, `path_dir_at`.

## Done when
- each ported function has its `// 0xADDR` comment; container/handle choices are `INVENTED:`.
- `make -C src clean check` green; vmboot output unchanged (nothing calls the spawn yet).
- card frontmatter: status implemented, branch name.
