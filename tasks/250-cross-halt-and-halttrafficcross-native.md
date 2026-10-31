---
id: 250
status: todo
branch: card/250
depends: []
---
# traffic: ctCross__halt (Cross+0x40 writer) and the GroundRef.haltTrafficCross native

## Goal
Implement the exe's cross-halt body and wire the already-registered script native to it.
Card 249 wrote the `Cross::k40` comment but deliberately left the body unported; this card
ports it and gives `k40` a real src writer.

1. `ctCross__halt 0x40de70` (ECX = cross, args `float delay`, `char all`): stores
   `cross->k40 = T.sim_time + delay` (+0x40, store at `0x40de90`), then recycles every car
   on the cross's own intrusive car list (+0x8), then (when `all` or the link's flag) every
   car on each link lane's car list, through the per-car helper `0x40fae0`. It does **not**
   touch any lane's halt flag (+0xc4); lane halting belongs to `0x419980`/`0x419ab0`.
2. The script native `0x4de360` = `GroundRef.haltTrafficCross(Vector3,F)V` (already in
   src's table, currently a stub): `getInfo(self, 0x39)` -> traffic manager; if 0 return;
   `t->nearest_cross(x, y, z, 0.0f, false)`; `halt_cross(cross, delay, all = 1)`. Void.
3. Evidence only, **do not port**: `0x419980` (the other `0x40de70` caller, delay
   `0x3a83126f` = 0.001f, void `RET 4`), `0x419ab0`, and the global lane worklist
   `0x5e6538/0x5e653c/0x5e6540`. They are reachable only from `Traffic__haltPath
   0x4196f0`, which needs `Traffic__findRoute 0x417e50` (UNRESOLVED, notes/19 §2), so no
   src caller can exist yet.

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x40de70 ctCross__halt (names.json `ctCross__halt`; ECX = cross; `RET 0x8`)
Raw anchors: `0x40de8c CMP [EDI+8],0`, `0x40de90 MOVSS [EDI+0x40],XMM0`,
`0x40df2d CMP [EDI+8],0 / JNZ 0x40dea0`, `0x40df57 CMP byte [EAX+ECX*8+4],DL`,
`0x40df67 MOV ESI,[EAX+8]`, `0x40e022 CMP ECX,[EDI+0x18]`, `0x40e031 RET 0x8`.
```
EDI = cross; delay = [EBP+8]; all = [EBP+0xc]
cross->k40 (+0x40) = *(float*)(DAT_005e652c + 0x84 /* sim_time */) + delay   // 0x40de73..0x40de90

// 1) own car list: cross+0x8 head, next = car+0x8. Always runs, `all` not consulted.
while (cross->cars (+0x8) != 0) {
    car = cross->cars (+0x8);
    if (car->notify (+0xdd) != 0 && car->live (+0xdc) != 0) {   // 0x40dea3/0x40deac
        --DAT_005e6514;                                        // live_count, 0x40deb5
        if (car->type_id (+0x14c) == 0 && car->inst_id (+0x138) != 0 &&
            car->notify (+0xdd) != 0) {
            logf(buf, "car_rem %d %d", car->inst_id, car);     // 0x528030
            Game__queueEvent(T + 0x3130, 0, 0x10, buf, 0);     // 0x47d550
        }
        car->live (+0xdc) = 0;                                 // 0x40df10
        if (car->k128 (+0x128) != 0) car->k128 = 0;            // 0x40df19
    }
    recycle_halted_car(cross /*ECX*/, car);                    // 0x40fae0, ALWAYS
}
// 2) link lanes. links = cross+0x14, count = cross+0x18, entry 8 bytes {Lane*, u8 flag(+4)}
for (i = 0; i < cross->link_count (+0x18); i++) {
    if (all == 0 && cross->links[i].flag (+4) == 0) continue;
    lane = cross->links[i].node;
    for (car = lane->cars (+0x8); car != 0; car = car->next (+0x8)) {
        if (all == 0 && car->next_path (+0xbc) == 0) continue;
        ... same notify/live block as 1) (identical offsets 0x40df86..0x40dffc) ...
        recycle_halted_car(cross /*ECX*/, car);                // 0x40fae0
    }
}
```
Touches: cross `+0x40` (write), `+0x8` (head read), `+0x14/+0x18` (links);
lane `+0x8` (head read); car `+0x8` (next), `+0xbc`, `+0xdc`, `+0xdd`, `+0x128`,
`+0x138`, `+0x14c`, `+0x24/0x20/0x18/0x14/0x2c/0x30/0xa8/0x154` (via 0x40fae0);
globals `0x5e6514` (live_count), `0x5e64f4`/`0x5e64fc` (via 0x40fae0), sink
`T+0x3130`. No lane +0xc4 write anywhere.

### 0x40fae0 recycle_halted_car (ECX = cross, one stack arg = car; `RET 0x4`)
Anchors: `0x40faed MOV EAX,[ESI+0x14c]; TEST; JZ 0x40fb63`, `0x40fbb1 CALL 0x416800`,
`0x40fcdf MOV ECX,ESI; CALL 0x40f290`.
```
if (car->type_id (+0x14c) != 0) {                       // scripted/materialized car
    Traffic__parkInstance(ECX = T, car->type_id, &car->link (+0x130));  // 0x417080
    unlink car's +0x130/+0x134/+0x13c node from its list (if +0x13c != 0);
    car->inst_id (+0x138) = 0;
    --DAT_005e64f4; ++DAT_005e64fc;
    TrafficCar__remove(ECX = car);                      // 0x40f290
    return;
}
// abstract car (type_id == 0): spawn a replacement near the halted cross's position
new = Traffic__spawnCarNear(ECX = T, cross->pos (+0x24/+0x28/+0x2c), 100.0f,
                            car->k14, car->k18, car->k20, car->k24);   // 0x416800
ResRefLink__set(ECX = &new->link (+0x130), car->node (+0x13c));        // 0x40cac0
unlink car's +0x130/+0x134/+0x13c node (same block);
car->inst_id (+0x138) = 0;
new->max_speed (+0x2c) = car->max_speed;  new->accel (+0x30) = car->accel;
new->lateral (+0xa8)   = car->lateral;    new->seed (+0x154) = car->seed;
new->notify (+0xdd)    = car->notify;
Game__getInfo(ECX = Game 0x5fde90, new->link (+0x130), 0x38, &new);     // 0x47c640
TrafficCar__syncInstance(ECX = new, (new->lane (+0xc4))->road (+0x10) + 0x68); // 0x410680
if (new->notify (+0xdd) && new->live (+0xdc))
    queue "car_add %d %d" to T+0x3130;                  // 0x528030 / 0x47d550
TrafficCar__remove(ECX = car);                          // 0x40f290
```
Only xrefs to 0x40fae0 are 0x40df28 and 0x40e009, both the two calls inside 0x40de70.

### 0x4de360 GroundRef.haltTrafficCross (names.json
`Native__util_resource_GroundRef__haltTrafficCross_4de360`)
Signature/registration: `java.util.resource.GroundRef.haltTrafficCross(Ljava.lang.Vector3;F)V`
— src row `src/script/natives.cpp:8548` already matches; body is `gr_halt_traffic_cross`.
Marshalling (`getArgs 0x4b1af0`, xrefs 0x4de376..0x4de389): out slots in order
`self`, `arg0` (Vector3 object), `arg1` (F raw dword) -> `[EBP-8]`, `[EBP-4]`, `[EBP-0xc]`.
Field names read on the Vector3 with `Instance__getFloatField 0x4ec980`: `"x" 0x5a37a0`,
`"y" 0x5a3788`, `"z" 0x5a377c` (read_bytes shows the C strings; missing -> 0).
```
handle = instanceToHandle(0x4ecc70)(self, DAT_007a2620)          // 0x4de37b..0x4de384
T = Game__getInfo(ECX = Game 0x5fde90, handle, 0x39, 0)          // 0x47c640
if (T == 0) return;                                             // RET, void
x = getFloatField(arg0, "x"); y = ..."y"; z = ..."z";
cross = Traffic__nearestCross(ECX = T, x, y, z, 0.0f, 0);        // 0x419050, 0x4de3dc..0x4de3f4
ctCross__halt(ECX = cross, delay = arg1, all = 1);               // 0x4de408, args pushed at
                                                                 // 0x4de3fe..0x4de403
RET                                                              // void, RET (no value)
```
`RET` at 0x4de411, no stack cleanup args, EAX not set/read. The exe does **not** check the
nearest-cross result: it halts through the returned pointer. The service src needs
(`getInfo(self, 0x39)` -> traffic graph) exists as `Runtime::ground_traffic(value_handle(a[0]))`;
the exe's return value for the missing-service case is void (no-op), not an error.

### 0x419980 (evidence only; ECX = cross, one arg; void `RET 0x4`)
Called only from `Traffic__haltPath 0x4196f0` at 0x4198a3/0x4198e3; both calls ignore EAX.
```
if (!cross) return;
cross_halt(cross, 0.001f /*0x3a83126f*/, 1);                    // 0x41999e
for (i = 0; i < cross->link_count (+0x18); i++) {
    lane = cross->links[i].node;                                // cross+0x14 + i*8
    lane->halted (+0xc4) = 1;                                   // 0x4199bf
    // lane+0x10 = parent road pointer; road nodes are at road+4/+8/+0xc,
    // so road+8+4*k18 = node[1 + k18]; k18 = lane+0x18 byte.
    entry = *(Road**)(lane+0x10) -> node[1 + *(u8*)(lane+0x18)];// 0x4199c6/0x4199c9
    for (j = 0; j < entry->link_count (+0x18); j++) {
        l = entry->links[j];                                    // +0x14, 8-byte {Lane*, u8 flag(+4)}
        if (l.flag == 0 || l.node->halted (+0xc4) != 0) continue;
        // keep the lane if any path out of link j reaches a non-halted lane
        for (p = 0; p < entry->path_count (+0x20); p++)
            if (paths[p].a (+0x1c + p*0x60) == j && entry->links[paths[p].b].node->halted == 0)
                goto next_j;
        if (list_len == list_cap) { list_cap += 0x400; list = realloc_0x5679b0(list, list_cap*4); }
        list[list_len++] = l.node;
    next_j:;
    }
}
```
`0x419ab0` (evidence only; arg = lane) is the same expansion starting from one lane:
`lane->halted = 1;` then the identical `lane+0x10` -> `node[1+k18]` -> append loop.

**Global lane worklist**: `DAT_005e6538` = `Lane**` data, `DAT_005e653c` = count,
`DAT_005e6540` = capacity in entries; grows by 0x400 entries via realloc `0x5679b0`, freed
by `0x567950`. Xrefs: only 0x4196f0/0x419980/0x419ab0 plus one data site 0x590f98. It is a
temporary worklist, not a persistent structure: `0x4196f0` calls `0x419980` for route
positions, then walks the list calling `0x419ab0` on each still-unhalted lane (the list may
grow during the walk), then frees it. Do **not** add an equivalent: src's
`Traffic__haltPath` is not ported (`gr_halt_traffic_path` is a stub) and `Traffic__findRoute
0x417e50` is UNRESOLVED.

### EXE offset -> src name
| exe | src | exe | src |
|---|---|---|---|
| cross+0x8 list head | `Cross::cars` | car+0xdc | `Car::live` |
| cross+0x14 links (8B) | `Cross::links` (`node`, `flag`) | car+0xdd | `Car::notify` |
| cross+0x18 link count | `Cross::links.size()` | car+0x128 | `Car::k128` |
| cross+0x40 | `Cross::k40` | car+0x138 | `Car::inst_id` |
| cross+0x24..0x2c | `Cross::pos` | car+0x14c | `Car::type_id` |
| lane+0x8 head | `Lane::cars` | car+0xbc | `Car::next_lane_b` |
| lane+0xc4 | `Lane::halted` (not touched here) | car+0x14..0x24 | `Car::k14/k18/k20/k24` |
| T+0x84 | `Graph::sim_time` | car+0x2c/0x30/0xa8/0x154 | `max_speed/accel/lateral/seed` |
| 0x5e6514 | `Graph::live_count` | 0x5e652c | the graph (src passes `Graph&`) |

## Changes
1. `src/game/traffic.h`, inside `struct Graph` next to `clear_halts()`:
   ```cpp
   // 0x40de70 ctCross__halt: k40 = sim_time + delay, then recycle the cross's cars
   // and (all / flagged link lanes) each link lane's cars.
   void halt_cross(Cross &c, float delay, bool all);
   ```
2. `src/game/traffic.cpp`, just above `Graph::remove_car` (0x40f290):
   ```cpp
   // 0x40fae0, the per-car helper of 0x40de70 (ECX = cross, arg = car): recycle a
   // car stopped by the halt. type_id == 0 spawns a replacement near the cross and
   // copies its fields; type_id != 0 parks the script instance. Both end in
   // TrafficCar__remove 0x40f290.
   // shortcut: +0x130/+0x134/+0x13c instance links, 0x417080 parkInstance,
   // 0x40cac0, 0x47c640 getInfo 0x38, 0x410680 syncInstance, car_add and the
   // 0x5e64f4/0x5e64fc counters are not modelled (src has no materialized cars).
   static void recycle_halted_car(Graph &g, const Cross &c, uint32_t ci)
   {
       Car &car = g.cars[ci];
       if (car.type_id == 0) {                          // +0x14c
           uint32_t nc = g.spawn_near(c.pos[0], c.pos[1], c.pos[2], 100.0f,
                                      car.k14, car.k18, car.k20, car.k24);  // 0x416800
           // INVENTED: the exe writes new+0x130 unchecked; a failed spawn only
           // removes the old car here.
           if (nc) {
               Car &n = g.cars[nc - 1];
               n.max_speed = car.max_speed;   // +0x2c
               n.accel = car.accel;           // +0x30
               n.lateral = car.lateral;       // +0xa8
               n.seed = car.seed;             // +0x154
               n.notify = car.notify;         // +0xdd
           }
       }
       car.inst_id = 0;                                 // +0x138, both branches
       g.remove_car(ci + 1);                            // 0x40f290
   }

   // 0x40de70 ctCross__halt (ECX = cross; args float delay, char all): the only
   // writer of Cross::k40. It writes no lane field: lane+0xc4 is set by 0x419980/
   // 0x419ab0 (haltPath) only.
   void Graph::halt_cross(Cross &c, float delay, bool all)
   {
       c.k40 = sim_time + delay;                        // 0x40de90
       std::vector<uint32_t> own = c.cars;              // INVENTED: snapshot (exe follows car+0x8)
       for (uint32_t ci : own) {
           Car &car = cars[ci];
           if (car.notify && car.live) {                // 0x40dea3/0x40deac
               live_count--;                            // 0x40deb5 (global 0x5e6514)
               // shortcut: T+0x3130 "car_rem %d %d" (0x528030/0x47d550) not modelled
               car.live = 0;                            // 0x40df10
               if (car.k128) car.k128 = 0;              // 0x40df19
           }
           recycle_halted_car(*this, c, ci);            // 0x40fae0, always
       }
       std::vector<CrossLink> links = c.links;          // INVENTED: snapshot of +0x14/+0x18
       for (size_t i = 0; i < links.size(); i++) {
           if (!all && links[i].flag == 0) continue;    // 0x40df50..0x40df5b
           Lane *lane = lane_at(*this, links[i].node);  // INVENTED: null guard
           if (!lane) continue;
           std::vector<uint32_t> list = lane->cars;     // INVENTED: snapshot (exe re-reads +0x8)
           for (uint32_t ci : list) {
               const Car &car = cars[ci];
               if (!all && car.next_lane_b == nullptr) continue;   // +0xbc
               Car &m = cars[ci];
               if (m.notify && m.live) {
                   live_count--;
                   // shortcut: car_rem sink not modelled
                   m.live = 0;
                   if (m.k128) m.k128 = 0;
               }
               recycle_halted_car(*this, c, ci);
           }
       }
   }
   ```
3. `src/script/natives.cpp`, replace the `gr_halt_traffic_cross` stub body at ~7527
   (keep the registry row at 8548 unchanged; `want(..., a, 3, err)` = self, Vector3, F):
   ```cpp
   // 0x4de360 GroundRef.haltTrafficCross(Vector3,F)V: self -> instanceToHandle 0x4ecc70,
   // Game__getInfo 0x47c640 id 0x39; 0 -> return (void). Reads Vector3 "x"/"y"/"z"
   // with Instance__getFloatField 0x4ec980 (missing -> 0), then
   // Traffic__nearestCross 0x419050(t, x, y, z, 0.0f, 0) and ctCross__halt 0x40de70
   // (delay, all = 1) at 0x4de408. Void; the exe does not check the cross.
   // INVENTED: null arg object / null cross => no halt (the exe dereferences both).
   bool gr_halt_traffic_cross(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!want("GroundRef.haltTrafficCross", a, 3, err)) return false;
       *out = Value::make_null();
       float pos[3] = {0.0f, 0.0f, 0.0f}, delay = 0.0f;
       if (a[1].kind == Value::Obj && a[1].obj) {
           pos[0] = dd_get_f(vm, a[1].obj, "x");
           pos[1] = dd_get_f(vm, a[1].obj, "y");
           pos[2] = dd_get_f(vm, a[1].obj, "z");
       }
       if (!get_f("GroundRef.haltTrafficCross", a[2], &delay, err)) return false;
       Runtime *rt = game_rt();
       traffic::Graph *t = rt ? rt->ground_traffic(value_handle(a[0])) : nullptr;
       if (!t) return true;                            // getInfo 0x39 == 0 -> no-op
       const traffic::Cross *c =
           t->nearest_cross(pos[0], pos[1], pos[2], 0.0f, false);
       if (!c) return true;                            // INVENTED: exe dereferences it
       t->halt_cross(const_cast<traffic::Cross &>(*c), delay, true);
       return true;
   }
   ```
   Delete the `SERVICE MISSING` lines above the stub; the old comment's `cross->halt(...)`
   is the exe's `0x40de70`, now reached as `Graph::halt_cross`.

## Files
- edit: src/game/traffic.h (`Graph::halt_cross` declaration)
- edit: src/game/traffic.cpp (`recycle_halted_car`, `Graph::halt_cross`)
- edit: src/script/natives.cpp (`gr_halt_traffic_cross` body + comment only)

## Done when
- `Graph::halt_cross` is commented `// 0x40de70 ctCross__halt` with `// 0x40de90` on the
  k40 store, `// 0x40fae0` on the helper call and `// 0x40f290` on the removal;
  `recycle_halted_car` carries `// 0x40fae0` and the `shortcut:` line for the instance
  services; the native comment cites `0x4de360`/`0x4de408`; every non-exe guard is
  `INVENTED:`.
- No lane `halted` write and no `0x419980`/`0x419ab0`/global-list code is added.
- `make -C src clean check` green; no new unit tests or hand-made expected values. If a city
  check changes only because a halted cross now has `k40 > sim_time` (routes blocked), stop
  and report it on the card; do not edit the check or its expected values.
- card frontmatter: status implemented, branch name.

## UNRESOLVED
- `DAT_007a2620` (second arg of `0x4ecc70` at 0x4de37b, `read_bytes 0x7a2620` = zeros) is
  not identified. Not needed: src already maps the self argument with `value_handle(a[0])`.
  Raw: `0x4de37b PUSH 0x007a2620; 0x4de381 PUSH [EBP-8]; 0x4de384 CALL 0x004ecc70`.
- `0x419980`/`0x419ab0` lane -> cross step: the exe literal is
  `0x4199c6 MOV EAX,[EAX+0x10]` / `0x4199c9 MOV ECX,[EAX+ECX*4+8]` with `ECX = lane+0x18`,
  i.e. road node `[1 + k18]` (road nodes at +4/+8/+0xc), while `0x40e460` uses
  `node[2 - k18]` as the exit cross. The entry/exit reading is inferred; a lane with
  `k18 >= 2` would make the exe read road+0x10 (a float) as a node pointer. Not ported, so
  it does not affect this card.
- `0x40fae0`'s instance branch (`0x417080` parking, +0x130/+0x134/+0x13c unlink,
  `0x5e64f4`/`0x5e64fc` counters) and its `car_add` sink are services src lacks
  (materialized script cars are not modelled). Kept as `shortcut:`; the exe return is
  void either way.
