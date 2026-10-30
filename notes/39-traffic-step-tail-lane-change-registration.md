# 39 — TrafficCar__step tail: step 7 (lane-change completion) and step 8 (target-lane obstacle registration)

Follow-up of notes/37 §4. Decompiled + asm-checked: `TrafficCar__step` 0x4107c0 from
**0x4115f1 to the return at 0x4118e4** (ECX/EDI = car), plus every callee named in the
tail: 0x414050, 0x4133f0, 0x413660, 0x40ca40, 0x40d1e0, 0x40e710, 0x40d3c0 (release
cleanup). Cards 237/238 already cover step 3 and the lane-change decision; this note is
the evidence for **card 239** (steps 7/8).

Pre-register facts used below (unchanged from notes/37):
- ref triples `{owner,path,dir}`: next `+0xb8/+0xbc/+0xc0`, cur `+0xc4/+0xc8/+0xcc`,
  prev `+0xd0/+0xd4/+0xd8`; `+0x128` lane-change sign, `+0x124` lane-change target lane.
- Lane object: **car list** head `+0x08`, tail `+0x0c`, nodes embedded in the car at
  `car+0x04` (prev) / `car+0x08` (next) / `car+0x0c` (lane) — `0x40d1e0`/`0x40ca40`
  (notes/29 §2). **Obstacle list** head `+0x04` (a *second*, separate intrusive list).
- Constants read from the exe: `0x59371c = 1.75f`, `0x5935c0 = 0x7fffffff` (abs mask),
  `0xbf800000 = -1.0f`, `0x593744 = -0.5f`. `car+0x28 k28` default 1.2 (ctor 0x40c400).

## 0. Obstacle-list layout (the "intrusive helper" question)

`procedure_callees 0x4107c0` returns 0x40c270, 0x40ca40, 0x40d1e0, 0x40d2c0,
0x40daa0, 0x40dc00, 0x40eff0, 0x40fde0, 0x412ed0, 0x412f60, 0x4133f0, 0x413660,
0x414050, 0x571110, 0x571143. **There is no separate obstacle-list insert/remove
helper:** both are inlined in 0x4107c0. The same remove block is inlined a second time
in the car destructor/clear **0x40d3c0** (0x40d479..0x40d4c4; called from
`TrafficCar__release` 0x40c730 at 0x40c74b and from Traffic__parkInstance 0x41757b/
0x41777c/0x417877).

The obstacle node is embedded in the car at **car+0xec**; the list head is
**owner_lane+0x04**. Node and record (offsets relative to the node = car+0xec):

| node off | car off | meaning |
|---|---|---|
| +0x00 | +0xec | prev node (0 = head) |
| +0x04 | +0xf0 | next node |
| +0x08 | +0xf4 | unused by this code (zeroed by ctor 0x40c400, `in_ECX[0x3d]`) |
| +0x0c | +0xf8 | owner lane (`ob_owner`) |
| +0x10 | +0xfc | distance on the owner lane's path; **-1.0 = not on this path** |
| +0x14 | +0x100 | speed captured at registration (step 3 on-path branch) |
| +0x18 | +0x104 | on-path flag byte (step 3 calls 0x40eff0 only if != 0) |
| +0x1c/+0x20/+0x24 | +0x108/+0x10c/+0x110 | free-body position x/z/y (step 3) |
| +0x28/+0x2c/+0x30 | +0x114/+0x118/+0x11c | free-body velocity (step 3) |
| +0x34 | +0x120 | radius (step 3 / 0x40eff0) |

Insert is **head insert into owner+4** (LIFO); unlink is O(1) through prev/next and
updates `owner+4` when prev == 0.

```
insert(node, lane):                       // 0x411838..0x411859
  if (lane->head) lane->head->prev = node;
  node->prev = 0; node->next = lane->head;
  lane->head = node; node->owner = lane;

remove(node):                             // 0x411801..0x411832, 0x411868..0x4118b0, 0x40d479..0x40d4c4
  if (node->owner) {
      if (node->prev) node->prev->next = node->next;
      else            node->owner->head = node->next;
      if (node->next) node->next->prev = node->prev;
      node->owner = 0;
  }
  node->next = 0; node->prev = 0;
```

### Field writers (finding; matters to card 237)
Searched the whole `.text` for absolute-displacement writes to `+0x104` (88/C6/89/C7
forms) and to `+0x108/+0x10c/+0x110/+0x114/+0x118/+0x11c` (MOVSS/MOVSD/MOVUPS forms):
- `+0x104` is written **only** by the ctor 0x40c400 (byte := 0, 0x40c545).
  => the step-3 `if (entry->flag != 0) TrafficCar__checkObstacle` branch
  (0x410c63..0x410c6c) is **dead for traffic cars**, so the T+0xc8 event-1 list
  (0x40eff0) is never filled by them. Card 237's `car_check_obstacle` must stay for
  fidelity but is unreachable on this data.
- `+0x108..+0x11c` have **no writer at all** in the traffic code (`0x40c000-0x420000`);
  the only absolute-displacement hits in that range belong to other objects (the spline
  builder 0x4124d0 at +0x108, the Traffic-object reset 0x41b4d0, PaintDecal 0x420100),
  never to a traffic car.
  => step 3's free-body branch reads zeros for traffic cars. It is only reachable via
  `+0xfc == -1`, and step 8 unlinks a car whose projection failed, so registered
  entries always carry a real distance. Keep the branch (fidelity), do not invent a
  writer; `INVENTED`/`shortcut:` note in card 237 stands.

## 1. Step 7 — lane-change completion (0x4115f1..0x4116f4, asm-verified)

Entry: `XORPS XMM1,XMM1` at 0x4115ee zeroes the "z" register; if the current path
pointer is null both x and z stay 0.

```
if (car.k128 == 0) goto step8;                       // 0x4115f1/0x4115f8
x = z = 0.0f;
if (car.path (+0xc8)) {                              // 0x411617
    p = car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral);
    // dir >= 0 -> 0x413660 (cross Segment), dir < 0 -> 0x4133f0; only x=p[0], z=p[2]
    x = p[0]; z = p[2];
}
target = car.k124 (+0x124);                          // a LANE pointer in the exe
ok = 0x414050(target + 0x3c /*target->path*/, x, z, &d /*EBP-0x38*/, &lat /*EBP-0x18*/);
if (!ok) { car.k128 = 0; goto step8; }               // 0x411683 -> 0x4116f4
if (1.75f <= fabsf(lat)) goto step8;                 // 0x41168a..0x41169f, 0x59371c
                                                     // (k128 stays != 0, retry next step)
old = car.path_lane (+0xc4);                         // 0x4116a9
car.path_lane = target;                              // 0x4116b4  (+0xc4 = lane ptr)
car.pos       = d;                                   // 0x4116bd
car.lateral   = lat;                                 // 0x4116c5  (signed)
car.path      = target + 0x3c;                       // 0x4116cd  (target->path)
car.k124      = old;                                 // 0x4116d3  (swap cur <-> target)
lane_unlink(car);                                    // 0x4116d9 -> 0x40ca40, ECX=car
lane_insert(target, car);                            // 0x4116e5 -> 0x40d1e0 (sets car+0xc=lane,
                                                     //  calls lane vf3 0x40cbd0)
target_old_vf4(car, -1);                             // 0x4116f1 -> 0x40e710, ECX=old lane
car.k128 = 0;                                        // 0x4116f4
```

Notes:
- `car+0xcc` (dir) is **not** written (a lane ref is always dir = -1).
- `unlinkLane` uses `car+0x0c` (lane car-list owner), not `+0xc4`.
- lane vf3 0x40cbd0 can overwrite `car+0x124` when `lane+0x34 == -1`
  (`k124 = lane+0x90 ?: lane+0x94`), and lane vf4 0x40e710 clears `car+0x124 = 0`
  when the *old* lane has `+0x34 == -1`. So the call order insert-then-vf4 matters:
  after step 7 `+0x124` may be 0, the old lane, or the new lane's side-lane id.
- The |lat| gate is strict: complete iff `fabsf(lat) < 1.75f`.

## 2. Step 8 — target-lane obstacle registration (0x4116fe..0x4118e4, asm-verified)

Runs every step, independent of step 7 (k128 is only the lateral-gate bypass).
`was = (car.ob_owner (+0xf8) != 0)` is captured first (0x4116fe..0x41170f).

```
was = car.ob_owner != 0;                             // SETNZ, EBP-0x11
if (car.k124 == 0) goto unlink_if_was;               // 0x411708/0x411712 -> 0x411864
x = z = 0.0f;
if (car.path) {                                      // 0x411734
    p = car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral);
    x = p[0]; z = p[2];
}
target = car.k124;
ok = 0x414050(target + 0x3c, x, z, &car.kfc /*+0xfc*/, &lat /*EBP-0x18*/); // 0x41179c
if (!ok) {                                           // 0x4117a3 -> 0x41185b
    car.kfc = -1.0f;
    goto unlink_if_was;
}
car.k100 (+0x100) = car.speed (+0xb0);               // 0x4117b0/0x4117b6, raw 32-bit copy
if (car.k128 != 0 || fabsf(lat) < car.k28 + 1.75f) { // 0x4117a9/0x4117be..0x4117da, ADDSS
    if (car.ob_owner != target) {                    // 0x4117e0/0x4117e6 (pointer/int eq)
        car.k120 (+0x120) = car.k20 (+0x20);         // 0x4117f2/0x4117fb, raw copy
        if (car.ob_owner) obstacle_remove(car);      // 0x411801..0x411832
        obstacle_insert(car, target);                // 0x411838..0x411859, head insert
    }
    goto done;                                       // 0x411859 -> 0x4118ba
}
unlink_if_was:                                       // 0x411861/0x411864
if (was) obstacle_remove(car);                       // 0x411868..0x4118b0
done:                                                // 0x4118ba
if (car.k124 != 0 && car.k128 == 0 && car.ob_owner == 0)
    car.k124 = 0;                                    // 0x4118d6
```

Notes:
- `car.kfc` is written *directly* by 0x414050 (arg `&car+0xfc`), and only set to
  -1.0 on failure; the callee writes it only on a hit.
- `k100` is written on every successful projection (also when the lateral gate then
  rejects), `k120` only inside the register branch.
- The register gate is strict: register iff `k128 != 0 || fabsf(lat) < k28 + 1.75f`
  (k28 default 1.2 => 2.95).
- Nothing else is written: no `+0x104/+0x108/+0x114` (see §0), no crossing of the
  `+0x10` event list, no leader slots.
- The final clear needs all three: target set, no change in progress, and not
  registered. When the car ends registered on the target, `+0x124` persists.

## 3. Consequences for the src port (see tasks/239)
- `car.k124` is a **node id** in src (lane_attach 0x40cbd0 and card 238 store
  `node88[2]/[3]`), while the exe stores a lane pointer. Step 7 must store the *old*
  current lane back into k124 => needs a lane-index -> node-id helper (the inverse of
  `Graph::lane_by_node`: `1 + index + cross_eq + cross_light + roads`).
- `0x40d1e0` writes `car+0x0c = lane` in both insert branches; src `lane_insert` does
  not (callers set `car.lane`). Step 7 must set `car.lane = target_index` (or
  `lane_insert` should, behaviour-equivalent to the exe).
- Order after the swap: `lane_unlink`, then insert/lane_attach (may set k124), then
  `lane_detach` on the old lane (may clear k124), then `k128 = 0`.
- `car_release` 0x40c730 must remove the obstacle node (0x40d3c0 third block); src's
  `car_release` currently only clears the serial, so a removed car would leave a
  dangling entry in a lane's obstacle vector.
- Dead code in today's vmboot: no lane goes live (notes/37 §0), so steps 7/8 never
  run there and no race/city number moves.

## 4. Card 239
`tasks/239-traffic-car-step-tail-registration.md` (depends on card 237 for the
obstacle node/helpers/fields).
