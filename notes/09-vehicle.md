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
Also: per-wheel records are `0x2e4` bytes; `car+0x1414` holds the array pointer (`W_i = *(car+0x1414) + i*0x2e4`, verified in `Chassis.getWheel 0x4b4150`), count at `[manager+0x1fb4]`; `Vehicle__refreshParameters` zeroes the per-wheel wear/damage fields (`+0x15c/+0x160` tyre, `+0x164/+0x168` brake, `+0x16c/+0x170` suspension; `0x4aece0..0x4aed1d`) and re-copies the 17-float Pacejka block from `manager+0x2ec4` (0x44 bytes, `0x4aed2d`); wheel parts are looked up with `FUN_004c6fd0(car, 0x65 + i)`.

## Where the rest is
- Corrections: `FUN_004ac310` is **not** gearbox code — it is a native *registrar* (registers `Chassis`, `WheelRef`, `SfxTable`, `Vehicle`, `Part`, `DynoData` natives via `JVM::registerNative`; see `06-native-methods.md`). `FUN_0047f980` (prints `MaxSpeed/Speed/Time/Dist`, `FPS`, `Csuszas` = slip) is the **follow/chase camera + debug HUD** update, not the gearbox.
- `wheel_brake_factor`, `wheel_gndfeedback_factor`, `engine_inertia_factor`: config reads in the config loader `FUN_0047c1xx` (tuning multipliers for brakes, ground feedback, engine inertia).
- ~100 callers of `FUN_004eabc0` in 0x488000–0x4d5000 are the per-feature vehicle subsystems (wheels, suspension, tyres, drivetrain). Next: decompile them by feature.

## Wheel record (`WheelRef` instance → native wheel struct, 0x2e4 bytes; offsets from the setter natives 0x4b6xxx)
Script setters write straight into the struct (the script's `WheelRef` is converted with `FUN_004ecc70(inst, DAT_007a2620)`):
| offset | field (script setter) |
|---|---|
| `+0x30` | pointer to the contact-patch block: `+0x9c`,`+0xa0`,`+0xa4` ← `setCPatch(a,b,c)` (first two with sign bit cleared) |
| `+0x44` | spring `force` (`setForce`) |
| `+0x48` | damping bump (`setDamping(f)` also sets `+0x4c = f·0.7`; `setDamping(b, r)` sets both) |
| `+0x4c` | damping rebound |
| `+0x50` / `+0x54` / `+0x58` | suspension rest / **max** / **min** length (`setRestLen/MaxLen/MinLen`); create defaults `+0x38+0x3c` / `+0x38+0.1` / `−1` |
| `+0x5c` | `restLen + wheelRadius` (derived) ; `+0x60` = wheel radius |
| every length setter | re-runs `FUN_004f4280(force, damp_bump, rest+r, max+r, min+r, damp_rebound, r)` — `Suspension__configure` on the wheel's `c_triggerspring` object (`wheel+0x30`): `s+0x5c=k`, `s+0x60=bump`, `s+0x64=rebound` (0.7·bump if ≤ 0), `s+0x4c=rest`, `s+0x50=param4` (= `W+0x54` max length + r, the ray length; if ≤ 0 → rest), `s+0x54=param5` (= `W+0x58` min length + r, bump stop; if ≤ 0 → 0), `s+0x58=s+0x68=param7=r` (if r ≤ 0: `+0x68=rest`, `+0x58=0`). See "Suspension object" below |
| `+0x64`,`+0x68` | friction (`setFriction`; also `+0x1f0`); `+0x6c`,`+0x70` = friction · `(1 − w²·0.6)` (min factor 0.1 when `w ≥ 1`, `w = [+0x15c]` = wear/damage factor) |
| `+0x74` | rolling resistance (`setRollRes`) |
| `+0xf0` | bearing friction (`setBearing`) |
| `+0x1e8 .. +0x228` | **Pacejka coefficient array, 17 floats** (`setPacejka(i, v)`, `0 ≤ i ≤ 16`). Named setters alias slots: `setSliction` → [0], `setFriction` → [2], `setStiffness` → [4], `setMaxLoad` → [11] (`+0x214`), `setFrictn_x` → [12] (`+0x218`), `setLoadSmooth` → [14] (`+0x220`) |
So the tyre model is a **Pacejka "magic formula"** parameter set per wheel, with load-dependent grip limited by max load; suspension is a spring/damper constraint with bump and rebound damping (rebound = 0.7 × bump by default).

Verified setter targets (all decompiled): `setFriction` 0x4b6420 writes `W+0x64`, `W+0x68` **and slot 2** (`W+0x1f0`), then recomputes `W+0x6c/+0x70` with `f = 1 − wear²·0.6` (0.1 at wear ≥ 1); `setSliction` 0x4b6fd0 writes slot 0 (`W+0x1e8`); `setStiffness` 0x4b7050 slot 4 (`W+0x1f8`); `setMaxLoad` 0x4b6950 slot 11; `setFrictn_x` 0x4b6500 slot 12; `setLoadSmooth` 0x4b6870 slot 14; `setPacejka` 0x4b6a90 any slot `0..16`; `setRollRes` 0x4b6f90 `W+0x74`; `setBearing` 0x4b6080 `W+0xf0`; `setDrive` 0x4b6330 `W+0xcc` and `W+0xe0`.
Other setters still to map: `setArm(7f)`, `setHub(10f)`, `setInstantCenter(6f)` (suspension geometry), `setPos/Ypr/Steer/Brake/HBrake/Width/OppWheel` and the `Chassis` natives (`setMaxSteer`, `setAckermann`, `setSteerWheel*`, `setBuck`, `setCooling`, `setTorque`, `getRPM`...).

## `FUN_0049c8e0(car, dt)` = **Vehicle::update** (17 KB, 1717 lines of pseudo-code; first-pass summary — the complete, corrected walk-through is in "Vehicle::update — full order of operations" below)
Called per car per physics tick (`param_1` = car, `ECX` = vehicle manager). Early outs: `car+0x2054 != 0` (disabled), `dt < 0.0001`. Root body of the car = `car+0x13ec` (follow `+0x20` while flag `&4`). Engine block `e = car+0x2050`.
Observed in the first part (engine / thermal model):
- State flags `car+0x70` (bit0 init'd, bit1, bit10 `0x400` = skip thermal, bit14 `0x4000` = engine running). On first update after the body becomes active (`body+0x58 & 0x80`) it seeds per-wheel force vectors (`wheel+0x140..0x154`) from the wheel contact data (`wheel+0x30` → contact-patch block; `FUN_004f3d30` = suspension/contact query).
- **RPM**: engine angular speed `car+0x1e7c` (rad/s) → rpm `= ω · 9.549296` (`30/π`). Throttle `car+0x1de4`.
- **Torque** (corrected): `FUN_004a1510` is the engine *friction* torque, `FUN_004a2dd0` the torque curve, `FUN_004a2d40` the nitro torque curve (see below). `e[0x3c]` (`+0xf0`) is the **idle** speed (rad/s), not a rev limit; `car+0x1dfc` is the **starter-cranking** flag.
- **Nitro**: `car+0x1df8` (active strength) and tank `car+0x1f5c`; consumes `strength · e[0xa58/4] · rpm · 0.0002 · dt`, adds extra torque (`·2000` to the heat term).
- **Engine temperature** `car+0x1ec0` (°C): heat in `= (torque_work · dt) / engine_mass(car+0x1ebc)`; cooling coefficient `car+0x1ec8` (from `e[0xa5c]` + airflow term `e[0xa60]`, `e[0xa64]` · speed², with `FUN_004b0ff0(radiator health)`); `T -= |T − ambient(car+0x1ec4)| · coeff · dt · (T − ambient)`; above 100 °C the engine takes damage: `FUN_004afb50(damage = (T−100)² · dt · k)`; wear probability also depends on (rpm/maxRPM)^16 (`e[6]`; see the corrected formula below), friction `e[0xf4/0xf8]` and `FUN_004afb50` returns whether a part failed — on failure torque factors `car+0x1e04/08` are reduced by `FUN_004b0ff0(health)`; at full damage (`≥1.0`) torque halves, friction ×50 and an event is queued (`FUN_004eb5c0`).
- Output torque/inertia terms are accumulated into locals and applied to the drivetrain further down (to be read next: lines 340+ — differential/wheel loop `for i < manager+0x1fb4` with stride 0x2e4, gearbox, tyre forces).
Names (corrected): `0x4a1510` `Engine__frictionTorque`, `0x4a2dd0` `Engine__torqueCurve`, `0x4a2d40` `Engine__nitroTorqueCurve`, `FUN_004afb50` damage roll, `FUN_004b0ff0` health→factor.

---

# Vehicle::update — full order of operations (`0x49c8e0`, read completely)

Conventions used below. `car` = vehicle instance (`param_1`), `M` = vehicle manager (`ECX`, holds the wheel count `M+0x1fb4`), `e` = engine block (`car+0x2050`, byte offsets `e+0x..`), `W` = wheel record (`car+0x1414 + i·0x2e4`), `S` = `W+0x30` suspension object (`c_triggerspring`), `B` = root physics body (`car+0x13ec`, follow `+0x20` while `+0x58 & 4`), `B.state` = active state `B+0x64 + B[0x20c]·0xd4` (pos `+0x64`, vel `+0x70`, orientation rows `+0xa4/+0xb4/+0xc4`, angular velocity `+0xd4`; see `08-physics.md`). `R` = 3×3 orientation, **world = R·local** (`PhysBody__rotateToWorld` `0x5061f0`), so `Rᵀ·v` = world→local. Axes in body-local space: **x = lateral, y = up, z = forward**. `dt` = `param_2` (s). Angular speeds rad/s. All floats are 32-bit; libm calls are the MSVC SSE2 helpers `atanf 0x57687b`, `expf 0x576724`, `sinf 0x570fcb`, `cosf 0x570e7e` (args/returns in XMM0 — Ghidra drops them; formulas below were taken from the assembly).

Config globals (from `FUN_0047c240` = `Config__loadGameplaySettings`): `internal_damage` → `DAT_005dacd4` (default 1.0, multiplies every wear/damage increment below, called **K_dmg**), `external_damage` → `DAT_005dacd0`, `engine_inertia_factor` → `DAT_005da500` (default 4.0), `wheel_gndfeedback_factor` → `DAT_005da4f8` (default 2.8), `wheel_brake_factor` → `DAT_005da4fc` (default 4.0). `esp_*` → `0x5da898..0x5da8a4`, `steerhelp_turn` → `0x5fe320` (used elsewhere, not in this function).

### 0. Early outs, sleep handling
- return if `car+0x2054 ≠ 0` or `dt < 1e-4`.
- `car+0x70` bit0 = **car asleep**. If clear and `B+0x58 & 0x80` (body went to sleep): `car+0x1de0 = car+0x1ddc` (steer), `car+0x1e60 = car+0x1e5c` (gear), set bit0, and for every wheel with `S`: if `S+0x50 ≤ S+0xf0` (no contact) zero `W+0x140..0x148`, else seed `W+0x140 = Rᵀ·groundVel(S)`, `W+0x148 += W+0xc0·W+0x60`; zero `W+0x14c..0x154`. If bit0 clear and body awake: clear bit1.
- If bit0 set and `|throttle| > 0.01` → wake the body (`PhysBody__wakeRoot 0x48ca50`). (Bit0 is cleared elsewhere.)

### 1. Engine (only if `car+0x70 & 0x4000` = engine running; else `ω_e = car+0x1e7c = 0`)
`ω_e = car+0x1e7c`, `rpm = ω_e · 9.549296`, `thr = |car+0x1de4|`.
- `Engine__frictionTorque(rpm)` (`0x4a1510`): `w = rpm·0.10471976`; `w>0`: `a = maxRPM(e+0x18)·0.05`, `f = −w·fric_fwd(e+0xf4)·a`, return `max(f, −0.5·I_e·w)`; `w<0`: return `min(w²·fric_rev(e+0xf8), −0.5·I_e·w)`; `w=0`: 0. (`I_e = e+0xe4 = engine_inertia`.) The clamp means friction can at most halve ω per tick.
- `Engine__torqueCurve(rpm)` (`0x4a2dd0`): table `e+0`=count, `e+4`=ptr to `(rpm, torque)` float pairs; 0 below the first rpm or at/after the last; linear interpolation otherwise. `Engine__nitroTorqueCurve` (`0x4a2d40`) is identical on `e+8`/`e+0xc`.
- If `|rpm| > 0.001`: `T_f = friction(rpm)`, `T_max = torqueCurve(rpm)·car+0x1e04`; `T_net = T_f`. **Idle governor**: if `0 < ω_e < idle(e+0xf0)` and `T_max > 0.001`: `thr = min(thr + |1.1·T_f|/T_max, 1)`.
- **Starter**: if `thr > 0.1` and `ω_e ≤ 0.95·idle`: `car+0x1dfc = 1`, `T_net += starter_torque(e+0xd0)`, `thr = 0`; else `car+0x1dfc = 0`.
- If `|rpm| > 0.001`:
  - **Bucking** (misfire/cut, list `car+0x1e18`, records made by `Chassis.setBuck`): for each record, when `now ≥ rec+0x3c`: toggle `rec+0x38`; `rec+0x3c += (rec+0x30·rpm + rec+0x28) · rand15/32768 · (state ? 1−rec+0x2c : rec+0x2c)`; while state ≠ 0: `thr *= rec+0x34`. (`now = *PTR_DAT_005dadf0`, rand = MSVC LCG `0x571110`.)
  - **Soft rev limiter**: `lim = RPM_limit(e+0x1c)`, `nxt = 2·rpm − car+0x1fb0`; `L = 1`; if `nxt ≤ lim`: `L = (lim−nxt)/(rpm−nxt)` when `lim < rpm`; else if `rpm ≤ lim`: `L = (lim−rpm)/(nxt−rpm)`, else `L = 0`. `car+0x1fb0 = rpm`.
  - `T = L·T_max`; `heat = thr·1000·T_max`.
  - **Nitro**: if `car+0x1df8 > 0.1`, tank `car+0x1f5c > 0`, `consumption(e+0xa58) > 0`: `ΔT = (nitroCurve(rpm)·car+0x1e08 − T)·car+0x1df8·L`; `T += ΔT`; `heat += 2000·ΔT`; `tank −= L·dt·car+0x1df8·e+0xa58·rpm·0.0002`.
  - `car+0x1e8c = (thr > 0.01) ? 2·T·thr : 0`; `T_net += car+0x1e8c` (**note the factor 2**).
  - **Thermal** (skipped if `car+0x70 & 0x400`): `if heat·dt > 0: Temp(car+0x1ec0) += heat·dt / car+0x1ebc` (`= engine_mass·8000`). Cooling coefficient `car+0x1ec8 = c / car+0x1ebc`, `c = e+0xa5c` if `Temp ≤ 90` or radiator damage `car+0x1eac ≥ 1`, else `c = health(car+0x1eac)·(|v_body|²·e+0xa64 + e+0xa60) + e+0xa5c` (`Chassis.setCooling(a,b,c)` → `e+0xa5c/0xa60/0xa64`, defaults 10, 50, 0.01). Above 100 °C: `damageRoll((Temp−100)²·dt·K_dmg)`. Engine wear (if `car+0x1e9c < 1`): `p = (2dt/car+0x1ea0)·(|rpm|/maxRPM)^16·fric·100·(1 + (|Temp−90|·0.01)²)` with `fric = e+0xf4` (rpm ≥ 0) or `e+0xf8`; if `damageRoll(p·K_dmg)` hits: `car+0x1e04 = e+0xd4·h`, `car+0x1e08 = e+0xd8·h`, `h = Vehicle__healthFactor(car+0x1e9c)` (= `1 − w²·0.6`, 0.1 at w ≥ 1); at `w ≥ 1` both halve again, `e+0xf4/0xf8 *= 50`, event `M+0x1f4c` posted.
  - (`(rpm/maxRPM)^16`: the code squares the ratio four times.)
- Always (engine on): `Temp −= |Temp − T_amb|·car+0x1ec8·dt·(Temp − T_amb)`, `T_amb = car+0x1ec4`; `car+0x1e0c = e+0x24 + 4·gear` (gear ratio of gear index `car+0x1e5c`; `e+0x24..0x40` = `ratio[0..7]`).

### 2. Drivetrain set-up
`G = rearend_ratio(e+0x44) · car+0x1e0c` (total ratio, 0 in neutral). `D = Σ_{wheels with S} |W+0xe0|` (total drive share).
If not asleep, for each wheel (temperatures cool even when asleep: tyre `W+0x178` toward `W+0x17c` with `W+0x180`, brake `W+0x188` toward `W+0x18c` with `W+0x190`, same law as the engine).
If `car+0x70 & 4` (frozen/placed): `Vehicle__resetWheelsFrozen 0x497e60` (ω = 0, `W+0xec = 0`, skid-mark/sound handles released, `car+0x1e78 = 0`, clutch `car+0x1df0 = 1`). Otherwise, per wheel with `S`:
- **Effective inverse inertia** `W+0xec = 0.1 / I_w`, `I_w = e+0xec` (base wheel inertia), plus `|G|·(W+0xe0/D)·I_e·(1 − clutch)` if `D > 1e-4`, `W+0xe0 > 1e-4` and `G ≠ 0`. (`|G|`, not G², in the original.)
- **Impact damage** (if `S` in contact and not `0x400`): `q = |S.normalForce(S+0xe4)| · |W+0x140| · dt·50 · ground.friction(S+0xac → +0x50)`. If `q > 0.1`: tyre heat `W+0x178 += q / W+0x174`; tyre wear roll `damageRoll(2·q·K_dmg)` (if `W+0x15c < 1`): on hit `W+0x70 = W+0x68·f`, `W+0x6c = W+0x64·f`, `f = 1 − w²·0.4` (0.5 at w ≥ 1); at `w ≥ 1` the **tyre is flat**: `r' = 0.8·r` (`W+0x60`), `W+0x5c += r'−r`, re-configure the spring with `r'`, Pacejka slot 13 (`W+0x21c`) = 0.4, event `M+0x1f5c`. Suspension damage (if `W+0x16c < 1`): `q2 = S+0xfc · dt·50 · 3`; if `q2 > 0.1` and `damageRoll(q2·K_dmg)` hits with `W+0x16c ≥ 1`: spring re-configured with `k·0.8`, `bump·0.3`, `rebound·0.3`, event `M+0x1f7c`.
- Tyre cooling (as above).
- **Wheel-average speed** `car+0x1e78 = Σ(−W+0xc0 · W+0xc4 · W+0xe0) / Σ|W+0xe0|` over wheels with `S`, `W+0xe0 > 0.001`, `|W+0xc4| > 0.001` (`W+0xc4` = per-wheel drive ratio/sign from the car template; its inverse maps the shaft back to the wheel).
- **Viscous differential**: for the same wheels, `target = (−1/W+0xc4)·car+0x1e78`, `Δ = target − ω_i`, `step = min(|Δ|·5·lock(e+0xa38)·dt, 1)·Δ`, `ω_i += step`, `W+0x148 += r·step`.
- **Anti-roll bars**: `n = e+0x48` (≤ 8), records at `e+0x4c` of 16 bytes `{int wheelA, int wheelB, float k, float c}`. If both springs carry load (`S+0xf4 > 0`): `F = ((S_A+0xf0 − r_A) − (S_B+0xf0 − r_B))·k + (S_A+0xf8 − S_B+0xf8)·c`; `S_A+0x90 = −F`, `S_B+0x90 = +F` (consumed by the spring solver as an extra force; template count is 0, so inert in shipped data).
- **Contact slip velocity** (not asleep): `W+0x140 = Rᵀ·(groundVel(S) − pointVel(B, S+0xd8))`, `W+0x148 += W+0xc0·r` (so `W+0x148` = longitudinal slip speed `v_z + ωr`), zero if `S+0x50 ≤ S+0xf0`.

### 3. Clutch / gearbox coupling (engine ↔ driveshaft)
If engine off or `car+0x1e0c == 0` (neutral): `car+0x1e80 = ω_e`, `car+0x1e88 = 0`, `car+0x1e84 = T_net`, `ω_e += T_net / I_e` (if `I_e > 1e-6`). **No `dt` factor**: torques here are per-tick angular impulses.
Else (`a = 1/I_e`, `k = 1 − clutch(car+0x1df0)`, `ω_s = car+0x1e78` old):
```
w      = (e+0xe8 / G²) / (engine_inertia_factor(4.0)·I_e + e+0xe8 / G²)
car+0x1e80 += ((ω_e − (1−w)·T_net·a) − car+0x1e80)·0.25      // smoothed rpm shown by getRPM()
ω_d    = ω_s·G                       // shaft speed seen by the engine
ω_free = ω_e + T_net·a
Δ      = (ω_d − ω_free)·w ;  ω_sync = ω_free + Δ
ω_d'   = ω_d    + (ω_sync − ω_d)   ·k·0.999
ω_e'   = ω_free + (ω_sync − ω_free)·k·0.999   → car+0x1e7c
car+0x1e78 = ω_d'/G
car+0x1e88 = 10·k·Δ / w²             // clutch slip torque indicator
per driven wheel: ω_i += ((car+0x1e78 − ω_s)/D)·W+0xe0·(−1/W+0xc4);  W+0x148 += r·that
```
`e+0xe8` = drivetrain (wheel-side) inertia. Wear (not `0x400`): transmission `car+0x1ea4 += car+0x1ea8 · |(ω_d' − ω_e')·k·0.1|·K_dmg`; clutch `car+0x1eb4 += car+0x1eb8 · (|car+0x1e88|·0.01)⁴·dt·1e-4·K_dmg`; both clamped to [0, 1]. The driven-wheel update in the last line is skipped for wheels whose render/script object vetoes it (`W+0x1e0` / instance `+0x50` test).

### 4. Brakes (not asleep), per wheel with `S` and `|ω| > 0.001`
```
T_b = (W+0xfc + brake(car+0x1de8))·e+0xdc·W+0xd8 + e+0xe0·hbrake(car+0x1df4)·W+0xdc + bearing(W+0xf0)
if T_b > 0: Δω = W+0xec·T_b·wheel_brake_factor(4.0)·dt·50
            |ω| ≤ Δω → ω = 0 (W+0x148 −= r·ω) ; else ω −= sign(ω)·Δω (W+0x148 adjusted by r·Δω)
```
`car+0x1de8`/`car+0x1df4` are stored as |value| first. Brake heat (not `0x400`): `q = |ω'|·(T_b − bearing)·0.01·dt·50`; `W+0x188 += q / W+0x184`; brake wear `W+0x164 += W+0x168·q·K_dmg` (clamped [0,1]); effective `W+0xd8 = W+0xd0·f`, `W+0xdc = W+0xd4·f`, `f = 1 − w²·0.6` (0.1 at w ≥ 1); at `w ≥ 1`: `bearing += 100`, both ×0.2, event `M+0x1f6c`. Then brake cooling.

### 5. Wheel spin angle / rim wobble
For wheels with `S` (and render-object test) and `|ω| > 0.001`: `W+0xe8 += ω·dt`, wrapped into [−π, π]; `off = sin(W+0xe8)·W+0xf4`; `Suspension__configure(S.k, S.bump, W+0x5c + off, W+0x54 + r + off, W+0x58 + r + off, S.rebound, r)`; `car+0x70 |= 0x200000`. (`W+0xf4` = wobble amplitude, 0 unless bent.)

### 6. Tyre forces (the Pacejka evaluation is **inline here**, `0x49eac7..0x49fb14`; no separate routine)
For each wheel with `S`, in contact (`S+0x50 > S+0xf0`), load `F_N = S+0xf4 > 0` and ground material `gm = S+0xac ≠ 0` (else `W+0x14c..0x158 = 0`):
```
vs  = −ω·r                                  // tyre surface speed (signed)
v   = Rᵀ·(groundVel(S) − pointVel(B, S+0xd8))   // contact velocity, body frame
vz  = v.z ; vx = W+0x140 (lateral slip velocity from step 2)
b   = clamp((|vz| − 0.1)·0.1, 0, 1)         // high/low speed blend (1 at 10.1 m/s)
if |vz| > 0.1: κ = (vs − vz)/|vz| ;  α = atan(vx/|vz|)   else κ = α = 0
if b < 1:                                    // relaxation-length low-speed model
   κl = ((vs − vz)·dt + 0.3·κ_prev(W+0x14c)) / ((dt·50·0.1 + 1)·0.3) ; |κl| ≤ 1
   αl = (0.5·α_prev(W+0x150) + vx·dt)       / ((dt·50·0.2 + 1)·0.5) ; |αl| ≤ 1
   κ = b·κ + (1−b)·κl ;  α = b·α + (1−b)·αl
sx = sin α ;  sz = cos α · p12(Frictn_x) · κ ;  σ = sqrt(sx² + sz²)
if σ ≤ 1e-4: store σ, κ, α (W+0x154/14c/150), W+0x158 = 0, no force.
n   = Rᵀ·S.normal(S+0xc4)                    // ground normal, body frame
p2  = gm.friction(+0x50) · W+0x6c            // written into slot 2 every tick
p13 = gm.rollres(+0x54) + W+0x74             // written into slot 13 every tick
cam = (sqrt(n.y² + n.z²) + 1)·0.5            // camber/tilt grip factor
Fz  = cam · ( p14·p11·(2/π)·atan((π/2)·F_N/p11) + (1−p14)·min(F_N, p11) )   // p11 maxLoad, p14 loadSmooth
p4' = p4 ; if p16 > 0: p4 = p4·(p15 + p16)/(p16 + Fz)   (restored after the wheel)
D   = p1·Fz + p2                             // friction coefficient μ
CD  = p0·D                                   // p0 = C (shape)
if CD == 0: F = 0 else
   B  = (p3·Fz + p4)·exp(−p5·Fz) / CD
   x  = σ + p9·Fz + p10                      // horizontal shift
   E  = p6·Fz² + p7·Fz + p8
   F  = max(0, Fz·D·sin(p0·atan(B·x + E·(atan(B·x) − B·x))))
F_lat  = F·sx/σ ;  F_long = F·sz/σ
Δω  = F_long·r·W+0xec·dt·50·wheel_gndfeedback_factor(2.8)
// overshoot guard: recompute κ' with ω+Δω (same blend as above); if κ'·κ < 0:
//   s = |κ/(κ'−κ)| ; Δω *= s ; F_long *= s
W+0x154 = σ ; W+0x150 = α ; W+0x14c = κ ; ω += Δω ; W+0x148 += Δω·r
W+0x158 = Mz = p2·Fz·1.49·sin(−2.3·atan(Bα·α − (atan(Bα·α) − Bα·α)))   with Bα = 15.2/(−2.3·1.49)
          // fixed-coefficient aligning-torque curve (constants DAT_005da8f8..920); stored only — no consumer exists (not applied, not read by FFB/steering)
f   = (F_lat, 0, vz·Fz·p13·0.01 − F_long)    // contact frame: x lateral, y normal, z forward
```
Pacejka slot map `W+0x1e8 + 4·i`: `p0 C` (`setSliction`), `p1 μ/load`, `p2 μ` (overwritten per tick), `p3/p4/p5` BCD (`p4` = `setStiffness`), `p6/p7/p8` E, `p9/p10` Sh, `p11` max load (`setMaxLoad`), `p12` longitudinal scale (`setFrictn_x`), `p13` rolling resistance (overwritten per tick), `p14` load smoothing (`setLoadSmooth`), `p15/p16` load-dependent stiffness. The 17 defaults are copied from `M+0x2ec4` (0x44 bytes) on create and on every `refreshParameters`.
**Force application**: contact frame `[a | n | b]` with `a = normalize(n × ẑ)`, `b = normalize(a × n)` (columns via `Mat34__setColumn 0x567250`); `f_body = [a|n|b]·f`; `f_world = R_S·f_body` (spring body `S+0x94`). Application point `P = S+0xd8` (contact point, body local); if contact patch `S+0xa0 ≠ 0`: `t = clamp(n.x / S+0xa0, −1, 1)`, `P.x += S+0xa4 − S+0x9c·t`. `P` is carried up the parent chain to the root body, `f` converted to root-local (`Rᵀ_root·f_world`) and then:
- **Instant-centre jacking** (`W+0x78..0x80` = hub position, `hub.y += r − min(S+0x50, S+0xf0)`): if `W+0x284 < 1000` and `|IC1.x − hub.x| > 0.1` (`IC1 = W+0x27c..0x284`): `f.y += (IC1.y − hub.y)·f.x/(IC1.x − hub.x)`; if `W+0x288 < 1000` and `|IC2.z − hub.z| > 0.1` (`IC2 = W+0x288..0x290`): `f.y += (IC2.y − hub.y)·f.z/(IC2.z − hub.z)`. (`setInstantCenter(x1,y1,z1,x2,y2,z2)` stores hub + args; 1000 in the unused coordinate disables.)
- wake root, clear state bit0, `PhysBody__addLocalForceAtLocalPoint(root, f, P)` (`0x504e70`: `F += R·f`, `τ += (R·P) × (R·f)`).

### 7. Aerodynamics and extra forces (not asleep)
- **Drag** (`C_drag = e+0x91c > 0`): `v = pointVel(B, drag_center(e+0x910))`; if `|v|² > 1`: `F = −C_drag·|v|²·v̂`. Implicit clamp: effective mass `m_eff = 1/(invMass(B+0x5c→+0x2c) + v̂·((I⁻¹(d×v̂))×d))` (`d = R·drag_center`, world inverse inertia rows at state `+0x108..0x130`), momentum `P = m_eff·v`, `h = P/2`; if `|F·dt + h|² > |h|²` then `F = ((F·dt + h)·|h|/|F·dt + h| − h)/dt` (drag can at most stop the point). `car+0x2248 = |F|`; `PhysBody__applyForceAt(B, F, drag_center)` (`0x5051e0`: world force at a local point, `τ += (R·p) × F`).
- Re-verified 2026-10-24 (card 161, asm 0x49fb34..0x49ff40): `car+0x2248 = 0` every tick first (0x49fb38), the whole §7 is skipped when `car+0x70 & 1`; `B` = `car+0x13ec` walked up `+0x20` while `B+0x58 & 4` (= root, car_root); `e = car+0x2050`. Constants: 0x593580 = 0.0 (C_drag gate), 0x593598 = 1.0 (|v|² gate), 0x593590 = 0.5 (`h = P/2`), 0x5947a0 = 0.25 (`|h|² = |P|²/4`). `d = R·drag_center` via 0x5061f0 (rotation only); `m_eff = 1/(invMass + v̂·((I⁻¹(d×v̂))×d))`. After `applyForceAt` 0x5051e0 the other state's bit0 is cleared (0x49ff39, body+0x174 − active·0xd4). Drag is a bare coefficient: no frontal area or air density term anywhere. Note: notes/14 / src/audio/sfx.h call car+0x2248 an exhaust-pop timer; 0x49fb38/0x49ff20 write it as the drag magnitude each tick — resolved 2026-10-24: the only reader is `type_car__vf13` 0x494910 at 0x495193, a wind/aero sound intensity (notes/14 §2.3), not an exhaust timer.
- Aero parts list `car+0x2144`: `AeroForce__apply 0x4e7b10` per entry with `+0x28 ≠ 0` (switch on a force kind 1..6: damping, scaled point velocity, plane-projected, wing lift with stall, drag, thrust); returned magnitudes add to `car+0x2248`.
- Hinged parts (doors/hood, `car+0x1518` records of 0x8c bytes): `Vehicle__updateFlaps 0x4a8430` (pendulum with angle limits `+0x24/+0x28`, events on hitting stops, detaches above 350 m/s relative speed); if it returns > 0.001 and the car is asleep the body is woken. Then slot-attached sub-objects (`car+0x2190` list) are re-posed (`car+0x70 |= 0x80000`).
- `car+0x1fa4 ≠ 0` and `|car+0x1f6c| > 0.1`: rate controller about the local **z** (forward/roll) axis: `τ_z = (ω_z − 4·car+0x1f6c)·(−30)·B.def+0x88` (`ω_z = (Rᵀω).z`, `B.def+0x88` presumably an inertia term) via `PhysBody__addLocalTorque 0x505090`.
- `|car+0x1f68| > 0.1`: lift force at local `(0,3,0)`: if that point's `v.y < 0.5`: `F = −2·mass·gravity(PTR+0x1c4..1cc) − 10·v` (`B.def+0x2c` = inverse mass) via `PhysBody__applyForceAt`.
- `|car+0x1f64| > 0.1`: push at local `(0,−0.2,2)`, direction from steering `φ = car+0x1ddc·maxSteer(e+0xa20)`: `F = mass·R·(20·sin φ, 0, −20·cos φ)` (i.e. a 20 m/s² push).
- Manager vector `M+0x135c..0x1364` (wind?; only if `|·|² > 0.01`) with `car+0x1f70/0x1f74`: local torque `(car+0x1f70·M+0x135c, car+0x1f74·M+0x1360, car+0x1f74·M+0x1364)`.

### 8. Telemetry, odometers, wear
`car+0x1fd8 = 0.8·old + 0.2·|v|` (speed); `car+0x1fc0..c8 += (Δv/Δt − old)·min(5dt, 1)` (acceleration from the two state buffers, `Δt` from state `+0x88`); `car+0x1fcc..d4` = same smoothing of angular velocity; `car+0x1e90 += |ω_e|·dt`, `car+0x1e94 += |car+0x1e78|·dt`, `car+0x1e98 += speed·dt`. If not `0x400` and moving: general wear `car+0xb8 += car+0xbc·speed·dt·K_dmg` (clamp [0,1]); every installed part (`car+0xdc` list) gets `FUN_00497520(part, …, dt)`; mileage `car+0x2250 += speed·dt` (`Chassis.getMileage`).

## Vehicle instance fields (`car+…`, confirmed by the code above and the natives)
| offset | meaning |
|---|---|
| `+0x70` | flags: bit0 asleep, bit1, bit2 (`4`) frozen, `0x400` no thermal/wear, `0x800` nitro active (`getNitroing`), `0x4000` engine running, `0x80000` slot objects moved, `0x200000` wheels rotated, `0x800000` cleared by refresh |
| `+0xb8/+0xbc` | general wear / rate |
| `+0xdc` | installed-part list; `+0x13ec` root body; `+0x1414` wheel array; `+0x1518/+0x151c` flaps |
| `+0x1ddc` steer, `+0x1de0` steer (sleep copy), `+0x1de4` throttle, `+0x1de8` brake, `+0x1df0` clutch (1 = open), `+0x1df4` handbrake, `+0x1df8` nitro | control inputs (`getAIparam_float(0..5)` = throttle, brake, handbrake, steer, clutch, nitro) |
| `+0x1dfc` | starter cranking |
| `+0x1e04/+0x1e08` | torque / nitro-torque health factors (`e+0xd4/0xd8 · health(+0x1e9c)`) |
| `+0x1e0c` | current gear ratio; `+0x1e5c` gear index, `+0x1e60` copy |
| `+0x1e18` | bucking list (`setBuck`) |
| `+0x1e78` | driveshaft/wheel-average speed (rad/s); `+0x1e7c` engine ω (rad/s, `setRPM` writes it raw); `+0x1e80` smoothed ω (`getRPM` = ·9.549296); `+0x1e84` T_net (neutral); `+0x1e88` clutch slip; `+0x1e8c` combustion torque |
| `+0x1e90/94/98` | engine revolutions·2π, shaft revolutions·2π, distance |
| `+0x1e9c/+0x1ea0` | engine damage / durability; `+0x1ea4/+0x1ea8` transmission wear/rate; `+0x1eac/+0x1eb0` radiator damage/rate; `+0x1eb4/+0x1eb8` clutch wear/rate (set from part damage records `Vehicle__applyPartWearRecord 0x4a8060`: kind 5 tyre `W+0x15c/160`, 6 brake `W+0x164/168`, 7 suspension `W+0x16c/170`, 8 engine, 9 transmission, 10 clutch, 12 radiator) |
| `+0x1ebc` | engine heat capacity (`engine_mass·8000`); `+0x1ec0` engine temp °C (`getEngineTemp`); `+0x1ec4` ambient; `+0x1ec8` cooling coefficient |
| `+0x1f5c` | nitro left (`getNitro`); `+0x1f64/68/6c/70/74` push/lift/roll-rate/torque helper inputs; `+0x1f78[i]` per-wheel friction override, `+0x1f88` torque override, `+0x1f8c/+0x1f90` front/rear drive override (`Vehicle__applyTuningOverrides 0x498020`); `+0x1fa4` roll-rate controller enable |
| `+0x1fb0` | last rpm (limiter); `+0x1fc0..d8` telemetry |
| `+0x2050` engine block, `+0x2054` traffic controller (GII_CAR_TRAFFICPTR 0x38; update skipped while set); `+0x2144` aero parts; `+0x2190` slot objects; `+0x2248` drag magnitude; `+0x2250` mileage |

## Engine block `e` (0xc34 bytes; template cloned by `EngineBlock__clone 0x4b0910` from the manager's default `M+0x2ebc`)
| offset | meaning |
|---|---|
| `+0x0/+0x4` | torque table count / ptr `(rpm, Nm)` pairs (built from the `DynoData` engine definition `e+0xc30`: `+0x1d0` step, `+0x1d4` count, `+0x1dc` torque table) |
| `+0x8/+0xc` | nitro torque table (from `+0x1e0`, or the normal table if absent) |
| `+0x10` | shifting state (`getShifting`); `+0x14` from engine def `+0x1c0` |
| `+0x18` maxRPM, `+0x1c` RPM_limit, `+0x20` gears (int), `+0x24..0x40` ratio[8], `+0x44` rearend_ratio | |
| `+0x48` | anti-roll bar count (template default 0); `+0x4c..0xcb` 8 × `{int wheelA, int wheelB, float k, float c}` (16-byte stride), copied 0x80 bytes by `EngineBlock__clone`; no writer besides the template found — see below |
| `+0xcc` ClutchF, `+0xd0` starter_torque, `+0xd4/+0xd8` torque multipliers (`setTorque`), `+0xe4` engine_inertia, `+0xe8` drivetrain inertia, `+0xec` base wheel inertia, `+0xf0` idle ω (rad/s), `+0xf4/+0xf8` friction fwd/rev | template defaults 500.0 / 50.0 / 200.0, 300.0 / 10.0 / 1000.0 / 50.0 / 83.86 rad/s / 1e-4, 0.03; `refreshParameters` overwrites `+0xe4` (script field `engine_inertia`) and `+0xd0/+0xd4/+0xd8/+0xcc/+0xf0/+0xf4/+0xf8` |
| `+0xdc` brake-torque factor, `+0xe0` handbrake factor | `type_car__vf1 0x49ab20` seeds 100.0 / 200.0, multiplies the car-type per-wheel template brake/handbrake bases (`M+0x1fd8` / `M+0x1fdc`, stride 0xc8) by them once, then resets both to 1.0; `EngineBlock__clone` copies them; the only reader found is the brake formula (§4); no other writer |
| `+0xa34/+0xa3c/+0xa40/+0xa44` | steerhelp / ABS / ASR / ESP values, set by the cfg parser with car flags `0x200/0x80/0x100/0x4000000`; template defaults 1.0 / 0 / 0 / 0 |
| `+0x808/80c/810` | rpm_trans_fwd/rev, sfx_starter_rpm |
| `+0x910..+0x91c` | drag_center (Vector3), C_drag |
| `+0xa20` | max steer (`setMaxSteer`); `+0xa28/+0xa2c` steering-wheel params (`setSteerWheel`, `setSteerWheelRadius` → `+0xa28`); `+0xa30` Ackermann |
| `+0xa38` | differential lock (viscous coupling rate); template default 0.1, cfg `difflock` (value > 0 else 0.1); per-tick rate multiplier in §2 |
| `+0xa4c` tank_nitro, `+0xa58` consumption_nitro, `+0xa5c/a60/a64` cooling (`setCooling`) |
| `+0xc30` | engine definition (`DynoData` native, 0x1ec bytes) |

### Engine-block template writers (resolved, 2026-10)
The offsets previously listed as unknown (`+0xdc/+0xe0/+0xe8/+0xec/+0xa38`) are **engine-block fields**, not wheel fields: the wheel record is only 0x2e4 bytes and has no `+0xa38`; its same-numbered fields are unrelated (`W+0xdc` effective handbrake base, `W+0xe0` drive share, `W+0xe8` spin angle, `W+0xec` inverse inertia). Their writers: `W+0xdc` ← template `[-0x25]` in `createCar` and the brake-wear rescale (§4); `W+0xe0` ← template `[-0x27]`, `setDrive 0x4b6330`, `Vehicle__applyTuningOverrides`; `W+0xe8` ← spin integration (§5); `W+0xec` ← effective-inertia computation (§2, `0.1/I_w`).
- `type_car__vf1 0x49ab20` initializes the manager's default block `M+0x2ebc`; `EngineBlock__clone 0x4b0910` copies it into each car. `Vehicle__refreshParameters 0x4aebf0` then overwrites only the script-driven fields above; it does **not** touch `+0xdc/+0xe0/+0xe8/+0xec/+0xa38/+0x48/+0x4c..0xcb`.
- `+0xdc/+0xe0`: seeded 100.0 / 200.0; the same routine multiplies the car-type per-wheel template brake base at `M+0x1fd8` and handbrake base at `M+0x1fdc` (stride 0xc8 = 0x32 floats) by them once, then resets both multipliers to 1.0. Clones therefore carry 1.0 while `W+0xd0/+0xd4` already bake the 100×/200×. Read only in the brake formula (§4).
- `+0xe8` drivetrain inertia = 1000.0 and `+0xec` base wheel inertia = 50.0 are template constants copied by the clone; no cfg key or other writer found.
- `+0xe4` engine inertia = template 10.0, overwritten from the car script field `engine_inertia` by `refreshParameters`.
- `+0xa38` difflock = template 0.1, overwritten by cfg `difflock` (value > 0 else 0.1).
- Anti-roll: `type_car__vf1` sets only the count `+0x48 = 0` and never touches `+0x4c..0xcb`; the engine ctor `FUN_004904c0` does not initialize them either, and no cfg key/native/other writer was found. The records are copied 0x80 bytes by the clone. With the shipped default count 0 the §2 loop is inert; treat the feature as legacy/unreachable unless a modded template sets the count (no such source found).
- `Vehicle__applyTuningOverrides 0x498020` writes `W+0xe0`/`W+0xcc` (front/rear drive override), scales `W+0x64/+0x68`, and multiplies `e+0xd4/+0xd8` by `car+0x1f88`; it does not write `e+0xdc/+0xe0`.
- `EngineBlock__clone 0x4b0910` copies `+0x00..0x14`, ratios `+0x20`, rearend `+0x44`, anti-roll count (`[0x12]` = `+0x48`) + 0x80 bytes from `+0x4c`, `+0xcc..+0xf8` (including `+0xdc,+0xe0,+0xe4,+0xe8,+0xec`), and `+0xa38` (`[0x28e]`).

### Car cfg parser (`type_car__vf14 0x4a2e60`)
Text key/value parser inside the car-type script (compare helper `0x57c11e`, scan helper `0x569380`; the decompiler renders the compare result as a float 0-test). Keys handled here:
| key | effect on `car` / `e` |
|---|---|
| `abs` | value > 0: set flag `0x80`, `e+0xa3c = 1/value`; else clear flag |
| `asr` | value > 0: set flag `0x100`, `e+0xa40 = 1/value`; else clear flag |
| `esp` | value > 0: set flag `0x4000000`, `e+0xa44 = value`; else clear flag and zero `W+0xfc` for every wheel |
| `difflock` | value > 0: `e+0xa38 = value`; else `e+0xa38 = 0.1` |
| `steerhelp` | value > 0: clear flag `0x200`, `e+0xa34 = value`; else set flag `0x200`, `e+0xa34 = 0` |
| `cruise` | value > 0: set flag `0x1000`, `FUN_004ebd90(car, 0x400000, 0)`; else clear flag, `FUN_004e8820(car, 0x400000)` |
| `damage_multiplier` | `car+0x1f54 = value` (parser default 1.0) |
The same parser also handles an integer key at `0x59b738` → `car+0x1f40` and `pickup` (not decoded). Consumers of flags `0x80/0x100/0x200/0x4000000/0x1000` are not in `Vehicle::update`.

## Wheel record (0x2e4) — fields used by `Vehicle::update` and the remaining setters
| offset | meaning |
|---|---|
| `+0x30` | suspension object `S` |
| `+0x38/+0x3c/+0x40` | template lengths (create: `+0x50 = +0x38 + +0x3c`, `+0x54 = +0x38 + 0.1`, `+0x58 = −1`) |
| `+0x44/48/4c` | k / bump / rebound (script copies); `+0x50/54/58` rest / **MaxLen** / **MinLen**; `+0x5c` rest + r; `+0x60` radius (`setRadius` reconfigures the spring) |
| `+0x64/+0x68` base friction, `+0x6c/+0x70` effective (tyre wear); `+0x74` rolling resistance (default 0.015) |
| `+0x78..0x80` | hub position, car frame (`setPos` = slot position + offset; also shifts both instant centres and re-poses the spring `FUN_004f4380(pos, W+0x110)`) |
| `+0x84..0x8c` | template vector; `+0x90..0xbc` orientation 3×4 (`setYpr`, Euler→matrix), copied to `+0x110..0x13c` |
| `+0xc0` | **wheel spin ω** (rad/s, +forward); `+0xc4` drive ratio/sign; `+0xc8` steer factor (`setSteer`); `+0xcc` drive (base, `setDrive`), `+0xe0` drive share (effective) |
| `+0xd0/+0xd4` | brake / handbrake base (`setBrake/HBrake`); `+0xd8/+0xdc` effective (brake wear `+0x164`) |
| `+0xe4` | 0 (create); `+0xe8` spin angle (rad); `+0xec` inverse effective inertia ×0.1 |
| `+0xf0` | bearing friction (`setBearing`); `+0xf4` wobble amplitude; `+0xf8`, `+0x100` flags (`&0x20` used by `getWheelDamage`); `+0xfc` extra brake drag |
| `+0x140..0x148` | contact slip velocity (body frame; `z` includes `+ωr`) |
| `+0x14c` κ, `+0x150` α, `+0x154` σ, `+0x158` Mz | per-wheel outputs of §6; only κ/α are read back (previous-tick relaxation state); σ and Mz are write-only (no WheelRef getter, no FFB/steering consumer found) |
| `+0x15c/+0x160` | tyre wear / rate; `+0x164/+0x168` brake wear / rate; `+0x16c/+0x170` suspension damage / rate |
| `+0x174` tyre heat capacity (30000), `+0x178` tyre temp (20), `+0x17c` ambient (20), `+0x180` cooling (1/600) |
| `+0x184` brake heat capacity (50000), `+0x188` brake temp (20), `+0x18c` ambient (20), `+0x190` cooling (0.001) |
| `+0x194..0x1c4` | two skid-mark/sound handles (`+0x19c`, `+0x1bc`) with owner `+0x1ac/+0x1cc` = car, index `+0x1b0/+0x1d0` |
| `+0x1d4` | half width (`setWidth(w)` → `w·0.5`, default 0.1); `+0x1d8..0x1e4` render-part link (`+0x1e0` "has render object") |
| `+0x1e8..0x228` | Pacejka p0..p16 |
| `+0x22c` car; `+0x230` template; `+0x234` arm param, `+0x238..0x240` arm axis (normalised), `+0x244..0x24c` arm pivot (`setArm(a, px,py,pz, ax,ay,az)`) |
| `+0x250` | opposite wheel ptr (`setOppWheel(i)`, −1 → null) |
| `+0x254..0x278` | hub geometry (`setHub(k, v1[3], v2[3], p[3])`; if `k == 0`: `v2` normalised and `p −= v1`) |
| `+0x27c..0x284` IC1 (roll), `+0x288..0x290` IC2 (pitch) | (`setInstantCenter`) |
| `+0x2e0` | 0 (create) |

## Suspension object `S` (`c_triggerspring`, solver `c_triggerspring__vf3` `0x4f2d70`)
| offset | meaning |
|---|---|
| `+0x4c` rest length, `+0x50` max length (ray length; contact iff current < this), `+0x54` bump-stop length, `+0x58/+0x68` radius | |
| `+0x5c` k, `+0x60` bump damping, `+0x64` rebound damping | |
| `+0x6c..0x74` | mount point (chassis local); suspension axis `DAT_005dadd0..d8` = (0,1,0), contact-patch lateral axis `DAT_005dadc4..cc` = (1,0,0) |
| `+0x84..0x8c` | extra force vector (cleared each solve); `+0x90` extra scalar force (anti-roll) |
| `+0x94` chassis body; `+0x9c/+0xa0/+0xa4` contact patch (`setCPatch`); `+0xac` ground material (`+0x50` friction, `+0x54` rolling resistance) | |
| `+0xb4` | ray hit distance² (reset to `max² + 0.1` = no hit); `+0xb8..0xc0` contact point on the ground body; `+0xc4..0xcc` ground normal (world); `+0xd0` ground body (0 = no ground body / ray miss; static ground is a real body with flag `4`) |
| `+0xd8..0xe0` | contact point (chassis local) = mount − len·axis; `+0xe4..0xec` normal force vector |
| `+0xf0` current length = min(√hit², max); `+0xf4` load `F = S+0x90 − (c·2v + (len − rest)·k)` with `c` = rebound if `v ≥ 0` else bump; `+0xf8` suspension velocity; `+0xfc` damper work `|c·2v·Δlen|` | |

**Solver order** (`c_triggerspring__vf3 0x4f2d70`; no direct callers — its address is vtable slot `+0xc` at `0x59dbb8`, the force callback run by the physics pass):
1. If the chassis is unusable/asleep (`def+0xb4` bit2 set or `body+0x58` bit7 set, `def = *(body+0x5c)`) **and** the ground is absent/static (`def+0xb4` bit4) or asleep (`body+0x58` bit7), unlink the spring and re-queue it on the world sleep list (`g_physWorld+0x154/0x15c`); return 0.
2. No ground (`S+0xd0 == 0`) or ray miss (`S+0x50² ≤ S+0xb4`): `S+0xf0 = S+0x50`, `S+0xf4 = 0`, `S+0xb4 = S+0x50² + 0.1`, clear `S+0x84..0x90`; return 0.
3. `len = min(√(S+0xb4), S+0x50)` → `S+0xf0`; `S+0xb4 = S+0x50² + 0.1`; previous length kept in a local. Contact point `S+0xd8..e0 = mount(S+0x6c..0x74) − len·axis` (axis `(0,1,0)`, `DAT_005dadd0..d8`).
4. Point velocities: chassis body (`S+0x94`) at `S+0xd8`, ground body at `S+0xb8`.
5. Gate: if the ground normal (`S+0xc4..cc`) projected on the chassis up-axis is negative, zero the load and return.
6. `v = S+0xf8` = relative suspension velocity; the sign is such that `v < 0` = compression (consistent with the bump-stop branch). Compression `Δ = len − prevLen`.
7. Damper `c = S+0x64` (rebound) if `2v ≥ 0`, else `S+0x60` (bump); damper work `S+0xfc = |c·2v·Δ|`; load `S+0xf4 = S+0x90 − (c·2v + (len − S+0x4c)·S+0x5c)`.
8. `S+0xf4 < 0` → `S+0xf4 = 0`, clear `S+0x84..0x90`, return 0.
9. Bump stop: if `len < S+0x54` and `v < 0`, add the normal impulse `−v / (effective inverse mass) / dt` to the force (effective mass sums the chassis and ground inverse-mass/inertia terms along the normal; a static ground contributes only the chassis).
10. Total force = `(load + bump impulse)` along the chassis up-axis (`R·(0,1,0)`, `DAT_005dadd0..d8`). A Gram-Schmidt correction then removes its component along the ground-plane projection of the chassis forward axis (`R·(0,0,1)`, `DAT_005daddc..e4`), so the spring does not push the car forward/backward. If `S+0xa0 ≠ 0` the application point shifts laterally by `S+0xa4 − S+0x9c·clamp(n.x/S+0xa0, ±1)` along `(1,0,0)` (`S+0x9c/+0xa0/+0xa4` = contact-patch params, `DAT_005dadc4..cc`).
11. Apply with `PhysBody__applyForceAt` at the (shifted) contact point, clear body state bit0 and wake the root; apply the reaction `−F` at the ground point unless the ground body is static/kinematic (`ground+0x5c+0xb4 & 4`). Store the normal projection of the load vector in `S+0xe4..ec`, clear `S+0x84..0x90`, return 1.

**Wheel vertical state.** There is none: no wheel body and no wheel vertical DOF is integrated. The raycast fills `S+0xb4` / `S+0xd0..cc` before the solve (writer not located; physics trigger/ray pass), the solver turns the current hit distance into `S+0xf0` and applies forces to the chassis body, and the physics world integrates the chassis `PhysBody` (`0x5001c0`/`0x506530`). Wheel vertical position is derived as chassis transform + `S+0xd8`.

**Configure call sites.** `Suspension__configure 0x4f4280` is called by `setForce 0x4b6380` (writes `W+0x44`), `setMaxLen 0x4b68b0` (writes `W+0x54`), `setMinLen 0x4b6990` (writes `W+0x58`), `setRadius`, `setRestLen`, and the wobble re-configure in `Vehicle::update`, always with `(k=W+0x44, damping=W+0x48, rest+r=W+0x5c, max+r=W+0x54+r, min+r=W+0x58+r, rebound=W+0x4c, radius=r)`.

`Wheel__contactQuery 0x4f3d30` = `S.groundVelocity`: velocity of the ground body at `S+0xb8` (0 if `S+0xd0 == 0`, i.e. no ground body); `0x4f3d20` returns `S+0xd8` and is called from `Vehicle::update`, `0x4af540` (skid-mark particle emitter) and `0x4af830` (skid sound) — neither of the last two applies suspension forces.

## Natives decoded in this pass
- `Chassis`: `getRPM` = `car+0x1e80·9.549296`; `setRPM(f)` → `car+0x1e7c` (raw); `getTorque(rpm, n)` = `torqueCurve(rpm)·car+0x1e04·(1−n) + nitroCurve(rpm)·car+0x1e08·n`; `setTorque(f)` → `e+0xd4 = e+0xd8 = f` and refresh factors; `setCooling(a=10,b=50,c=0.01)`; `setMaxSteer` → `e+0xa20`; `setAckermann` → `e+0xa30`; `setSteerWheel(a,b)` → `e+0xa28/0xa2c`; `setSteerWheelRadius` → `e+0xa28`; `setBuck(id, handle, a, b, c, f)` creates/updates/removes a bucking record (`+0x24 id, +0x28 a, +0x2c b, +0x30 c, +0x34 f, +0x38 state=1, +0x3c now`; `handle>0` with `a,b ≤ 0` deletes); `forceUpdate` = `refreshParameters`; `getShifting` = `e+0x10`; `getNitroing` = flag `0x800`; `getEngineTemp` = `+0x1ec0`; `getNitro` = `+0x1f5c`; `getMileage` = `+0x2250`; `getAIparam_float(0..5)` = throttle, brake, handbrake, steer, clutch, nitro; `get/setOSD(i)` read/write a global HUD/OSD table (`+0x1b0c..0x1b60`, cases 4 and 9 are speed/rpm-scaled needles); `getWheelDamage(i)` formats 10 floats (hub pos, Euler, `S+0xf4`, 1, 1, `S+0xf8`) when `W+0x100 & 0x20`.
- `WheelRef`: offsets in the wheel table above (`setDrive`, `setSteer`, `setBrake`, `setHBrake`, `setBearing`, `setArm`, `setHub`, `setInstantCenter`, `setOppWheel`, `setWidth`, `setPos`, `setYpr`, `setRadius`, `setRollRes`, `setFriction`).
- `DynoData.calcDyno(steps)` (`0x4cba80`): builds a 0x1ec-byte engine definition from script fields (`cylinders`, bore/stroke ×0.5, valve timing `time_in/out_open/close`, `time_burn`, spark `time_spark_*`, turbo `rpm_turbo_*`, `P_turbo_max/waste`, `mixture_H/ratio`, `max_air/fuel_consumption`, `intercooling` (×293.15 K), `nitro_H`, `nitro_cooling`, `maxRPM`), runs a thermodynamic engine model (`FUN_004f5980`, `FUN_004f59f0`, table builder `FUN_004f4640(step = maxRPM/steps)`), writes `torquetable`, `HPtable`, `torquetable2` (nitro), `Displacement`, `Compression`, `maxTorque`, `RPM_maxTorque`, `maxHP`, `RPM_maxHP` back to the script. `getTorque(rpm)`/`getHP(rpm)` linearly interpolate `+0x1dc`/`+0x1e4` with step `+0x1d0` (int) and count `+0x1d4`. This same struct is the `e+0xc30` engine definition used by `refreshParameters`. (The thermodynamic model itself is not decoded yet → candidate for `notes/10-engine-dyno.md`.)

## Tyre force / Pacejka — second pass (address-verified)

**No standalone tyre routine.** The complete per-wheel force evaluation is inline in `Vehicle::update` (`0x49eac7..0x49fb14`). `FUN_004f3d30` (contact/ground-velocity helper, see "Suspension object") has exactly one caller, `Vehicle::update`; `FUN_004f3d20` returns `S+0xd8` and is called from `Vehicle::update`, `0x4af540`, `0x4af830`.

**Slot map, writers, defaults.** Wheel `W` is `*(car+0x1414) + i*0x2e4` (verified in `Chassis.getWheel 0x4b4150`; WheelRef natives receive `W` directly). The 17 floats `p0..p16` live at `W+0x1e8+4i`. Verified writers: `setPacejka(i,v)` 0x4b6a90 (`i` 0..16), `setSliction` 0x4b6fd0 → p0/`+0x1e8`, `setFriction` 0x4b6420 → `W+0x64=W+0x68=W+0x1f0` (p2) plus effective `+0x6c/+0x70` (tyre wear: ×`1−w²·0.6`, min 0.1; corrects the "setFriction writes no slot" claim in notes/13), `setStiffness` 0x4b7050 → p4/`+0x1f8`, `setMaxLoad` 0x4b6950 → p11/`+0x214`, `setFrictn_x` 0x4b6500 → p12/`+0x218`, `setLoadSmooth` 0x4b6870 → p14/`+0x220`. p0 is the magic-formula shape factor C; the shipped `Tyre.java` passes `sliction` (dynamic/static ratio, ~0.695–0.76) to it and the `Wheel.java` fallback calls `setSliction(1.0)`, so C < 1 normally and the lateral force saturates below D (observed behaviour, not documented intent).

**p2/p13 are live per-tick scratch.** Inside the force pass, with `gm = S+0xac`: `W+0x1f0 = gm.friction(gm+0x50) * W+0x6c` and `W+0x21c = gm.rollres(gm+0x54) + W+0x74` (stores at `0x49ef68`, `0x49ef7a`). The formula reads them at `0x49f048` (D), `0x49f3fe` (Mz) and `0x49f431` (rolling-resistance force). So `setFriction`'s p2 write, any script `setPacejka(2/13,…)`, and the flat-tyre `W+0x21c = 0.4` (`0x49d860`) are overwritten on the next force-pass evaluation; the 0.4 survives only while that pass is skipped (airborne, unloaded, or no ground material).

**Formula/provenance checks.** §6 was checked against the asm (`0x49ef9e..0x49f135`); the code forms `B·x` with `x = σ + p9·Fz + p10`, and p4 load-dependent smoothing runs only when p16 > 0 (default 0 ⇒ off). The 17 defaults are `.data 0x5da8b0` = `{1.4, 0, 1.49, 0, 15.2, 0, -0.0, 0, -1.0, 0, 0, 8000, 1.0, 0.015, 0.4, 0, 0}`; `type_car__vf1` copies it to `M+0x2ec4` (0x49bbd4..0x49bc3c), `createCar` copies `M+0x2ec4` to each `W+0x1e8` (memcpy at 0x499c38), and `refreshParameters` re-copies it (0x4aed2d). `FUN_0048fe00` also writes nine slots (0,2,4,11,12,13,14,15,16) into `M+0x2ec4` from a 9×`%f` string (format 0x59da68); it is reached only through the .data callback pointer 0x5da65c, so its caller/source remains unresolved (uncertain). The script defaults match `bin/the game/parts/scripts/rgearpart/reciprocatingrgearpart/Wheel.java` and `Tyre.java`.

**Aligning torque re-check.** The Mz block (`0x49f30a..0x49f410`) reads fixed floats, not wheel slots: `0x5da8f8=-2.3`, `0x5da900=1.49`, `0x5da908=15.2`, `0x5da918=-1.0`, `0x5da920=0.0` (the last is **0.0**, not 8000; refs are only 0x49f32e/0x49f3e4 to 0x5da8f8 and 0x49f393 to 0x5da920). Hence `Bα = 15.2/(−2.3·1.49)`, `inner = 2·Bα·α − atan(Bα·α)`, `Mz = p2·Fz·1.49·sin(−2.3·atan(inner))`, stored in `W+0x158` only (not applied to the body). **Consumers: none found.** Write sites: `Vehicle__createCar 0x4992e0` wheel constructor zero, the per-tick store at `0x49f410`, and the airborne/unloaded zero at `0x49fb0a`. No WheelRef native exposes it; every `+0x158` read examined belongs to a different structure (car records/lists/heap nodes), and no steering/FFB code reads the wheel slot. Mz is dead/diagnostic state, not force feedback.

**p15/p16 (load-dependent stiffness).** The only use is `if (p16 > 0) p4 = p4·(p15+p16)/(p16+Fz)`; p4 is saved on the stack and restored after the wheel (`0x49faca`). Shipped defaults p15 = p16 = 0 disable it. The branches at `0x49f3fe` (Mz, uses per-tick p2) and `0x49f431` (rolling resistance, uses per-tick p13) are outside the p16 branch and are fully accounted for in §6 and above.

**Drivetrain split.** Re-confirmed from §3: per-wheel shaft correction `((ω_shaft_new − ω_shaft_old)/D)·W+0xe0·(−1/W+0xc4)`, `D = Σ|W+0xe0|`; `setDrive` writes `W+0xcc` (base) and `W+0xe0` (effective share); `Vehicle__applyTuningOverrides` can rewrite both from `car+0x1f8c/+0x1f90`.

## Open questions
- Source of the car-type per-wheel template `M+0x2070 + i·0xc8` (`Vehicle__createCar` copies fields from it, including `W+0xc4` drive ratio/sign). Which package/script fills it before `type_car__vf1` scales the brake/handbrake bases is not traced.
- Anti-roll record initialization/source: no writer found; count 0 in the shipped template makes the feature unreachable. Also unverified whether the allocator zeroes `e+0x4c..0xcb`.
- ~~Writer of the suspension raycast results~~ resolved: `c_triggerspring__onSensorContact` 0x4f3d70 via trigger event 0x20 (see "Drive path" below).
- Writer/intent of the `S+0x84..0x8c` extra force vector (the solver orthogonalizes it against the normal).
- The bit0 (asleep) clear site; `FUN_00497520` (per-part wear tick); `AeroForce__apply` kinds; consumers of the ABS/ASR/ESP/steerhelp/cruise flags (`0x80/0x100/0x4000000/0x200/0x1000`) — none are in `Vehicle::update`.
- `S+0xf8` sign is inferred from the damper/bump-stop branches (`v < 0` = compression); worth confirming at runtime.

## Drive path: how `Vehicle::update` is reached per physics sub-step (2026-10-24, cards 145/146 RE)

`0x498b40` is not a function: it lies inside `Vehicle__updateControls` 0x498300 (`_type_car` vf11, gear-shift block). The per-sub-step entry is `_type_car` **vf12 = `Vehicle__update` 0x49c8e0**, reached only through the game's mode-9 ("drive") callback list.

**Registration (once, at native create).** `GameObject__createNative` 0x5240e0: after `typed+0x4c = M->vf3(ctx, params)` (= `Vehicle__createCar` 0x4992e0) it calls `Game__registerByInfo` 0x47d560(&ctx) (ECX = game):
```
if (GameObject__getInfo(ctx, 8,    0)) tail-append node(ctx) to game+0x28 list, game+0x214++   // control
if (GameObject__getInfo(ctx, 9,    0)) tail-append node(ctx) to game+0x74 list, game+0x218++   // drive (GII_DRIVE)
if (GameObject__getInfo(ctx, 0x1c, 0)) tail-append node(ctx) to game+0xc0 list, game+0x21c++   // animate
```
`GameObject__getInfo` 0x47c640: typed object type 1 → script `getInfo` (0x47e450); type 8 (native) → `T+0xc` manager `vf15` (`+0x3c`). `_type_car` vf15 0x4a15c0 returns **1 for codes 8, 9 and 0x1c** (cases 11/12/39 return 1 too), so every car is in all three lists. `Game__registerCallback` 0x47d670(obj, mode 8/9/0x1c) is the same append done explicitly; its callers are `type_car__vf15` case 0x38, `type_ground__vf3` 0x4bade0, `type_part` create 0x4bd040 (mode 0x1c only), and `GameType.registerCallback` 0x4dc740.
- vf15 case **0x38 = `GII_CAR_TRAFFICPTR`** (GameType.java:59) sets `car+0x2054` (the traffic controller; `Vehicle__update`/`updateControls` return early while it is set): clearing it re-registers modes 8 and 9 and posts `"obs_add %d"`; setting it from null sleeps the root, zeroes its velocities and unregisters 8/9 (0x47e390). So `car+0x2054` is the traffic pointer, not a "disabled" flag (correct the table above).

**Consumers.** `PhysicsWorld__step` 0x502060 order per sub-step: `PhysBody__applyForces` 0x4f8460 for every awake root → `runControllers` 0x4ffca0 (every `c_triggerspring` vf3 0x4f2d70 = spring forces) → **`Game__runDriveCallbacks` 0x47be30(dt)** → integrate 0x5001c0 every root → contacts/solve loop. 0x47be30 walks `game+0x54` (list head of the `+0x74` sentinel), deletes dead nodes (`node+0x2c == 0`, `game+0x218--`), else `Game__driveCallback` 0x47be90(dt, node+0x24):
```
typed = obj.typed(0xa0000000)
if (typed+0x4c == 0) { if (typed+0x50) call script method "drive"(dt) }      // scripted objects
else T = typed+0x44 .typed(0x80000000); if (T+0xc) T+0xc->vf[0x30/4 = 12](typed+0x4c /*car*/, dt)
```
= `Vehicle__update(car, dt)`. Mode 8 is the same shape: `Game__runControlCallbacks` 0x47bc20(ctrlAcc) once per `Game__physicsTick` 0x47e050 before the sub-steps → `Game__controlCallback` 0x47bc80 → native `vf[0x2c/4 = 11]` = `Vehicle__updateControls` 0x498300 (player axes via `0x47c640(ctrl, 0x43/0x19, axis)`), or script `"control"`.

**Wheel/suspension objects (createCar 0x4992e0 wheel loop, `i < M+0x1fb4`, template `M+0x2070 + i·0xc8`).**
- Spring weight `M+0x1f34`: if ≤ 0, `= 1 / invMass(car+0x13ec)` (the part_comp compound after `Part__initInstance` ran `PhysBody__updateCompoundMass` on its bones), set once per car type, before the loop.
- Per wheel: `W+0x38 = base`, `W+0x3c = travel`, `W+0x40 = damping ratio` (cfg `spring base travel ratio`, e.g. Axis `0.290 0.100 0.050`), `W+0x50 = base + travel`, `W+0x54 = base + 0.1`, `W+0x58 = −1`, `W+0x60 = r`, `W+0x5c = base + travel + r`, `W+0x74 = 0.015`, hub `W+0x78 = (shape COM − child offset) + template pos`, `y += W+0x5c − W+0x60`.
- `S = new c_triggerspring(0x104)` ctor 0x4915d0 → `c_triggerspring__init` 0x4f3f10(car, car_root, &mount, (0,−1,0), L = W+0x5c, W+0x64): **sensor capsule child body** `createBox(0.01, 3, L/2, 0.05)` at `mount − (0, L/2, 0)`, flag `|= 0x40`, `addChildBody` 0x4f78d0 under car_root, `S+0x94` = that sensor, `addController` 0x4f7960 (world controller list), `S+0xf0 = S+0x50`, `S+0xb4 = S+0x50² + 0.1`.
- Spring constants: **`W+0x44 = |g| · M+0x1f34 / M+0x1fb4 / W+0x3c`** (static sag = travel), `W+0x48 = W+0x44 · W+0x40`, `W+0x4c = 0.7 · W+0x48`, then `Suspension__configure` 0x4f4280(k, bump, W+0x5c, r + W+0x54, r + W+0x58, rebound, r) and `S+0xf0 = template + W+0x38`.
- Optional `"wheelbone"` child (template wheel shape ≠ 0, flags `|= 0x8020`) and `"a_arm"` children, then after the loop `PhysBody__updateCompoundMass` 0x5032d0(car+0x13ec), which recurses into the parent (car_root).

**Who writes the ray hit (`S+0xb4/+0xb8/+0xc4/+0xd0/+0xa8/+0xac`)** — resolves the open question above. The sensor capsule (flag 0x40) meets the ground in contact generation (`Collide__bodyVsStatic` 0x4fd910 → `PhysicsWorld__addTriggerContact` 0x502d60, notes/11 §4), which queues event **0x20** with a temporary `c_collision` payload to the car's native handler `type_car__vf14` 0x4a2e60: `for i < M+0x1fb4: if (c_triggerspring__onSensorContact 0x4f3d70(ECX = W_i+0x30, c)) { car+0x70 |= 0x100000; break; }`. 0x4f3d70:
```
if (!S+0x94) return 0
side = (c+0x48 == sensor) ? 0 : (c+0x50 == sensor) ? 1 : return 0;  o = 1 - side
if (root(c+0x48+o*8) == root(sensor)) return 1                       // own car parts
p = c+0x74 + o*0xc  (contact point on the other body);  d2 = |S+0x6c..74 (mount) − p|²
if (S+0xf0 − sqrt(d2) > S+0x68 /*radius*/) return 1
if (d2 < S+0xb4) { S+0xb4 = d2; S+0xd0 = ref(other body); S+0xb8 = p; S+0xc4 = c+0x8c (normal);
                   S+0xa8 = face material index (c+0x60 → *idx), S+0xac = ref(other.def+0x90 → +0xb0 + idx*0xc4) or S+0xa8 = 0 }
return 1
```
Corrections (re-decompiled 2026-10-24, card 156/157 RE): `d2` uses the **sensor-side** point `c+0x74 + side*0xc` (= pA, the crossing point in the sensor body frame, from 0x4fd110 via 0x4af9a0), while `S+0xb8` stores the **other-side** point `c+0x74 + o*0xc` (pB, the world crossing point). The mount `S+0x6c..74` is `(0, L/2, 0)` in the sensor frame (0x4f3f10), i.e. the capsule top = the car_root mount, so `sqrt(d2)` is the length along the spring axis. `S+0x68` is the wheel radius (`Suspension__configure` 0x4f4280 p7 = r; 0x4f3f10 first sets it to L): the gate rejects hits more than `r` above the previous length `S+0xf0`. `S+0x50` = configure p4 = `r + W+0x54` = src `max_ray()`; since step 2 of the tick treats `S+0x50² ≤ S+0xb4` as a miss, only hits with `d < max_ray` count. The capsule length stays the createCar-time `L = W+0x5c` (configure never resizes the sensor body). The segment-vs-face test that produces the contact is 0x4fd110 (notes/19 §7 "Sensor segment vs PHYS faces"). Event 0x20 is dispatched synchronously (notes/18 §1), so the hit is written during the collide pass and consumed by the next tick's controller.
The next `runControllers` turns `S+0xb4` into the length/load (notes "Suspension object" above) and resets it to `max² + 0.1`. src's `vehicle::Car::update_suspension` replaces this sensor/trigger round trip with a direct `raycast_body` against `Car::ground` (INVENTED equivalent; same nearest-hit-along-the-spring semantics, no one-step event delay).

**`PhysBody__updateCompoundMass` 0x5032d0(body)** (skip if `def+0xb4 & 0x100`; `body+0x58 & 0x8000` → only the shape inertia via 0x5008c0): over children (`+0x24`/`+0x28`) with `invMass ≠ 0`: running mass-weighted COM of the child offsets (`child+0x1c` record `+4..+0xc`) and parallel-axis inertia (point-mass terms + each child's inertia rotated by its offset matrix `+0x20`, 0x566330); the body's **own shape mass is not counted**. No massive child → `invMass = 0`, `def+0xb4 |= 4` (static). Else `def+0x2c = 1/M`, `def+0x60..0x8c` = inertia, inverse 0x566830, `def+0x98 = −0.005·M`, `def+0x9c = −0.01·M`; if the COM offset² > 1e-5: subtract it from every child offset, move the body's active pose by `R·com` (copied into the other state) and the body's own offset record; `body+0x18 = 0.1 / invMass` (1000 if static); recurse `0x5032d0(body+0x20)` when the body is itself a child. For the Axis (`_main_200S.cfg`) the 17 `body` lines sum to 1180 kg; wheel `type` lines are `10.000 sphere 0.315`.

**src today.** `vehicle::Car` (src/vehicle/vehicle.*) implements `Vehicle::update` §1-§6 (+ the trigger-spring solve) but nothing instantiates it; `game::CarData` holds the `part_comp`/`car_root` bodies (mass 1 each, no compound mass, no wheels/sensors) and `WheelData` records from the cfg (`fill_car`: spring line 3rd value and `W+0x44/48/4c` not derived); `natives::advance_sim` calls `World::step` per sub-step with no drive hook. Cards: 145 (compound mass + spring constants), 146 (drive hook: woken car rests on its wheels).

## Control path: player input -> car inputs, once per physics tick (2026-10-24, cards 147/148 RE)

**Who drives which car.** A script `Controller` (system:0x32, `typeof 8`, rsd `native player`, Controller.java ctor `createNativeInstance(parent, 0x32, params, "controller")`) is a native **`_type_player`** object; its axis table (notes/14 §1.5, `C`) is embedded at `player+0x1c` (`type_player__vf15` 0x4d28d0 case 0x1b returns `player+0x1c`; 0x565b60 = `Controller::getAxisVal` with ECX = `player+0x1c`). Freeride: `Track.enter` (Track.java:814) does `player.car.command("transmission "+Config.player_transmission)` (Vehicle.setDefaultTransmission, Vehicle.java:197-205), `player.controller.command("controllable " + player.car.id())` (Track.java:872), then `player.controller.reset(); activateState(ControlSet.DRIVERSET = 1)` (Track.java:1048), which `user_Add`s the group-1 rows of `save/controls/active_control_set`.
- `type_player__vf14` 0x4d2d20 code 0x10 `"controllable %d"`: ignore if the id is already in the player's list (`player+0x2310`, count `+0x230c`, 0x10-byte ResourceRefs, id at `+8`), else `0x47f200(id,1)` (ref) → **0x4d2ab0(player, ref)**: (camera/hide/render events when `player+0x218c` set) then `Game__queueEvent(car, 0, 0x10, "activate 0x%x 0x%x 0x%x" (player id, player+0x218c, player+0x219c))`, `"damage_multiplier %.3f"` (player+0x21c4), and to the player itself `"control 0x%x"` (car id; adds the car to `+0x2310`, `+0x2424` = its index if `getInfo(7) == 5` vehicle). vf14 returns 1 for every code (so the Controller script never sees a command).
- `type_car__vf14` `"activate"` (0x4a3360; scan `"%*s %d %d %d"` 0x59b438, the custom scanner reads the 0x-prefixed ids): `slot = 0x4a1470(car, pid, 0, +1)` (first slot whose `car+0x158 + s·0x4a8` == pid, cyclic over `M+0x17ec` slots; -1 none). Found: rebind `car+0x150+s·0x4a8` = ref(player) (0x51fa40), `+0x160` = ref(camera id), `+0x170` render-instance (0x51f9b0), `getInfo(player, 0x1a, car+0x1a0+s·0x4a8)`, `0x49c800` (split-screen flag `car+0x1f94` when >1 slot has a camera), `0x4ad770(car, s)` (camera/HUD gauges for the `M+0x17f0` slot). Not found: `s = 0x4a1470(car, 0, 0, 1)` (free slot), none → return; same binds. Slot record `R_s = car+0x150 + s·0x4a8`: `+0` player ref (`+8` id, `+0xc` RI), `+0x10` camera ref, `+0x20` render ref, `+0x50` viewport/HUD ref, `+0x60` (car+0x1b0) only written by createCar's zero-init (0x499c08): non-zero blocks control (UNRESOLVED meaning), `+0x64` (car+0x1b4) = axis 7 look value.
- Slot count/driver from the **car cfg**: keywords `driver` and `controller` (table 0x5da758/0x5da760) both → **0x48dee0**(keyword, M): `if (M+0x120 == 0 && M+0x17ec < 4) { if (count == 0) M+0x17f0 = 0; if (keyword == "driver") M+0x17f0 = count − 1; M+0x17ec = count + 1; parse the camera record M+0x1858 + count·0x90 }`; `nodriver` 0x48e070: `if (M+0x120 == 0) M+0x17f0 = −1`. Defaults (0x49b34e): `M+0x17ec = 0`, `M+0x17f0 = −1`. Axis `_main_*.cfg:105` `controller 0 1 2 3 4 5` → 1 slot, `M+0x17f0 = 0`. `M+0x17f0` = **the slot whose player drives** (updateControls) and gets the HUD.
- **Engine on at create** (resolves card 146's INVENTED): createCar 0x49a4ef `if (M+0x17f0 >= 0) { if (!Steam.neutralize flag DAT_007a75df (0x4e4d40)) car+0x70 |= 0x4000 else &= ~0x4000 }`, then sleep (0x506d90). `car+0x1e6c = −1.0` at create (0x499511): 0x4b04a0 zeroes throttle while `car+0x1e6c ≥ 0`.
- `"transmission %d"` (0x4a37f0): `flags &= ~0x30; if (tr & 1) { flags = tr&2 ? flags & ~0x40 : flags | 0x40; flags |= tr&4 ? 0x20 : 0x10; car+0x1fa8 = car+0x1df0 } else { tr&2 ... (manual-clutch branch, not read) }`. `Vehicle.TRANSMISSION_*`: 0 manual, 1 auto (→ 0x50), 2 manual clutch, 5 semi-auto (→ 0x60). Config.player_transmission default 1. Flag consumers in updateControls: `0x10` → auto gearbox 0x497930, `0x20` → 0x4ad480 (semi-auto), `0x40` → auto clutch 0x4977a0.

**Control list.** `Game__physicsTick` 0x47e050: `ctrlAcc += step; if (ctrlAcc > 1e-4) { Game__runControlCallbacks 0x47bc20(ctrlAcc); ctrlAcc = 0 }` **before** the sub-step loop. 0x47bc20 walks `game+8` (mode-8 list; dead nodes `+0x2c == 0` deleted, `game+0x214--`), `Game__controlCallback` 0x47bc80(dt, node+0x24): typed `+0x4c` == 0 → script `"control"(dt)` if `+0x50`; else `T+0xc->vf[0x2c/4 = 11](car, dt)` = **`Vehicle__updateControls` 0x498300** (ECX = M). Every car is on the list (vf15 GII 8 → 1).

**`Vehicle__updateControls` 0x498300(car, dt)** (`e = car+0x2050`, `P = R_{M+0x17f0}`, `ax(v)` = `*Game__getInfo(P, 0x43, v)` float, `ai(v)` = `Game__getInfo(P, 0x19, v)·(1/127)` (player vf15 returns `int(val·127)`), both = `Controller::getAxisVal(v)` of the player unless `player+0x10 & 2` (then the replay/net array `player+0x21dc[v]`)):
```
old_steer, old_thr, old_brk, old_clu = car+0x1ddc, +0x1de4, +0x1de8, +0x1df0
if (car+0x2054) return                                  // traffic AI car
if (!(tear car+0xb4 > 0)) { handbrake=0; push/lift/roll(+0x1f64/68/6c)=0; brake=0; throttle=0 } // -> tail
else if (M+0x17f0 < 0 || P+0x60 != 0) { horn +0x1e00 = 0; +0x1f64/6c/68 = 0; brake = 0; throttle = 0 }
else if (P player id (+8) == 0) { throttle = brake = 0; +0x1f68/6c/64 = 0; horn = 0 }
else {
  steer +0x1ddc = ax(1 TURN_LEFTRIGHT); throttle +0x1de4 = ax(28); brake +0x1de8 = +0x1dec = ax(29)
  handbrake +0x1df4 = ax(48); clutch +0x1df0 = ai(49); nitro +0x1df8 = ai(31)
  shift = ai(50 GEAR_UPDOWN); horn_ax = ai(54 HORN); gearset = ai(51 GEAR_SET)
  aero list car+0x2144: entry+0x30 = ax(entry def+2 (short)) when set
  slot objects car+0x2190: v = ax(obj def+0x16c); if (v != +0x2c or (v != 0 and def+100 & 0x80000)) and (car+0x70 & 1): PhysBody__wakeRoot (0x498599); +0x2c = v
  flags 0x1000 (speed limiter car+0x2258 = crime/limit speed: throttle scaled to 0 between 0.5·lim and lim), 0x2000 (steer assist, car+0x2268/226c)  [not ported]
  flags 0x40000 ("brake" command): throttle 0, brake 1, handbrake 1, nitro 0, shift 0, gear 0
  horn: +0x1e00 = (+0x1e00 + 1) & 3 on each edge of horn_ax across 0.1 (odd = pressed)
  R_s+0x64 (car+0x1b4 + s·0x4a8) = ai(7 LOOK_LEFTRIGHT)
  wheel average steer/slip -> car+0x2278/+0x227c ; getInfo(P, 0x44, {1, ...}) force feedback (player vf15 0x44 -> 0x5660c0/0x566140)
  steer help: if (!(flags & 0x200) && e+0xa34 > 0 && M+0x1330 > 0) blend steer toward the slip angle (M+0x132c..0x1340, DAT_005fe320), then steer = old + (steer − old)·min(10·dt, 1), halve a sign flip   [not ported]
  gear shift (latch flag 0x400000, cleared while |shift| ≤ 0.1; now = world time *PTR_005dadf0):
    shift >  0.1 and latch clear: latch; if (e+0x20 gears) { gear 7 -> 0, +0x1e68 = now+0.2 ; gear < gears -> gear+1, +0x1e68 = now+0.3 ; 0x4b0190(e, car, 1) }
    shift < −0.1 and latch clear: latch; if (gears) { gear 0 -> 7, +0x1e68 = now+0.3 ; gear != 7 -> gear−1, +0x1e68 = now+0.2 ; 0x4b0190(e, car, 0) }
  gear set: if ((DAT_005fe31c || gearset > 0) && |+0x2280 − gearset| > 0): +0x2280 = gearset; g = min(int(gearset·7 + 0.5), 7); if (g != gear && (0 ≤ g ≤ gears || g == 7)) gear = g, 0x4b0190(e, car, 0)
  +0x1f74 = steer; +0x1f70 = throttle − brake; +0x1f6c = 0; +0x1f68 = ai(39 CHEAT_1); +0x1f64 = ai(40 CHEAT_2)
  if (+0x1f68 != 0) for each hinged part car+0x1518 with bit0: 0x4b10c0(0), clear bit0, its body +0x58 |= 0x20000
}
if (flags & 0x40000) { brake = 1; handbrake = 1; nitro = 0; gear = 0 } else goto tail_keep_throttle
throttle = 0                                                   // (also the two no-control branches)
tail_keep_throttle:
if (flags & 0x10) 0x497930(e, car, dt)    // auto gearbox     [card 149]
if (flags & 0x20) 0x4ad480(e, car, dt)    // semi-auto        [card 162, see below]
if (flags & 0x40) 0x4977a0(e, car, dt)    // auto clutch      [card 149]
0x4b04a0(e, car)                          // shift timers
flags &= ~0x03000000
k = min(10·dt, 1): throttle = old_thr + (throttle − old_thr)·k; brake = old_brk + (brake − old_brk)·k
clutch = old_clu + (clutch − old_clu)·min(33.333336·dt, 1)
flags 0x80 -> 0x493fb0, 0x100 -> 0x494110, 0x4000000 -> 0x4a1000      [not traced]
if (!(flags & 1)) { if (|steer_copy − steer| ≤ 0.001) return }
else { B = root(car+0x13ec); if (B+0x58 & 0x80 asleep && |throttle| ≤ 0.01 && |steer − steer_copy| ≤ 0.01 && gear == gear_copy +0x1e60) return
       flags = (flags & ~1) | 2; if (B asleep) PhysBody__wake 0x507060(B) (0x499299, recursive over children) }
0x4aa830(car)      // per wheel W+0xe4 steer angle = e+0xa20·steer·W+0xc8 (Ackermann e+0xa30 via atan), wheel matrices, steering-wheel bone; flags |= 0x100000
steer_copy +0x1de0 = steer
```

**Semi-auto `0x4ad480(e, car, dt)`** (flags `0x20`, transmission 5 = `TRANSMISSION_SEMIAUTO`; decompiled + asm 2026-10-24; `dt` unused). Constants: 0x593838 = 0.2, 0x59ab98 = 9.549296, 0x3fa66666 = 1.3 is pushed to 0x4b19a0 but never read.
```
if (!(car+0xb4 tear > 0) || !(flags & 0x4000) || (flags & 0x40004)) return
if (e+0x20 gears == 0 || gear(+0x1e5c) == 7 || gear == 0) return      // never engages from neutral / reverse
if (!(|0x4b19a0(e, car)| < 0.2)) return                              // mean driven-wheel kappa
w = car+0x1e78 shaft * car+0x1e0c ratio[gear] * e+0x44 rearend ; r = w * 9.549296
T = (e+0x1c RPM_limit <= r) ? 0 : 0x4a2dd0(r, e)
if (r >= 0) { 0x4b06b0(e, car, r, T, w) /*upshift*/ ; 0x4b05a0(e, car, r, T) /*downshift*/ ; return }
if (car+0x1e74 > 0) return
gear = 1 ; car+0x1e68 shift_end = now + 0.2 ; 0x4b0190(e, car, 0)
```
Differences to the auto box 0x497930: no `shift_end > 0` gate, no standstill/brake-reverse logic, a slip gate, and r < 0 goes to gear **1** (auto: 0). All callees are ported in src/game/car.cpp (upshift/downshift/drive_slip/shift_timers/torque_at).
`car+0x70` bit0 is set by `Vehicle__update` §0 (0x49c984: body asleep → `steer_copy = steer`, `gear_copy = gear`, `flags |= 1`), cleared here (input wakes the car). So in freeride the player's car wakes on the first throttle/steer/shift input (no script `"wakeup"`).
- **0x4b04a0(e, car)** shift timers: `if (car+0x1e6c ≥ 0) throttle = 0`; `t = car+0x1e68`: if `t > 0`: `t ≤ now` → `t = −1, e+0x10 shifting = 0`; else `(flags & 0x30) == 0` (manual) → `throttle = 0, clutch = 1, e+0x10 = 1`, else `clutch = 0, e+0x10 = 1`. Then `car+0x1e70/+0x1e74` (set by 0x4b0190) expire to −1 and, when both < 0, `0x4b03c0(e, car)` + `e+0x10 = 0` (0x4b03c0 not traced).
- **0x4b0190(e, car, up)**: `up == 0`: `+0x1e74 = now+0.3, +0x1e70 = now+0.7`, shift-down SFX if `e+0x8b0`; `up`: `+0x1e70 = now+0.5, +0x1e74 = now+1.0`, shift-up SFX if `e+0x8a0` (`Game__postEvent` 0x4eb5c0); then gearstick bone `"gsbone"` pose from table `0x5da948 + gear·0xc` + `e+0x9c0..0x9c8` when a cockpit render exists.
- **Gear index**: `car+0x1e5c` 0 = neutral, 1..`e+0x20` forward, **7 = reverse**; `Vehicle__update` reads `car+0x1e0c = e+0x24 + 4·gear` (0x49d2e1), Chassis.java:718-725 defaults `ratio[0]=0, [1..4]=3.5/2.7/1.9/1.3, [7]=−4.0`.
- **Auto gearbox 0x497930(e, car, dt)** (flags 0x10, the default player transmission) only acts while `car+0xb4 > 0`, engine on, not braked/frozen (`0x40004`), not mid-shift (`+0x1e68 ≤ 0`); standstill (`|shaft +0x1e78| ≤ 1`): `throttle − brake > 0.1`, `gear == 0`, `e+0x20 ≠ 0` and `rpm +0x1e80 > 3·idle(e+0xf0)` → `gear = 1`, 0x4b0190(1); `< −0.1` swaps throttle/brake and engages 7 above `3·idle`; moving: up/down-shift helpers 0x4b06b0/0x4b05a0 (not traced). **Auto clutch 0x4977a0** (flags 0x40): clutch from rpm vs a throttle-dependent launch rpm `((|ratio/e+0x28|·thr·0.56 + 0.24)·(e+0x14·0.10471976 − idle) + idle)`, rate 33.3/s closing, 200/s opening, `car+0x1fa8` = previous clutch.

**Auto gearbox / clutch, fully traced (2026-10-24, card 152 has the pseudo code).**
- `0x497930(e, car)` (dt unused) uses `car+0x1e64` = brake latch (set when braking in a forward gear while moving; standstill clears it once `throttle − brake > −0.5`). Standstill (`|+0x1e78| ≤ 1`): pedal `> 0.1` in neutral engages 1st when `rpm +0x1e80 > 3·idle`; `< −0.1` swaps throttle/brake and engages reverse 7 above `3·idle`; no pedal in gear with `rpm < 1.2·idle` → neutral. Moving: engine-side `r = +0x1e0c·shaft·e+0x44·9.549296` rpm, `T = r < RPM_limit ? curve(r) : 0` (`0x4a2dd0` = src `sample_curve`); braking in gear ≥ 2 → downshift only; accelerating in gear ≥ 1 needs `|0x4b19a0| < 0.2` (mean `W+0x14c` κ over wheels with `S`, drive base `W+0xcc ≠ 0`, load `S+0xf4 > 0`), then upshift 0x4b06b0 + downshift 0x4b05a0; `r < 0` → neutral + `+0x1e68 = now+0.2`.
- `0x4b06b0(e, car, r, T, w)` upshift: needs `+0x1e70 ≤ 0`, clutch locked (`|+0x1e80 − w| ≤ 0.1·+0x1e80`, or `|G| ≤ 0.01`), `gear < gears`; next gear `Gn = e+0x28+4g`, `Tn = curve(r/G·Gn)`; shifts when `r > min(0.95·maxRPM, 0.95·RPM_limit)` or `G·T < Gn·Tn` (`+0x1e68 = now+0.3`). `0x4b05a0(e, car, r, T)` downshift: `+0x1e74 ≤ 0`, gear > 1, `Gp = e+0x20+4g`, shifts when `r/G·Gp < th` and `1.15·G·T < Gp·Tp` (`+0x1e68 = now+0.2`).
- `0x4977a0(e, car, dt)` auto clutch: predicted rpm `pred = 2·+0x1e80 − +0x1fac` (`+0x1fac` = previous rpm), launch `((|G/e+0x28|·thr·0.56 + 0.24)·(e+0x14·2π/60 − idle) + idle)`; open (1) when shaft-side engine speed `> 1.2·min(maxRPM, RPM_limit)` or `pred ≤ idle`; else `c = 1 − (pred − idle)/(launch − idle)` while engine faster than the shaft and `pred < launch`; only ever raises the clutch, rate 200/s opening / 33.3/s closing from `+0x1fa8`.
- `0x4b03c0` (shift end in 0x4b04a0) only poses the driver hand bone (`lhbone`/`rhbone`, `e+0x930` sign) on the cockpit render: cosmetic.
- Debug (scratch build): vmboot's `rpm 740` is `rpm_raw` in rad/s (7066 rpm, limiter); driven wheels spin (κ ≈ 1.8) with driven loads 1236 N vs 2601 N undriven, sum 7.7 kN vs 11.8 kN weight → card 153.

**src today.** `input::Controller` per Controller resource (`natives::controller_of`, natives.cpp:5095) has the mapped axes; the player resource never handles commands (`Runtime::queue_event` ports only `game::car_command`), so `"controllable"` and `"transmission"` are dropped; CarData has the input fields (`throttle`, `brake_input`, `handbrake_input`, `steer_input`, `clutch_input`, `nitro_input`, `gear`, `gear_copy`, `steer_copy`, `horn_id`) but nothing writes them, and `vehicle::Car` keeps its own copies (`throttle/brake/handbrake/steer/nitro/clutch/gear`) and its own engine block (`Engine::torque_curve` empty, `ratios[]` placeholder defaults) that nobody syncs from `CarData` (`torque_rpm/torque_nm`, `ratio[8]`, `rearend_ratio`, `max_steer`, `idle_omega`...). `advance_sim` has no control-list call. Shipped key map (`save/controls/active_control_set`, group 1, device 0 SysKeyboard, DIK codes): throttle 28 ← 200 (Up), brake 29 ← 208 (Down), steer 1 ← 203 (Left, +1) / 205 (Right, −1), handbrake 48 ← 57 (Space), gear 50 ← 30 (A, +1) / 44 (Z, −1), clutch 49 ← 46 (C), nitro 31 ← 49 (N), horn 54 ← 35 (H). Cards: 147 (player binding, engine-on, transmission flags, CarData→sim bridge), 148 (control list + updateControls input/shift path + input wake); next 149 = auto gearbox/clutch (0x497930/0x4977a0 + 0x4b05a0/0x4b06b0).

**Card 147 first run, root causes (2026-10-24, verified in a scratch worktree).**
(1) `Chassis.getWheel` 0x4b4150 resolves its wrapper class with `JVM__getClass("java.game.parts.WheelRef")` (0x55add0, load on miss); src used `Vm::find_class`, so every `Chassis.updatevariables` aborted at line 340 since forceUpdate was ported (error ignored, pre-existing on main). With `get_class` the call completes and Chassis.java:346 `setInstantCenter(10000 x6)` lands on all 4 wheels. (2) src `g_players` (native-player set) was filled before `g_rt` was set and wiped by the next `car_data()` -> `reset()`. With both fixed, Track.enter's `controllable -2147482363` (negative decimal id) reaches the player handler, `activate 0x80000011` binds slot 0 of the Axis car (0x80000505), flags 0x4050 (engine on + transmission 1). (3) Open gap: src never sets `Slot::attached` (no part install), so `partOnSlot` is null, Chassis finds no `Block`, and the engine torque curve stays 1 point of 0 Nm. Exe install path traced: notes/13 §15 (type_part__vf14 0x4c4400 "install", search 0x4c96c0/0x4c9af0/0x4c89f0, compatibility 0x4c9d90, attach 0x4cb1d0, commit 0x4c7d20); card 150.

**Card 153 traction RE (2026-10-24, Ghidra + scratch debug build of HEAD 421e928).**
- The vmboot car is **Duhen SunStrip 1.8 DVC** (`cars/racers/Duhen_data/scripts/_main_SunStrip_1_8_DVC.cfg`, body lines 742.297 kg; the used-dealer browse picks it for its D18V block), not the Axis (1160 kg). car_root = 782.337 kg, weight 7674.7 N; at rest the spring loads sum to **7677 N** (= weight). The "11.8 kN" in the old card 153 was the Axis mass.
- Static split is what the cfg compound gives: COM (0, −0.262, +0.239) from the body lines + 4×10.01 kg wheel children; hubs z −1.333 / +1.235 → expected 1489 N front (driven, FWD) / 2349 N rear per wheel; measured 1461/1477/2375/2364. The tyre/suspension maths is not the fault.
- **Root cause 1, missing part masses.** `Part__commitInstall 0x4c7d20` joins the installed part's body under the target part's body (`0x501d00` unlink, `0x4f7b20` addChild, target = the target body or its compound parent when `!(+0x58 & 0x8000) && +0x20 && +0x1c`) and re-runs `PhysBody__updateCompoundMass 0x5032d0(target)`, which recurses up to car_root. src `part_attach` (car.cpp, card 150) skips the physics join. Scratch tally for the SunStrip: 53 installed parts with a body = **460.1 kg**, COM z ≈ −0.49 (engine, gearbox, front suspension), so the exe car is ~1242 kg with COM z ≈ −0.03 → ~3.0 kN on each front wheel instead of 1.47 kN.
- **Root cause 2, the compound has no shape inertia.** `0x5032d0` adds each child's own inertia (`def+0x60`, rotated by the child offset matrix, 0x566330) to the parallel-axis terms; src sums point masses only, and mesh bodies have no mass props at all (`compute_mass_props` TODO; exe `Poly__massProps 0x508d20` Mirtich integrals, notes/11 §1). The roll inertia is then ~30 kg·m²: with the real spring/damper values (below) the explicit spring step goes unstable (the car bounces off the soup within 20 frames). Scaling the inverse inertia by 0.2 in the scratch build made it settle (loads 1460/1503/2377/2334).
- **Root cause 3, wheel parts never reach their WheelRef.** `Part.getWheel()` (Part.java:733) = `Chassis.getWheel(getWheelID())`; `getWheelID` maps `P+0xd0` (101..400 → (v−101)%10, else −1). `P+0xd0` is written by 0x4c7d20 = `0x4c8b10(P, 0)`: follow P's first installed slot (flags & 1, `+8` set) to its target part and recurse with the mate slot id (`+0x10`); at a part with no installed slot (the chassis) return the type-slot node (`M+0x116c` list, `+0x60 == slot id`) field `+0x64` as ushort, 0 if none. `+0x64` is the cfg **`slottype`** value (keyword table 0x5da5d0 → handler 0x4c0190: `last_type_slot(M+0x1194)+0x64 = scan("%d")`, only when `M+0x120 == 0`). Chassis slot 311 (FL spring) has `slottype 301` → wheel 0. src never sets `wheel_id` and does not parse `slottype`, so every Spring/ShockAbsorber/Wheel(tyre)/Brake/Suspension `updatevariables` sees `whl == null`: zero WheelRef setForce/setRestLen/setMinLen/setMaxLen/setBearing/setRollRes calls in a full vmboot run. With a scratch wheel-id hack 43-44 calls each arrive: SunStrip front k 59841 N/m, damping 3900/3900, rest 0.266, min 0.178, max 0.266, radius 0.329, friction 1.77; rear k 53955, damping 3600 (vs cfg-derived 18205 / 910 / 0.39 / −1 / 0.39 / 0.315 / 1.0).
- **Root cause 4, sim wheels are a copy.** `vehicle::Car::wheels` are filled from `WheelData` once in `create_car_bodies`; the WheelRef natives write `WheelData` only (exe: one struct + `Suspension__configure 0x4f4280` re-run by every length/force setter). Needs the same per-tick bridge copy the engine block already has (`drive_cars`).
- Wheelspin itself (κ ≈ 1.5-1.8 at full throttle in 1st) follows from 1: 159 Nm × 3.23 × final 5.5 / 2 / 0.315 ≈ 4.5 kN per front wheel vs μ·load ≈ 1.5 kN; with the part masses and the tyre friction 1.77 it is ≈ 5.3 kN of grip.
- Side note (resolved 2026-10-24): src sphere invI = 2.5/(r²·m) matches 0x505760, which handles every type-1 sphere; 0x5056a0 (R not squared) is only for type 7 / mesh-less type 6 from createBox 0x4f7d60 (notes/11).
- Cards: 153 (compound mass + inertia: installed parts and shape inertia), 154 (wheel id + WheelData → sim sync).

**Card 153 implemented (2026-10-24).** `compute_mass_props` SHAPE_MESH runs Poly__massProps 0x508d20 (Mirtich 0x508820, doubles) and returns the centroid; `game::rebuild_compound` (0x5032d0) re-sums car_root from the car cfg body lines, the wheelbone sphere + 0.01 kg sensor per wheel and every installed part body (0x4c1a10 pose), called by `create_car_bodies` and by `part_attach` after 0x4c7d20. vmboot: 76 installed parts with cfg bodies, **1280.294 kg** (742.297 + 40.040 + 497.957), I diag 1471 / 1666 / 333 kg·m²; at rest loads 3084/3249 front (share 3170) and 3056/3171 rear (share 3110), sum 12560.5 N vs weight 12559.7; Up-held speed 1.75 m/s, driven kappa 0.494 (was 1.8). The 76/497.957 vs the RE tally 53/460.1 is the recursive slot walk including nested engine parts (the flat and nested compounds are equivalent); the card's ~1242 kg estimate was direct children only.

## Top speed 103.85 m/s in vmboot = free fall through the map, not missing drag (2026-10-24, card 159/160 RE)
Scratch debug build of HEAD c3c75ae (copy of src in the session scratchpad, prints only), freeride Duhen SunStrip 1.8 DVC, Up held 900 frames:
- f0-f240: gears 1->4, the car drives **down a ~14 deg slope** (PHYS normal (-0.29, 0.93, -0.21)) at 23 m/s horizontal, 4 wheels loaded. f242: the soup turns flat (n = (0,1,0), y = -26.05). The leading wheels (0/1, -z = engine-bay side = front) compress to `ray_len` 0.003 (loads 18-24 kN), then the mount passes the flat face: the one-sided sensor segment (0x4fd110, mount-L..mount) no longer crosses it -> no contact (f245 wheel 0, f250 wheel 1, f255/f260 rear). From f260 nothing supports the car: `car_root` has `SHAPE_NONE` (car.cpp ~1104 "shortcut: no compound child collision yet") and the cfg body hulls are mass-only compound children (`rebuild_compound`), so the body passes the floor; at f288 no soup face lies below the car at all and it falls (f840: vel (-5.5, -92.5, -22.4), pos.y -476). `final speed` = |vel| = 103.85 is mostly vertical; the auto box shifts to 5th while the driven wheels spin free at the limiter (135 rad/s).
- In the exe the chassis hulls (cfg `body` lines, shape kind 6 .scx) are compound children of part_comp and collide with the soup: `Collide__bodyVsTriangles` 0x4feb20 recurses over children (notes/19 §7), per face swept-sphere/face-AABB test `0x4f9860`, SAT box-vs-triangle `0x4fc450`, cached triangle Poly `0x50f000` (built with `Poly__buildFromFaces` 0x50e6d0), candidate `0x4fbda0` (non-trigger body -> `body_dummy_candidate` into `DAT_006028f8`), then `Collide__candidatesTOI` 0x4fc7e0 (V-Clip 0x50d230 hull vs triangle, 10-step TOI bisection on prev->cur state 0x506c00, deep penetration -> hull vertex with min distance along the face normal, 0x4fa550 EE-feature sign, margin, `addContact` 0x502830: one contact per body, earliest TOI). All decompiled 2026-10-24 (pseudo code in cards 159/160). V-Clip itself is documented in notes/11 §2-3; src has no Poly/V-Clip (shape.cpp only primitive pairs; `pair_unsupported_mesh` skips mesh-vs-mesh).
- **Drag (§7) is not what limits this car.** Exe drag at 0x49fb48..0x49ff40 (re-read: `e+0x91c > 0` gate via `comiss 0.0`, `|v|² > 1.0`, `F = -C·|v|²·v̂`, point velocity 0x5062e0 at `drag_center` e+0x910, effective-mass clamp with world inv inertia `state+0x108..0x130` and `invMass def+0x2c`, 0.5 = `h` factor, `applyForceAt`) is not ported in src (`CarData::c_drag` is read by ch_force_update and never used). But for the stock SunStrip the script's own result is `C_drag = 0`: Chassis.updatevariables sets `C_drag = fully_stripped_drag` (0.48, Duhen_SunStrip_1_8_DVC.java:29) and subtracts `drag_reduction` of every BodyPart on the chassis slots: hood .03, 2 bumpers .10, 2 windshields .12, 2 headlights .02, 4 quarterpanels .20, 2 doors .05, 2 sideskirts .005, 2 taillights .02, trunk .03, targa .0025, pedals .0015 = 0.579 > 0.48 -> clamped to 0 (Chassis.java:670; debug print of the 75 installed parts, slot ids 3..999). The exe runs the same script, so this car has no aero drag there either; drag still must be ported for cars where it is > 0 (other freeride cars printed 0.021, 0.11, 0.17).
- What does limit speed in the exe for this car: the rev limiter (§1, `L = 0` once `rpm` and the predicted `2rpm - last` exceed `RPM_limit` e+0x1c) and the gearing: top gear 5 (`ratio[5]` 0.963, `rearend_ratio` 5.5, `RPM_limit` 6750, tyre r 0.329) -> `v_top = 6750·π/30 / (0.963·5.5) · 0.329 = 43.9 m/s` (158 km/h) wheel-surface speed; body linear drag `-0.005·m·v` (0x4f8460, ported), rolling resistance `p13·Fz·vz·0.01` (ported), engine friction 0x4a1510 (ported) only lower it. Drivetrain efficiency: no such term in 0x49c8e0 (the clutch coupling §3 is lossless up to the `k·0.999` factor, ported).
- Small gaps found on the way (not the cause): `p13` in src is `W+0x74` only; the exe adds the ground material's rolling resistance `gm+0x54` (src `Shape::rolling_resistance` exists, vehicle.cpp comment "ground material has no rollres" is stale). Engine block `e+0xe8` drivetrain inertia 1000 / `e+0xec` wheel inertia 50 are template constants (clone 0x4b0910, "Engine-block template writers"); src defaults match.
- Cards: 159 (Poly build + V-Clip closest features for kind-6 hulls vs a triangle, real-data distance check), 160 (car hull children vs soup: 0x4feb20 + 0x4fc7e0 -> contacts; Done when the auto drive stays on the ground and |vel| <= v_top). Drag §7 + ground rollres: card 161 (written; gm = S+0xac is the per-face material `shape+0xb0 + idx·0xc4` when `idx < shape+0xac`, else the shape itself, 0x4f3d70).

## Script placement of a car: GameRef.setMatrix -> type_car vf7 0x4a99b0 (2026-10-24, card 164)
Why it matters: the career race (Garage CMD_EVENTLIST 133 -> EventList(MODE_AMATEUR) -> eGen CMD_RACE ->
`java.game.Track` + `java.game.CircuitRacing`) places every car with `GameRef.setMatrix` from the map's
`TrackData.getGrid()` data (`Track.applyStartGrid`, Track.java:2901-2941; freeride: `Track.lockCar`
setMatrix(posStart, oriStart), Track.java:1688-1697, then "reset"). The grid is script data of the map
mod (TrackData subclass), not exe. The exe only provides the native below.

Scratch probe (cmd 133 instead of 134, run 2026-10-24): the player car's Resource pos became the grid slot
(40.067 -4.301 22.549) but `car_root` stayed at (0.010 -0.208 -0.061), stacked with 9 other sleeping cars at
the origin; 4 other root bodies fell to y -219 .. -32956. The woken car overlapped the stacked hulls
("Voronoi error (infinite loop)" x105 on stderr), was thrown up (pos.y 51, vel.y -44.5, 0 wheel hits):
the 12 drive-check failures. Cause: src `gr_set_matrix` (natives.cpp) only writes `Resource::pos/ori`.

Exe chain (asm/Ghidra 12.1.4):
- `0x4db950` vm_gameref_setmatrix: reads Vector3 x/y/z, Ypr y/p/r (null -> 0), formats `"ffffff"` (0x56a000)
  and calls `0x4eb3f0`(str) on the GameRef handle.
- `0x4eb3f0`: typed object (0xa0000000 lookup 0x51f4f0) -> its type manager -> `vtable+0x1c` (vf7)
  `(typed+0x4c, str)`. For cars that is `type_car__vf7 0x4a99b0` (vtable 0x59dcd8 slot 7).
- `0x4a99b0`(car, str), ECX = M: `Str__scan(str, "fffffffffffffffi")` = pos3, ypr3, vel3, angvel3,
  steer, wheelspin delta, pitch-add, int dir; setMatrix sends only 6, the rest stay 0.
  - M wheel count `M+0x1fb4` < 4: `pos += R(ypr) * M+0x1f1c` (cfg keyword `center`, handler 0x48de60;
    no shipped cfg uses it, so 0).
  - else, per wheel i<4 (W = car+0x1414 + i*0x2e4): contact point `c_i = (W+0x78, W+0x7c - ((W+0x60 +
    W+0x50) - (|g|*m/n)/W+0x44), W+0x80)` (hub minus rest ray at static compression; |g| =
    |DAT_5dadf0+0x1c4|, m = 1/(def+0x2c) of the car+0x13ec body, which vf7 walks up to its root,
    so it is part_comp, not car_root: UNVERIFIED which mass src keeps there, check card 153 notes). pitch += atan2((y2+y3-y0-y1)/2, (z2+z3-z0-z1)/2);
    roll -= atan2((y1+y3-y0-y2)/2, (x1+x3-x0-x2)/2) (fpatan order checked in asm 0x4a9db6..0x4a9dcd).
    R = ypr matrix (0x5666d0); v = R * mean(c_i); `pos.x -= v.x; pos.z -= v.z;
    pos.y -= min(v.y, -(R*M_center).y)`. So the given pos is where the tyres touch.
  - flaps (car+0x1518, 0x8c records): their offsets to the chassis are kept across the move.
  - `0x4eac50`(pos, R) on the car's body (part_comp, car+0x13ec): `0x4e9730` walks the compound to the
    root and returns the child offset `off` in root frame; root pos = pos - R*off (src: root->pos =
    pos + R*com, part_comp sits at -com), root orientation = R (0x567ea0), copied to both state buffers;
    `0x5030b0` refreshes child poses. Returns nonzero when pos or rotation changed.
  - if moved: root vel (+0x70) = vel arg and angular (0x506ce0) = angvel arg in both buffers (0 here);
    car+0x12c = pos + vel*0.02, car+0x138 = vel; 0x4c6d30 copies vel to the part renders.
  - always: car+0x1e5c/0x1e60 (gear state) = 0, +0x1ddc steer = arg (0), +0x1de4/1de8/1dec/1df4 = 0,
    +0x1e7c (engine omega) = +0x1e78 (shaft) = +0x1e80 = 0; if |+0x1de0 - steer| > 0.001: 0x4aa830 +
    `+0x1de0 = steer`.
  - if moved: per wheel W+0xc0 = 0; sensor S = W+0x30: pose refresh (0x4f4360/0x4f4310), S+0xf4 = 0,
    `S+0xf0 = (W+0x60 + W+0x50) - (|g|*m/n)/W+0x44 - W+0x80*(pitchadd/...)` (pitch-add 0 here) -> the
    gate's previous ray length starts at the static length; car+0x70 |= 0x100000.
  - `if (!(root+0x58 & 0x80)) PhysBody__sleep 0x506d90(root)`: a placed car sleeps until "wakeup"
    (Gamemode.unlockCars, Gamemode.java:774-800, after the countdown).
  - car+0x70 &= ~0x30000 (| 0x10000 / 0x20000 for dir -1 / 1).

Career-map drive failures after the placement port (card 164 finish, 2026-10-24): not the placement.
- Ground bodies: src built a `groundbody` for every type-1 record at package mount, so city, city2,
  test_track, ROC_track, carlot and the garage rooms all sat at the origin together with the race map;
  the car hit their faces on the start straight (contacts against soups with > 75750 faces). The exe
  builds it in `type_ground__vf3` 0x4bade0 when the instance is made resident (0x5195a0) and frees it
  in vf4 0x4bb3d0 (0x4fb2a0 def release; body removal 0x4fadf0 = unlink + contacts + `w+0x54 -= 1`).
  src now creates it on the first script handle bind of the record (INVENTED stand-in for residency)
  and removes it in `Runtime::unload` (scripts call `map.unload()`, Track.java:1132, Garage.java:489).
- Two-sided road: the career map stores the road twice (e.g. faces 59838/59839, normals +y/-y). 0x4fc7e0
  uses the min hull-vertex distance only when V-Clip penetrates (d <= -margin); src took min(V-Clip,
  signed plane distance) on every face, so a hull above the back face got ~-1 m and was pushed down
  through the road. src now follows the d <= 0 rule and skips a face whose front the hull centre is
  not on while V-Clip penetrates (INVENTED: no prev-state TOI bisection).
- Pacejka p13: 0x49ef5a writes slot 13 only when sigma > 1e-4; src's per-tick WheelData -> sim copy
  reset it to the default 0.015 on free-rolling wheels. The written value is now kept.
- Soft rev limiter (s1, `L = (lim - nxt)/(rpm - nxt)` while rpm > lim and falling): the long straight
  holds the SunStrip on the limiter; top-gear engine rpm floats to ~6870 (RPM_limit 6750), 43.98 m/s vs
  the 43.86 m/s computed at RPM_limit. The A1Ring drive never reached the limiter.
- Open: the used car is picked by a time-seeded rand (Math.randomize 0x4dee90); other picks (Axis 200S,
  Einvagen 110 GTK) fail vmboot checks that assume the SunStrip (D18V slot 401, getWheelID) and the
  Axis 200S hull 15 sits ~8 cm below its tyre plane at rest.

## Wheel steer frame: 0x4aa830 -> sensor body matrix -> tyre/spring pass (2026-10-24, card 170 RE)
`0x4aa830(car)` (ECX = manager, called from updateControls 0x498300 when `|steer − steer_copy| > 0.001`, and from
0x4a99b0 placement), per wheel W (`car+0x1414 + i·0x2e4`) with `S = W+0x30 ≠ 0`:
```
d0 = e+0xa20 (max steer) · car+0x1ddc (steer)              // no speed sensitivity here
if |d0| <= 0.001            W+0xe4 = 0
else if e+0xa30 (L) == 0    W+0xe4 = W+0xc8 · d0
else { R = L / (sin d0 / cos d0)                           // 0x570fcb = sin, 0x570e7e = cos (from 0x5666d0)
       R > 0 : W+0xe4 =  atan2(L,  W+0x78 + R) · W+0xc8   // FPATAN(ST1 = L, ST0 = x), W+0x78 = hub x
       else  : W+0xe4 = −atan2(L, −R − W+0x78) · W+0xc8 }
Ry = Euler(W+0xe4, 0, 0) (0x5666d0 into W+0x110)  -> rows (c,0,s),(0,1,0),(−s,0,c)
W+0x110 = W+0x90 (setYpr base) · Ry               (0x566a70: this = param · this)
0x4f4380(S, pos 0, W+0x110): copies W+0x110 into the sensor body's local offset matrix (S+0x94 → +0x1c, +0x20..0x4c)
0x4a7fe0(car) render wheel pose (cosmetic); car+0x124 steering-wheel bone yaw += e+0xa24·steer; flags |= 0x100000
```
Matrix convention (from the W+0x140 transform in 0x49c8e0): world→local is `local_j = Σ_r M[r][j]·v_r`, so the
**columns** are the local axes; `Ry` column 2 = `(sin δ, 0, cos δ)` = wheel forward in the base frame, i.e. `δ > 0`
turns the wheel's +z toward +x (yaw ω_y > 0 for a car rolling along +z).
**Consumers.** Every "R" in Vehicle::update §2/§6 and in the spring solver is the matrix of the body `S+0x94`
(`+0xa4 + 0xd4·state(+0x20c)`), i.e. the sensor body whose local offset is `W+0x110`, so `R = R_car · W+0x90 · Ry(W+0xe4)`:
§2 slip velocity `W+0x140 = Rᵀ·(groundVel − pointVel)` (0x49d5xx loop at pseudo line 648), §6 `v`, the normal
`n_body = Rᵀ·S+0xc4` and the force basis back to world (0x49ef9e..), and the spring Gram-Schmidt forward `R·(0,0,1)`
(§ Suspension step 10; up `R·(0,1,0)` is unchanged by a yaw). src `vehicle::Car` computes `steer_angle` without
Ackermann and uses the chassis `r` everywhere, so steering never reaches the tyres (card 169 measured 0.001 rad/s
yaw at full lock, 8.5 m/s). Steer help (0x498300, on unless cfg `steerhelp ≤ 0` sets flag 0x200, needs
`M+0x1330 > 0`) and flag 0x2000 change `steer` before 0x4aa830 and stay out of scope (card 170).
