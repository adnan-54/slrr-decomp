# Vehicle simulation (game-specific layer) — first map

## Script-field accessors (used everywhere in the vehicle code; `Instance` = script object)
| exe | meaning |
|---|---|
| `FUN_004ece80(inst, "name")` | read int field by name |
| `FUN_004ec980(inst, "name")` | read float field by name |
| `FUN_004ecae0(inst, "name", "class")` | read object/array field by name |
| `FUN_004ecc70(inst, class)` | with a class id: instance → native handle; with `DAT_007a261c` (array class) returns array length |
| `FUN_004ec960(arr, i)` | float array element |
| `FUN_004ed080(inst, "name", f)` / `FUN_004ed320(inst, class, obj)` | write float / object |
| `FUN_0047dd50(obj, "updatevariables", a, b)` | call a script method by name on a game object |
| `FUN_0051f4f0(0xa0000000/0x80000000, ...)` + vfunc `+0xc` | get typed sub-resource of a game object (0xa0000000 = physics/vehicle data, 0x80000000 = game type) |

So the **vehicle's tunable parameters live in script fields** (the car's `.java`/`.rdb` data), copied into native structs by `FUN_004aebf0`.

## `FUN_004aebf0(car)` — "refresh car parameters" (ECX = vehicle manager, `param_1` = car)
Runs when the car's script does not set `suspend_update`; calls the script `updatevariables`, then copies into the car's **engine block** (`car+0x2050`, pointer; defaults from `FUN_004b0910`):
| script field | native (float index in engine block `e[]`) |
|---|---|
| `engine_inertia` | `e[0x39]` |
| (constant 1.0) | `e[0x35]`, `e[0x36]` (scaled onto `car+0x1e04/0x1e08` by `1 - w²·0.6`, `w = car+0x1e9c`, min 0.1 when w ≥ 1) |
| `starter_torque` | `e[0x34]` |
| `engine_friction_fwd/rev` | `e[0x3d]`, `e[0x3e]` |
| `engine_rpm_idle` | `e[0x3c] = rpm/60·2π` (rad/s) |
| `engine_mass` | `car+0x1ebc = mass·8000`; `car+0x1ec8 = old·old/new` |
| torque curve | built from the engine definition `+0x1c0..0x1e0` (count `+0x1d4`, step `+0x1d0`): `e[1]`/`e[3]` = arrays of `(rpm = i·step, torque)` pairs, 8 bytes each |
| `ratio[]` (≤ 8) | `e[9..16]` gear ratios; `gears` → `e[8]`; `rearend_ratio` → `e[0x11]` |
| `maxRPM`, `RPM_limit`, `ClutchF` | `e[6]`, `e[7]`, `e[0x33]` |
| `rpm_trans_fwd/rev`, `sfx_starter_rpm` | `e[0x202..0x204]` |
| `SFX_trans_fwd/rev`, `SFX_ignition` | sound resources (`FUN_0051fa40(res, 6, 0)` = request-load) |
| `tank_nitro`, `consumption_nitro` | `e[0x293]`, `e[0x296]`; `car+0x1f5c` (nitro level) clamped to tank |
| `drag_center` (Vector3), `C_drag` | `e[0x244..0x246]`, `e[0x247]` |
Also: per-wheel records are `0x2e4` bytes at `[car+0x1414]`, count at `[manager+0x1fb4]`; per-wheel force accumulators (`+0x15c..0x170`) are zeroed each refresh and a 0x44-byte block is copied from `manager+0x2ec4`; wheel parts are looked up with `FUN_004c6fd0(car, 0x65 + i)`.

## Where the rest is
- `FUN_004ac310` (contains the `getRPM` string, 2.4 KB) and `FUN_0047f980` (prints `Gear: %d T: %d B: %d Axis: %d`, 2.6 KB) — gearbox/driver input debug and processing.
- `wheel_brake_factor`, `wheel_gndfeedback_factor`, `engine_inertia_factor`: config reads in the config loader `FUN_0047c1xx` (tuning multipliers for brakes, ground feedback, engine inertia).
- ~100 callers of `FUN_004eabc0` in 0x488000–0x4d5000 are the per-feature vehicle subsystems (wheels, suspension, tyres, drivetrain). Next: decompile them by feature.
