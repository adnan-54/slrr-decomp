# 35 — Car part bodies, P nodes and the object box O+0xb4 (RE 2026-10-26)

Follow-up of notes/32 §4 ("driver 1 not carded: src has no wheelbone/a_arm/part bodies, any O box is
INVENTED"). Ghidra session, names from analysis/names.json re-applied. Decompiled: 0x4c1f90, 0x4992e0,
0x4c7d20, 0x524620, 0x4e8a50, 0x4f78d0, 0x4f7990, 0x4f7d60, 0x4ba8c0, 0x5056a0, 0x5001c0, 0x504180,
0x503c20; asm-checked 0x49a12c..0x49a355 (createCar wheel bones). New names: 0x4f78d0
`PhysicsWorld__addChildBody`, 0x4f7990 `PhysBody__compoundAttach` (both added to names.json).
Known names used: 0x4ba8c0 `Shape__ctor`, 0x5056a0 `RigidBody__setSphereInertia`, 0x4a99b0 `type_car__vf7`.

## 1. Who creates which body, and which bodies get a P node
`GameObject__createPhysInstance 0x524620(H, 0, body, name)` creates P (type 2, `_physinst` default
name, flag `+0x84 |= 0x200`) **under H's game instance** (`H+0xc` parent link), `typed(P)+0xb4 =
ref(body)`, and seeds its box: `P+0xb0 = 0`, centre `P+0xb4..+0xbc` = body active pos, half
`P+0xc0..+0xc8` = br (= `def+0x90 shape +0xc`, 0 when the body has no shape record). The caller then
links `body+0x48 -> P` (asm `lea ecx,[esi+0x48]; call 0x40cac0`), i.e. body+0x54 = P.

| creator | body | parent body | P name, under | br (shape+0xc) |
|---|---|---|---|---|
| 0x4c1f90 (every part, incl. the car) | `part_comp` = createBox(1.0, 6, 0,0,0, 0) via 0x4e8a50 | none (world root), slept | "part_comp", the part | **0** (type 6 without mesh: 0x4f7d60 case 6 only calls 0x5056a0, Shape ctor 0x4ba8c0 zeroes +0xc) |
| 0x4c1f90 bone loop, every M bone record of kind 1/2 | addChildBody(part_comp, rec shape, P.pos + R_P(rec.pos + rec.R*shape.com)) | part_comp | only record `M+0xff0` (first `body` line): "bbone"; record `M+0xff4`: "steer" (+ P+0x124 ref, flags 0x8020); others: **none** | mesh: `sqrt(max |v|^2) + 0.05` over the centroid-shifted verts (0x4f7d60 case 6 with mesh) |
| 0x4c1f90, if `M+0x1040` (click shape) | addChildBody(part_comp, click shape) | part_comp | "cbone" (P+0x34 == 0) or "click" | by shape kind |
| 0x4992e0 createCar | `car_root` = createBox(1.0, 5, 1,1,1) at part_comp's root active pose; part_comp unlinked and attached under it | world root | "car_root", the car | `sqrt(3) + 0.05` = 1.78205 |
| 0x4992e0 per wheel, template shape `rec[-0x1c]` != 0 | addChildBody(car_root, wheel shape, hub + spring offset) flags `|= 0x8020` | car_root | "wheelbone", the car; `W+0x00` link -> P | sphere `r + 0.05` |
| 0x4992e0 per wheel, `rec[-0x1a]` != 0 | two addChildBody(car_root, rec[-0x1a], hub rec[-0x2e..-0x2c] through part_comp's active pose), flags `|= 0x8020` | car_root | "hub" (0x59c0a8) -> `W+0x10`, "a_arm" -> `W+0x20` | by shape kind |
| 0x4f3f10 (Suspension__ctor 0x4915d0) sensor capsule | addChildBody(car_root, capsule) | car_root | **none** (only body+0x38 bound) | — |

createBox br formulas (0x4f7d60): sphere `r+0.05`; capsule `a+b+0.05`; box `sqrt(a²+b²+c²)+0.05`;
type 6/7 mesh `sqrt(max |v|²)+0.05`. src `Shape::bounding_radius()` (shape.cpp:12) matches all of
these; mesh-less type 6 = 0 there too (SHAPE_NONE / no mesh).

Compound links: `addChildBody 0x4f78d0` = new body, 0x501f30 init, head insert into the parent's
child list, `0x4f7990(child)`: parent `+0x58 |= 8`, child `+0x58 |= 4` with a 0x50-byte record at
`+0x1c`: `+4..+0xc` = `R_parent^T (child.pos - parent.pos)`, `+0x10` = relative rotation.

**Installed parts (0x4c7d20).** The part's own `part_comp` (with its bbone/steer children) is unlinked
and attached under the target body T found by `getInfo(0x2f, slot)`: `if (!(T+0x58 & 0x8000) &&
T+0x20 && T+0x1c) T = T+0x20` (one step up). The chassis/engine part_comp are compound children of
car_root, so every ordinary install lands **directly under car_root** (src `rebuild_compound`'s flat
"every installed part joins car_root" is right for that case); only targets flagged 0x8000
(wheelbone/hub/a_arm/steer) keep the part under the bone. Then `0x5032d0(T)` (recurses to car_root).
The installed part's P nodes stay **under the part's game instance**, never under the car.

## 2. Per step (0x5001c0, re-read)
Bodies are integrated only when `(+0x58 & 0x81) == 1` (awake + active). A compound child (`+0x58 & 4`)
is not integrated: its other state is set rigidly from the parent's other state,
`pos = parent.pos + R_parent * rec.off`, `rot = parent.rot * rec.rot`, velocity = parent point
velocity. Then children recurse; a body with `+0x58 & 0x10` (no traced car creator sets it) re-centres
on its children and writes its own br; then `if (body+0x50) PhysBody__updateBounds 0x504180`:
`P.kind = 1, P.centre = (a.pos + o.pos)/2, P.half = |a.pos - o.pos|/2 + br` per axis (a = step-start
state, o = integrated state); finally the compound root's `+0x38` object is queued (0x501da0).
Event phase: `0x504090(O)` recurses the type-1 children, then `0x503c20(O)` = AABB union of **O's
direct type-2 children** (`+0x7c == 2`) with `kind != 2` (no traced writer of kind 2). The union is
written only if at least one child exists; otherwise O keeps its previous box. createCar seeds
`O.kind = 0, O.centre = part_comp active pos, O.half = (10, 10, 10)` (0x49a5dd).

Consequence: the car's O box (the one `Collide__groundTree` 0x4fe6f0 walks with, notes/31 §3) is the
union of the swept boxes of **the car's own P nodes only**: car_root, the car's part_comp, the car's
bbone (first car-cfg `body` line), steer/cbone when the car cfg has them, and per wheel wheelbone +
hub + a_arm. Installed parts do not widen it (their P nodes are under their own instances).

## 3. What changes the child offsets after creation
`type_car__vf7` 0x4a99b0 rewrites the compound records of the wheel bones every call:
asm 0x4ab3eb `getPhysBody(W+0x20)->+0xb4->+0x1c +4..+0xc = local pos` (a_arm), 0x4abd52 same for
`W+0x10` (hub); `type_car__vf14` 0x4a4c95 rewrites the steer body (`P+0x124`) record. So wheel-bone
P boxes follow suspension travel / steering. Not ported (vf7 is large, not traced here). The error of
using the create-time offsets is the suspension travel (a few cm) inside a box whose car_root term
alone is ±1.78 m.

## 4. Do we need real child bodies in src?
**No, not for the O box.** Because 0x5001c0 places every compound child rigidly from its parent, each
P box is a pure function of (root start pose, root integrated pose, child offset in the root frame,
child br). src already has the root's start state (`StepState::start`, world.cpp:341) and the root's
integrated pose, and `rebuild_compound` already knows the flattened child offsets (`c.pos - com`).
A per-root list of "P records" {root-frame offset, br} reproduces 0x504180 + 0x503c20 exactly for
the rigid children. This is INVENTED **storage** (flattening, same as `collision_children`), not
invented behaviour. Real child bodies (World child integration, per-child states, part_comp under
car_root with its own bbone children, 0x4c7d20 relinking) would touch the integrator, the contact
pass and `rebuild_compound`: high risk for the green race numbers (659.414 s), no gain for the walk.

Remaining approximations, each a `shortcut:` with a known bound:
- wheel bones at create-time offsets (vf7 not ported, §3);
- hub/a_arm only when src knows `rec[-0x1a]` (src does not parse it today -> omitted; they sit at the
  hub, inside the wheelbone/car_root union in practice);
- steer/cbone records only when the car cfg has them (src does not track `M+0xff4`/`M+0x1040` for the
  car type -> omitted).

## 5. Plan
1. **Card 229 (safe, no behaviour change):** P records on the root + O box per event phase. Nothing
   reads the box yet, so race numbers cannot move; gate = `make -C src clean check` green with the
   race time unchanged.
2. **Not carded (needs a decision / more RE):** the keep-alive walk itself (notes/31 §3:
   for every resident map node overlapping O's box with `N+0x84 & 0x100`, call the road handler vf5
   with 1.0 -> card 219's `road vf1`). Blockers: (a) the runtime has no map-node -> bound road object
   link (card 217's data stays in resman), (b) the groundTree node overlap test and the
   IntervalMgr pair pass 0x4ee460/0x4ef475 must be ported in the same order, (c) live lanes then
   start stepping traffic (card 220) during the race run, which can move the race numbers. Do it as
   its own RE round after 229 lands.
3. Not planned: real child bodies (§4), type_car__vf7 bone offsets, 0x4c7d20's 0x8000-target branch.
