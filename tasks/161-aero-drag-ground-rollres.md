---
id: 161
status: todo
branch: card/161
depends: [160]
---
# Aero drag (Vehicle::update §7, 0x49fb34..0x49ff40) + ground material rolling resistance in p13 (gm+0x54)

## Why (RE 2026-10-24, notes/09 §7 and "Top speed 103.85 m/s")
Two terms of `Vehicle::update` 0x49c8e0 are not ported:
1. Aero drag on the car root at `drag_center`. `CarData::c_drag/drag_center` are filled by
   `ch_force_update` (natives.cpp ~3788, from `Chassis.updatevariables`: `C_drag = fully_stripped_drag -
   sum(drag_reduction)`, clamped at 0, Chassis.java:329/665/670) and never used. Inert for the vmboot
   SunStrip (its parts reduce C_drag to 0), but other shipped cars end > 0 (debug prints of other
   freeride cars: 0.021, 0.11, 0.17), and a SunStrip with drag-reducing parts removed would too.
   The exe drag is a plain coefficient: `F = -C_drag*|v|^2*v_hat` (no frontal area / air density).
2. Tyre Pacejka slot 13 (rolling resistance) is `gm+0x54 + W+0x74` in the exe; src uses `W+0x74` only
   (vehicle.cpp:472, stale comment "ground material has no rollres in Shape").

## Exe code (rea/Ghidra 12.1.4 on bin/the game/StreetLegal_Redline.exe, read 2026-10-24)
Constants read with read_bytes: `0x593570` = sign mask 0x80000000, `0x593580` = 0.0, `0x593590` = 0.5,
`0x593598` = 1.0, `0x5947a0` = 0.25. `ESI` = car, `e = car+0x2050` (engine/chassis block, `[EBP-0x28]`),
`dt = [EBP+0xc]`.

### (a) p13 store inside the tyre loop (asm 0x49eb07, 0x49ef5a..0x49ef7a)
```
gm = S+0xac                                   // 0x49eb07, S = wheel's c_triggerspring
W+0x1f0 = gm+0x50 * W+0x6c                    // p2  (src: w.ground_friction * w.friction_eff, ported)
W+0x21c = gm+0x54 + W+0x74                    // p13 (src: w.rolling_resistance only -> FIX)
```
`gm` comes from `c_triggerspring__onSensorContact` 0x4f3d70 (decompiled 2026-10-24):
```
if (c+0x60 != 0) { idx = *(c+0x60); S+0xa8 = idx; sh = other_body.def(+0x5c)+0x90 (shape);
                   S+0xac = ref(idx < sh+0xac ? sh+0xb0 + idx*0xc4 : sh) }   // per-face material, else the shape
else S+0xa8 = 0                                                         // S+0xac left as it was
```
For the ground body the shape is written by `type_ground__vf3` 0x4bade0: `+0x50 = Config.ground_friction`
(1.0 default), `+0x54 = 0.01` (imm 0x3c23d70a), `+0x58 = 0.2` (notes/19). src already holds this:
`ground->def.shape.friction` / `.rolling_resistance` (runtime.cpp:260-261). INVENTED (existing, keep):
gm = ground shape, the per-face material table `sh+0xb0` is not ported (count `sh+0xac` for the
groundbody UNRESOLVED; notes/19 "per-face materials").

### (b) Drag, asm 0x49fb34..0x49ff40 (after the per-wheel loop, before the aero-parts list car+0x2144)
```
car+0x2248 = 0                                // 0x49fb38, every tick
if (car+0x70 & 1) goto after_aero             // 0x49fb34 asleep flag -> skip all of §7 (0x49ff7e)
B = car+0x13ec (part_comp); while (B+0x58 & 4) B = B+0x20     // walk to the root = src car_root
if (!(e+0x91c > 0.0)) goto parts              // comiss 0x593580, JBE
v = pointVel(B, &e->drag_center /*e+0x910*/)  // 0x5062e0
if (!(|v|^2 > 1.0)) goto parts
s = |v|; vh = v * (1/s)
F = vh * -(s*s * C_drag)                      // xorps sign mask
d = rotate(B, drag_center)                    // 0x5061f0: R * p, no translation
c = d x vh
w = Iw^-1 * c                                 // rows B+0x108/0x118/0x128 (+ active state*0xd4) = world inv inertia
m_eff = 1 / (vh . (w x d) + def(B+0x5c)+0x2c /*invMass*/)
P = v * m_eff ;  h = P * 0.5 ;  q = F*dt + h
if (|q|^2 > |P|^2 * 0.25) {                   // |h|^2
    F = (q * (sqrt(|P|^2*0.25) / |q|) - h) * (1/dt)      // drag at most stops the point
}
car+0x2248 = |F|
PhysBody__applyForceAt 0x5051e0(B, F, &drag_center)
other_state(B)+0x3c (= body+0x174 - active*0xd4) &= ~1     // 0x49ff39: clear the other state's bit0
parts: (aero parts list car+0x2144 ... not this card)
```
### (c) `0x5062e0` pointVel(this = body, p_local, out) (root branch, `B+0x58 & 4 == 0`)
```
st = body + active*0xd4 ; if (!p) out = st+0x70 (lin vel)
rp = R(st+0xa4 rows) * p ; out = st+0x70 + omega(st+0xd4..0xdc) x rp
(child branch: transform p by the child record +0x1c and recurse; not reached here, B is the root)
```
### (d) `0x5061f0` rotate(this = body, p, out): `out = R(st+0xa4 rows) * p` (0 if p null).
### (e) `0x5051e0` applyForceAt(this = body, F_world, p_local)
```
st+0x8c..0x94 += F ; if (p) { rp = R*p ; st+0xec..0xf4 (torque) += rp x F }
```

## Field map exe -> src
| exe | src |
|---|---|
| `e+0x91c` C_drag, `e+0x910` drag_center | `game::CarData::c_drag`, `drag_center[3]` (car.h:225) -> copy to new `vehicle::Car::c_drag`, `drag_center` in the per-tick glue (car.cpp ~1384, next to `sim.max_steer`) |
| B (root of car+0x13ec) | `vehicle::Car::body` (= car_root, `CarData::root_body`) |
| 0x5062e0 / 0x5061f0 / 0x5051e0 | `RigidBody::point_velocity(p_local)`, `mat_vec(body.rotation(), p)`, `RigidBody::apply_force_at(F_world, p_local)` |
| `state+0x108..` world inv inertia, `def+0x2c` | `RigidBody::world_inv_inertia()` (world.cpp:143, R I^-1 R^T), `body.def.inv_mass` |
| other-state bit0 clear | `body.integrated = false` (physics.h:141 comment) |
| `car+0x2248` drag magnitude | new `vehicle::Car::drag_magnitude` (+ copy back to CarData if a native needs it; none known) |
| `car+0x70 & 1` | src skips asleep cars before `sim.update` (car.cpp ~1370), same gate |
| `gm+0x54`, `W+0x74`, `W+0x21c` | `ground->def.shape.rolling_resistance`, `Wheel::rolling_resistance`, `w.pacejka.p[13]` |

## Files (<= 3 + vmboot)
- `src/vehicle/vehicle.h` / `src/vehicle/vehicle.cpp`: `Wheel::ground_rolling_resistance` (reset with
  `ground_friction` at vehicle.cpp:137, set from `ground->def.shape.rolling_resistance` at :196); p13 at
  :472 = `w.ground_rolling_resistance + w.rolling_resistance` (drop the stale comment). New
  `Car::update_drag(dt)` = (b), called in `Car::update` after `apply_contact_forces()` (exe order:
  after the tyre loop). Record for vmboot: `drag_velocity` (v), `drag_force` (applied F), `drag_m_eff`,
  `drag_clamped`, `drag_magnitude` (car+0x2248; 0 when gated off).
- `src/game/car.cpp`: glue copies `c_drag`/`drag_center` into the sim per tick. Add (with its one-line
  declaration in car.h, next to `car_body_instances()`) `std::vector<CarData *> car_payloads()` over
  `g_cars` for vmboot.
- vmboot (`src/tools/vmboot.cpp`): checks below.

## vmboot checks (exe formulas + shipped data only, no hand-made numbers)
Keep every existing check (the 900-frame auto drive bounds may move a little because p13 grows by 0.01).
1. Rolling resistance: on every loaded wheel during the auto drive, `pacejka.p[13] ==
   ground->def.shape.rolling_resistance + w.rolling_resistance` (bit-exact). Print both terms once
   (expected 0.01 from 0x4bade0 + the car's W+0x74).
2. Inert for the SunStrip: print `car_payload->c_drag`; if it is 0, hard check `drag_magnitude == 0` on
   every frame of the 900-frame drive (gate `e+0x91c > 0`).
3. Shipped C_drag corpus: print `c_drag` of every `car_payloads()` entry whose Chassis ran forceUpdate;
   hard check at least one is > 0 (values come from Chassis.updatevariables over the real installed parts).
   Pick X = the car with the largest c_drag.
4. Drag exercised: after check 2, continue the auto drive 300 more frames with `car_payload->c_drag` and
   `drag_center` set to X's values (INVENTED harness: forced value taken from shipped data; the exe would
   reach it by removing parts). Per frame, on the recorded last sub-step values: if `|v|^2 > 1`:
   `drag_magnitude > 0`, `drag_magnitude == |drag_force|`, `drag_force . v < 0`; if `!drag_clamped`:
   `|drag_force - (-C*|v|^2*v_hat)| <= 1e-4 * |drag_force|`; if clamped: `| |drag_force*dt + h| - |h| |
   <= 1e-4*|h|` with `h = drag_m_eff * v * 0.5`. Hard check >= 250 frames had `|v|^2 > 1` (the car is
   moving). Print C, |v|, |F| at the last frame, the clamp count, and the final |vel| (still <= v_top).

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-4 pass, 0 failed threads. Result lists the printed values.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). If check 3 finds no
car with C_drag > 0, stop and report (do not invent a value). No unit tests or synthetic inputs; cite exe
addresses; no tuning constants; `INVENTED:` for the harness in check 4 and the gm = ground shape choice.

## Not this card
Aero parts list `car+0x2144` (`AeroForce__apply` 0x4e7b10), flaps 0x4a8430, the lift/push/roll helpers
(`car+0x1f64..0x1fa4`), wind `M+0x135c`, per-face ground materials (`sh+0xb0`, 0x4f3d70).

## Result
