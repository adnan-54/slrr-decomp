---
id: 239
status: merged
branch: card/239
review: deepseek-pass
depends: [237]
---
# traffic: TrafficCar__step tail — step 7 lane-change completion + step 8 target-lane obstacle registration

## Goal
Port the tail slice of `TrafficCar__step` 0x4107c0 (ECX/EDI = car) into card 235/236's
`car_step(Graph &, uint32_t)` — the code from **0x4115f1 to the return 0x4118e4**:
1. **step 7** (`0x4115f1..0x4116f4`, gated by `car.k128 != 0`): project the car's world
   x/z onto the lane-change target's path with `ctPath__projectStrict` 0x414050; if
   `|lat| < 1.75` swap cur lane <-> `car.k124`, replace pos/lateral/path, unlink from
   the old lane's car list, insert into the new one and detach from the old one
   (lane vf4 0x40e710); clear `k128`. Projection failure also clears `k128`,
   `|lat| >= 1.75` keeps the change running.
2. **step 8** (`0x4116fe..0x4118e4`, every step): project on `car.k124`'s path into
   `car.kfc` (+0xfc); on success copy speed into `car.k100` (+0x100); when
   `k128 != 0 || |lat| < car.k28 + 1.75` register the car in the **target lane's
   obstacle list** (`lane+4`, node `car+0xec`, radius copy `car.k120 = car.k20`);
   otherwise (and on failure / no target) unlink it if it was registered; finally
   clear `k124` when target set, no change in progress and not registered.

Card 237 owns the obstacle node layout, the insert/remove helpers
(`obstacle_link` / `obstacle_unlink`), the `Car` fields (`ob_prev/ob_next/ob_owner`,
`kfc`, `k100`, `k104`, `k108`, `k114`, `k120`) and `Lane::obstacles` + `Graph::event1`.
Reuse them; do not redefine. Step 3/4 and the lane-change decision (cards 237/238) stay
untouched.

## Exe evidence (cleaned pseudocode, renamed fields)
Field map (all cited in src unless marked): `+0x14 k14`, `+0x20 k20`, `+0x28 k28`,
`+0x30 accel`, `+0x88 pos`, `+0xa8 lateral`, `+0xb0 speed`, `+0xc4 path_lane`,
`+0xc8 path`, `+0xcc dir`, `+0xc8/+0xcc` are the cur path ref, `+0x124 k124 target lane`,
`+0x128 k128`; NEW from 237: `+0xec/+0xf0/+0xf8 ob_prev/ob_next/ob_owner`,
`+0xfc kfc`, `+0x100 k100`, `+0x120 k120`. Lane: `+0x34 k34` (== -1 gate),
`+0x04` obstacle list head, `+0x3c` path, `+0x90/+0x94` side-lane node ids
(`node88[2]/[3]`), `+0x08/+0x0c` car list head/tail.
`/` = lane vtable: vf3 0x40cbd0 (src `lane_attach`), vf4 0x40e710 (src `lane_detach`).

Helpers already in src (reuse):
`car_ref_pos_offset(path, dir, d, off, out)` = 0x40dde0 (dispatches 0x4133f0 lane /
0x413660 cross, cross branch a `shortcut:`), `path_project_strict` = 0x414050,
`lane_unlink` = 0x40ca40, `lane_insert` = 0x40d1e0, `lane_detach` = 0x40e710,
`lane_at` (anonymous namespace, `traffic.cpp` ~686) = node id -> `Lane*`,
`Graph::lane_by_node`.

### 0x4107c0 step 7 (0x4115f1..0x4116f4, asm-verified)
```
if (car.k128 == 0) goto step8;                        // 0x4115f1/0x4115f8
float x = 0.0f, z = 0.0f;                             // 0x4115ee XORPS + 0x411604 MOVAPS
if (car.path) {                                       // +0xc8, 0x411617
    float p[3];
    car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);   // 0x4133f0/0x413660
    x = p[0]; z = p[2];                               // y is ignored
}
Lane *tl = lane_at(g, car.k124);                      // exe pointer, INVENTED bounds
float d = 0.0f, lat = 0.0f;
bool ok = tl && path_project_strict(tl->path, x, z, &d, &lat);        // 0x414050
if (!ok) { car.k128 = 0; goto step8; }                // 0x411683 -> 0x4116f4
if (!(fabsf(lat) < 1.75f)) goto step8;                // 0x41168a..0x41169f, 0x59371c
                                                      // strict <; keep k128 for next step
uint32_t old = car.path_lane;                         // 0x4116a9
uint32_t ti  = (uint32_t)(tl - g.lanes.data());
car.path_lane = ti;                                   // 0x4116b4
car.pos       = d;                                    // 0x4116bd
car.lateral   = lat;                                  // 0x4116c5  (signed)
car.path      = &tl->path;                            // 0x4116cd
car.k124      = lane_node_id(g, old);                 // 0x4116d3  swap cur <-> target
lane_unlink(g, ci);                                   // 0x4116d9 -> 0x40ca40 (car+0xc)
car.lane      = ti;                                   // 0x40d1e0 writes car+0xc itself
lane_insert(g, ti, ci);                               // 0x4116e5 -> 0x40d1e0 (vf3 may set k124)
lane_detach(g, old, ci);                              // 0x4116f1 -> vf4 (may clear k124)
car.k128 = 0;                                         // 0x4116f4
```
- `car.dir` (+0xcc) is not written (lane refs are dir -1).
- `lane_node_id` = inverse of `Graph::lane_by_node`: `1 + li + cross_eq.size() +
  cross_light.size() + roads.size()`; NEW, `INVENTED` (the exe stores pointers).
- `lane_unlink` must run while `car.lane` is still the OLD lane; `lane_insert` must see
  the NEW lane in `car.lane` (the exe's 0x40d1e0 sets `car+0xc`).
- Insert vs vf4 order is load-bearing: vf3 0x40cbd0 sets `car.k124 =
  lane.node88[2] ?: node88[3]` when `lane.k34 == 0xffffffff`, and vf4 0x40e710 sets
  `car.k124 = 0` when the OLD lane's `k34 == 0xffffffff`. src `lane_attach` /
  `lane_detach` already implement both; keep this order.
- If `k124` names no lane: treat as projection failure (`k128 = 0`), `INVENTED: bounds
  (the exe dereferences the lane pointer)`.

### 0x4107c0 step 8 (0x4116fe..0x4118e4, asm-verified)
Runs every step, independent of step 7.
```
bool was = car.ob_owner != kNoneLane;                  // 0x4116fe..0x41170f SETNZ, EBP-0x11
if (car.k124 != 0) {                                   // 0x411708
    float x = 0.0f, z = 0.0f;
    if (car.path) {                                    // 0x411734
        float p[3];
        car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);
        x = p[0]; z = p[2];
    }
    Lane *tl = lane_at(g, car.k124);
    float lat = 0.0f;
    bool ok = tl && path_project_strict(tl->path, x, z, &car.kfc, &lat);  // 0x41179c
    if (!ok) {                                         // 0x4117a3 -> 0x41185b
        car.kfc = -1.0f;
    } else {
        car.k100 = car.speed;                          // 0x4117b0/0x4117b6 raw copy
        if (car.k128 != 0 || fabsf(lat) < car.k28 + 1.75f) {   // 0x4117a9/0x4117be..d
            uint32_t ti = (uint32_t)(tl - g.lanes.data());
            if (car.ob_owner != ti) {                  // 0x4117e0/0x4117e6
                car.k120 = car.k20;                    // 0x4117f2/0x4117fb raw copy
                if (car.ob_owner != kNoneLane)
                    obstacle_unlink(g, ci);            // 0x411801..0x411832 (0x40d3c0 twin)
                obstacle_link(g, ti, ci);              // 0x411838..0x411859 head insert
            }
            goto done;                                 // 0x411859 -> 0x4118ba
        }
    }
}
if (was) obstacle_unlink(g, ci);                       // 0x411861/0x411864..0x4118b0
done:
if (car.k124 != 0 && car.k128 == 0 && car.ob_owner == kNoneLane)
    car.k124 = 0;                                      // 0x4118d6
```
- `car.kfc` is 0x414050's `d` output written in place; only failure writes it here
  (-1.0). `k100` is written on every successful projection, `k120` only in the
  register branch. No `+0x104/+0x108/+0x114` writes (see notes/39 §0: those writers do
  not exist in the traffic code; step 3's flag/free-body branches are dead on this
  data — keep card 237's `shortcut:`).
- Gates are strict: complete iff `|lat| < 1.75`; register iff `k128 != 0 ||
  |lat| < k28 + 1.75`. The `k128 != 0` test bypasses the lateral gate entirely.
- `ob_owner` is compared as a lane index against the target lane; it is `kNoneLane`
  when unregistered (card 237). `lane_at`/`lane_by_node` resolve the `k124` node id;
  a bad id makes `ok` false (`INVENTED: bounds`, same as step 7; the exe dereferences).

### 0x40c730 TrafficCar__release cleanup (small addition, exe 0x40d3c0 third block)
0x40c730 calls 0x40d3c0 (0x40c74b), whose third block unlinks the obstacle node
(0x40d479..0x40d4c4). `car_release` must therefore call `obstacle_unlink(g, i)` (and
leave the fields to `car_ctor`'s `Car()` reset). Without it a released car stays in a
lane's obstacle vector.

## src changes (single pass; keep the exe's float order and raw copies)
- edit `src/game/traffic.cpp`
  - add `uint32_t lane_node_id(const Graph &g, size_t li)` in the anonymous namespace
    (inverse of `lane_by_node`, `// INVENTED: the exe stores a lane pointer`;
    `// 0x4116d3`).
  - `car_step`: replace the final `// shortcut: steps 7/8 (...)` comment with the two
    blocks above, in this order, each starting with `// step 7: 0x4115f1..0x4116f4` /
    `// step 8: 0x4116fe..0x4118e4`.
  - `car_release` (0x40c730): `obstacle_unlink(g, i)` before `serial = 0`
    (`// 0x40c74b -> 0x40d3c0 obstacle-node unlink`).
- no other file changes: the `Car`/`Lane`/`Graph` fields, `obstacle_link`,
  `obstacle_unlink`, `kNoneLane` and `Lane::obstacles` come from card 237.

## Verification
- `make -C src clean check` green; no unit tests. The new code is dead in today's
  vmboot (`Graph::step` has no live lane, notes/37 §0), so race/city numbers must stay
  (race 659.414 s, `--city` 63) — report if they move.
- Every ported block carries its 0xADDR comment; `INVENTED` on `lane_node_id`/bounds,
  `shortcut:` inherited from card 237 for the dead flag/free-body writers.
- Done: frontmatter status implemented, branch name.
