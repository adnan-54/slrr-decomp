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
The next `runControllers` turns `S+0xb4` into the length/load (notes "Suspension object" above) and resets it to `max² + 0.1`. src's `vehicle::Car::update_suspension` replaces this sensor/trigger round trip with a direct `raycast_body` against `Car::ground` (INVENTED equivalent; same nearest-hit-along-the-spring semantics, no one-step event delay).

**`PhysBody__updateCompoundMass` 0x5032d0(body)** (skip if `def+0xb4 & 0x100`; `body+0x58 & 0x8000` → only the shape inertia via 0x5008c0): over children (`+0x24`/`+0x28`) with `invMass ≠ 0`: running mass-weighted COM of the child offsets (`child+0x1c` record `+4..+0xc`) and parallel-axis inertia (point-mass terms + each child's inertia rotated by its offset matrix `+0x20`, 0x566330); the body's **own shape mass is not counted**. No massive child → `invMass = 0`, `def+0xb4 |= 4` (static). Else `def+0x2c = 1/M`, `def+0x60..0x8c` = inertia, inverse 0x566830, `def+0x98 = −0.005·M`, `def+0x9c = −0.01·M`; if the COM offset² > 1e-5: subtract it from every child offset, move the body's active pose by `R·com` (copied into the other state) and the body's own offset record; `body+0x18 = 0.1 / invMass` (1000 if static); recurse `0x5032d0(body+0x20)` when the body is itself a child. For the Axis (`_main_200S.cfg`) the 17 `body` lines sum to 1180 kg; wheel `type` lines are `10.000 sphere 0.315`.

**src today.** `vehicle::Car` (src/vehicle/vehicle.*) implements `Vehicle::update` §1-§6 (+ the trigger-spring solve) but nothing instantiates it; `game::CarData` holds the `part_comp`/`car_root` bodies (mass 1 each, no compound mass, no wheels/sensors) and `WheelData` records from the cfg (`fill_car`: spring line 3rd value and `W+0x44/48/4c` not derived); `natives::advance_sim` calls `World::step` per sub-step with no drive hook. Cards: 145 (compound mass + spring constants), 146 (drive hook: woken car rests on its wheels).
