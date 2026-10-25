---
id: 215
status: implemented
branch: card/215
depends: [214]
---
# GroundRef: addTraffic / addTrafficCar / Traffic__spawnCarNear on the car pool

## Goal
Turn the `GroundRef.addTraffic` (0x4dc9e0, Java name `addTraffic`, native `addTrafficN`) and
`GroundRef.addTrafficCar` (0x4dd090) stubs into the exe's abstract spawn on top of cards 213/214,
plus `Traffic__spawnCarNear` 0x416800. vmboot `--city` reaches 19 addTraffic and 45
addTrafficCar calls (Valocity.java:95-163). Game instances for traffic cars (pre-created
`traffic_car` objects, park pool, type_car getInfo 0x38 take-over) stay out: notes/29 §5.

## Exe evidence
rand15 = `msvc_rand() & 0x7fff`; every rand call below is listed, in order.
### 0x4dc9e0 GroundRef.addTraffic(GameRef type, int n, float a, float b, float c) -> int (asm-checked)
```
T = ground_traffic(self); if (n < 1 || !T) return 0
density = Config.trafficDensity             // 0x47c080; OptionsDialog default 1.0
make the type resident                      // src: car_data(rt, type) loads its cfg
d = 0.5
c = wheels ? ((z2 + z3) - (z0 + z1)) * 0.5 : 2.0      // getInfo(type, 0x40): always overwrites c
d = wheels ? (z2 + z3) / ((z2 + z3) - (z0 + z1)) : 2.0 // getInfo(type, 0x46): always overwrites d
    // wheels = M+0x1fb4 != 0; z_i = wheel template i mount z = CarData::wheels[i].template_pos[2]
    // (M+0x1fc0 + i*0xc8, notes/13 §6); i = 0..3 read whatever the count
notify = (M+0x2ec0 >> 1) & 1               // getInfo(type, 0x45) = CarData::notify
k = min(n, (int)(density * 20.0f))
repeat k: createScripted(type, "0,-10000,0,0,0,0", "traffic_car") + Traffic__parkInstance   // NOT ported
spawned = 0
repeat n:
    r = (int)((float)rand15 * 3.0517578e-05f * (float)(nRoads - 1))      // last road never chosen
    car = spawn_on_road(r, 1.0, 2.0, 4.0, 1.0)                           // card 213
    if (!car) continue
    car.type_link = type                      // +0x144, type id +0x14c
    car.seed  = (rand15 * 0xffff) >> 15
    car.max_speed = (float)rand15 * 0.00023735895f + 13.611111f
    car.accel = (float)rand15 * 1.2207031e-05f + 0.7f
    car.k18 = c; car.k1c = d; car.k14 = a; car.k20 = b; car.k24 = 1.0
    car.notify = notify != 0
    car.lateral = (float)rand15 * 3.0517578e-05f * 2 - 1.0f
    TrafficCar__refreshInstance(car)          // 0x4107a0 -> 0x410680: instance side, NOT ported
    spawned++
return spawned
```
### 0x4dd090 GroundRef.addTrafficCar(GameRef inst, Vector3 pos) -> int
```
if (!inst) { log "vm_ground_addtrafficcar: Traffic car instance is null ..."; return 0 }   // src has this
T = ground_traffic(self); if (!T) return 0
car = 0
if (!pos) {
    m = #roads whose first lane (Road::node[0]) has !live
    if (m) { r = (int)((float)rand15 * 3.0517578e-05f * (float)m) % m; car = spawn_on_road(r-th such road, 1,2,4,1) }
    else if (nRoads > 0) { r = (int)((float)rand15 * 3.0517578e-05f * (float)(nRoads - 1)); car = spawn_on_road(r, 1,2,4,1) }
} else car = spawn_near(pos.x, pos.y, pos.z, 0.0, 1.0, 2.0, 4.0, 1.0)
if (car) {
    car.instance_link = inst                  // +0x130, id +0x138
    getInfo(inst, 0x38, &car)                 // type_car take-over: NOT ported (freezes the car, notes/27 §3)
    car.notify = CarData(inst).notify         // getInfo(inst, 0x45)
    0x4ec210(...)                             // reparent: NOT ported (ECX not asm-checked)
    car.max_speed = (float)rand15 * 0.00033908422f + 19.444445f
    car.accel = (float)rand15 * 1.2207031e-05f + 0.7f
    car.lateral = (float)rand15 * 3.0517578e-05f * 2 - 1.0f
    car.seed = 0
    TrafficCar__refreshInstance(car)          // NOT ported
}
return car                                    // exe: raw pointer; src: card 213 handle (index + 1)
```
### 0x416800 Traffic__spawnCarNear(x, y, z, dist, p1, p2, p3, p4) (asm-checked)
```
X = nearest_cross(x, y, z, dist, false); if (!X) return 0
N = X.links.size(); free = #links whose lane is !halted; live = #links whose lane is !halted && live
if (free == 0) return 0
if (free == N && (live == 0 || live == N)) lane = links[rand15 % N] lane        // may be halted
else if (live == 0)  lane = (rand15 % free)-th !halted link lane
else                 lane = (rand15 % live)-th (live && !halted) link lane
return spawn_on_road(lane.road, p1, p2, p3, p4)    // Lane::road (+0x10) node id
```
`rand15 % k` is integer modulo (no float scaling) here.

## Files
- edit: src/game/traffic.h / traffic.cpp — `spawn_near` (0x416800).
- edit: src/script/natives.cpp — `gr_add_traffic_n` (0x4dc9e0) and `gr_add_traffic_car`
  (0x4dd090): replace the SERVICE MISSING comments; graph via
  `game_rt()->ground_traffic(value_handle(a[0]))` like `gr_get_nearest_cross`; type data via
  `game::car_data(*rt, value_resource(a[1]))`; `Config.trafficDensity` read like
  src/game/cursor.cpp `read_sensitivity` (INVENTED default 1.0 when statics are missing). Each
  NOT-ported line above is one `// shortcut:` line. `remTrafficCar`/`notifyTrafficCar` stay stubs.
- edit: src/tools/vmboot.cpp `city_checks` (card 211): after the run, check on the map's graph:
  pool has cars; every lane's car list is ascending in `pos` and each `pos` is in
  `[0, lane path length]` (0x40ce00/0x40d1e0); the heap is a min-heap by time (0x41a950) and
  its size equals the number of lanes with cars and `pending_events == 1` (0x40cbd0: no lane is
  live in src).

## Done when
- each ported function has its `// 0xADDR` comment; handle form and defaults are `INVENTED:`.
- `make -C src clean check` green; `--selfcheck`/`--race` output unchanged; `--city` checks pass
  (run vmboot with `ulimit -v` + `timeout`, see CHECKPOINT environment notes).
- card frontmatter: status implemented, branch name.
