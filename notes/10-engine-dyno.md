# DynoData engine model ("dyno") — how the torque table is built

The engine's torque curve is **not authored**: part scripts fill a `java.game.parts.DynoData` object (bore, stroke, valve timing, spark, turbo, mixture …) and the native `calcDyno` runs a **0-D single-zone four-stroke cylinder simulation** (crank-angle stepping, polytropic compression, finite-duration heat release, valve flow toward manifold pressures, wall heat loss, p·dV work) at every rpm point to produce `torque[]` and `power[]` tables. `Vehicle__refreshParameters` (`0x4aebf0`) then copies the tables into the car's engine block (`notes/09-vehicle.md`).

Reference re-implementation: **`tools/dyno_model.py`** (same formulas/constants; run it to print the curve for the `DynoData.java` defaults: ~171 Nm @ 1750–2000 rpm, ~93 hp @ 4250 rpm for the 1.9 l default I4).

Script side (`bin/the game/parts/scripts/DynoData.java`, `bodypart/Chassis.java`):
```java
// Chassis.updatevariables(): engine = part on the engine slot
DynoData dyno = engine.dynodata;           // filled by the engine part scripts (Block, heads, cams, turbo, NOS …)
RPM_limit = dyno.RPM_limit;
if (dyno.maxRPM < 100) dyno.maxRPM = dyno.RPM_limit*1.25;
int desired_steps = dyno.maxRPM/250;  dyno.maxRPM = desired_steps*250;
dyno.calcDyno(desired_steps);              // => table step = 250 rpm
engine_torque = dyno.torque; maxRPM = dyno.maxRPM; ...
```
The `DynoData` script object owns a native 0x1ec-byte struct (`newNative` `0x4cc3b0` → `EngineModel__ctor 0x4f44a0`, handle stored in the instance via `FUN_004ed320(inst, DAT_007a2620, ptr)`; `deleteNative` frees it and its tables). `Vehicle__onPartAttached` (`0x4942b0`) stores the engine part's (or, if slot 401 is empty, the chassis') `dynodata` native pointer into the car's engine block `e+0xc30` and sets car flag `0x4000` (engine running).

## Functions
| addr | name | role |
|---|---|---|
| `0x4cba80` | `DynoData.calcDyno(F)` | reads script fields → struct, runs the model, writes results back |
| `0x4cc2b0` / `0x4cc1b0` | `DynoData.getTorque(rpm, nitro)` / `getHP(rpm, nitro)` | lerp in `+0x1dc` / `+0x1e4` (step `+0x1d0`, count `+0x1d4`); **the `nitro` argument is ignored** (always the normal table); 0 if `rpm < 0` or `rpm ≥ (count−1)·step` |
| `0x4f44a0` | `EngineModel__ctor` | zero table pointers `+0x1dc..0x1e8`, `+0x1d8`, `+0x120` |
| `0x4f44e0` | `EngineModel__freeTables` | free the 4 tables |
| `0x4f49e0` | `EngineModel__setDefaults` | constants below; computes mixture fractions/densities, `resetCycle` |
| `0x4f5980` | `EngineModel__updateMixture` | `fuel = 1/(AFR+1)`, `air = AFR/(AFR+1)`, `ρ_amb = P_amb/101325·ρ0`, `ρ_exh = P_exh/101325·ρ0` (9.869233e-6 = 1/101325) |
| `0x4f59f0` | `EngineModel__updateGeometry` | bore/stroke/area/swept volume/compression ratio, piston-speed rpm limit, copy manifold state |
| `0x4f4640` | `EngineModel__buildTables(step)` | rpm sweep, convergence loop, peak tracking, allocates tables |
| `0x4f4ca0` | `EngineModel__setupRpmPoint` | per-rpm spark/valve timing, turbo boost, consumption limits, burn duration |
| `0x4f4fc0` | `EngineModel__resetCycle` | cylinder at TDC filled with intake charge |
| `0x4f4560` | `EngineModel__runCycle` | step until one full cycle completes; fuel/air flow, BSFC, VE |
| `0x4f50e0` | `EngineModel__stepCrank(n)` | the per-crank-step physics (n = 1 from `runCycle`) |
| `0x4f5ab0` | `EngineModel__boostGauge(rpm)` | turbo boost for the OSD turbo gauge (called from the cockpit/OSD update in `type_car__vf13`) |
| `0x571143` | `libm_powf` | MSVC SSE2 `powf` (x in XMM0, y in XMM1, Ghidra drops both) |

## `calcDyno(tablesize)` (`0x4cba80`)
1. Native = the instance's handle; if there is none (class `java.lang.Native` missing or handle null) a **temporary** struct is allocated and freed at the end (`temp` mode).
2. `EngineModel__setDefaults()`, then copy script fields (byte offsets in the struct):
   `cylinders → +0x14`, `bore·0.5 → +0x04` (bore radius), `stroke·0.5 → +0x00` (crank radius), `Vmin → +0x10`, `in_min/out_min/in_max/out_max → +0x44/+0x48/+0x4c/+0x50`, `time_in_open/close → +0x94/+0x98`, `time_out_open/close → +0x9c/+0xa0` (also: `+0xa4 = in_open`, `+0xa8 = in_close − in_open`, `+0xb8 = out_open`, `+0xbc = out_close − out_open`), `time_burn → +0x1c`, `RPM_limit → +0x58` (stored only), `time_spark_min/inc/RPM0/RPM1 → +0xcc/+0xd0/+0xd4/+0xd8`, `rpm_turbo_mul/opt/range → +0xdc/+0xe0/+0xe8`, `+0xe4 = 0`, `P_turbo_max → +0xec`, `P_turbo_waste → +0xf0` (= `P_turbo_max` if the field does not exist), `mixture_H → +0x3c`, `mixture_ratio → +0xf4`, `max_air_consumption → +0xfc`, `max_fuel_consumption → +0xf8`, `T_intake +0x34 = 293.15 · intercooling` (if the field exists).
   **Not read:** `T_loss` (the struct keeps 95), `nitro_consumption`, `flags`, `torque`/`torque2` (inputs).
3. `updateMixture(); updateGeometry();` If script `maxRPM > 0` it overrides `+0x8c`, else the computed piston-speed limit is written back to `maxRPM`.
4. `step = max(maxRPM / tablesize, 1)` → `buildTables((int)step)` into table set 0 (`+0x1d8 = 0`).
5. `temp` mode only: `table_stepsize = step`, `torque = 1.0`, and the script arrays `torquetable[]` / `HPtable[]` are filled (clamped ≥ 0). (In 2.3.1 `DynoData.java` these fields are commented out, so with the normal persistent native nothing is exported; scripts use `getTorque/getHP`.)
6. Always: `Displacement = +0x64` (m³, all cylinders), `Compression = +0x74`, `maxTorque = +0x1ac`, `RPM_maxTorque = +0x1c0`, `maxHP = +0x1b0`, `RPM_maxHP = +0x1c4`.
7. **Nitro pass**: `T_intake` reset to 293.15 (intercooling dropped!), read `nitro_H`, `nitro_cooling`; if `nitro_H − 1 > 1e-4` **or** `nitro_cooling − 1 > 1e-4` (only values *above* 1 trigger; the NOS scripts use `nitro_H = 1 + consumption·0.0666 > 1` and `nitro_cooling = 1 − …·0.0666 < 1`, so the trigger is `nitro_H`): `mixture_H *= nitro_H`, `T_intake = 293.15·nitro_cooling`, `updateMixture(); updateGeometry();` (maxRPM restored afterwards), `+0x1d8 = 1`, `buildTables((int)(maxRPM / tablesize))` (no ≥ 1 clamp here) into table set 1 (`+0x1e0` torque, `+0x1e8` power). `temp` mode: `torque2 = 1`, `torquetable2[]`. The peak statistics in the struct are overwritten by the nitro pass but the script fields were already written.
8. Returns 0.

## Geometry (`updateGeometry`, `0x4f59f0`)
```
r = stroke/2 (+0x00)   R = bore/2 (+0x04)   L = conrod 0.15 m (+0x0c, fixed — not script settable)
+0x5c bore = 2R ; +0x60 stroke = 2r ; A (+0x68) = π R² ; Vd (+0x6c) = 2r·A (per cylinder)
Vtot (+0x70) = Vd + Vmin ; CR (+0x74) = Vtot / Vmin ; Displacement (+0x64) = cyl·Vd
+0x78 = 45 (m/s) ; maxRPM (+0x8c) = (45 / r)·(30/π)      // peak piston speed ω·r = 45 m/s
manifold copies: H_in(+0x154) = mixture_H, P_in(+0x158) = P_amb, T_in(+0x15c) = T_intake, ρ_in(+0x160) = ρ_amb
+0x170 = ω·r (piston speed)
```

## Table builder (`buildTables(step)`, `0x4f4640`)
```
count (+0x1d4) = (int)maxRPM / step ;  step → +0x1d0 ; reset the peak trackers
(re)allocate torque[count], power[count] for table set k = +0x1d8 ; torque[0] = power[0] = 0
for i = 1 .. count-1:  rpm = i·step
    +0x88 rpm ; Tc (+0x124) = 120/rpm (one 4-stroke cycle) ; ω (+0x128) = rpm·π/30 ; t = θ = 0
    setupRpmPoint()
    dt (+0x84) = Tc / 231                       // 231 crank steps per cycle (≈ 3.1° crank)
    resetCycle() ; runCycle() ; ref = IMEP
    repeat up to 10×: runCycle(); if |ref/IMEP − 1| < 0.05 break; ref = 0.4·ref + 0.6·IMEP
    track maxima (strictly greater): P_ind → +0x1b4/+0x1c8 ; P_brake → +0x1b8/+0x1cc/+0x1c4, maxHP +0x1b0 = P_brake·0.001341 ;
                                     IMEP → +0x1a8/+0x1bc/+0x1c0, maxTorque +0x1ac = torque at that point
    torque[i] = torque (+0x188) ;  power[i] = P_brake (+0x194, **watts**, not hp)
afterwards +0x188 = maxTorque, +0x190 = max P_ind, +0x194 = max P_brake
```
The cylinder state is reset only once per rpm point; the extra `runCycle` calls continue from the previous cycle (residual gas, pressures) — the loop is a steady-state iteration. (`setDefaults` initialises `dt = Tc·0.005`, overwritten here.)

## Per-rpm set-up (`setupRpmPoint`, `0x4f4ca0`)
```
updateMixture()
spark (+0x90) = spark_min + spark_inc·min(f,1)  with f = (rpm − RPM0)/(RPM1 − RPM0);  = spark_min if RPM1 ≤ RPM0 or f < 0
valve times (variable valve timing hooks, all unused by calcDyno → constant):
   in_open  (+0x94) = +0xa4 + +0xac·clamp((rpm − +0xb0)/(+0xb4 − +0xb0))   ; in_close  (+0x98) = in_open + +0xa8
   out_open (+0x9c) = +0xb8 + +0xc0·clamp((rpm − +0xc4)/(+0xc8 − +0xc4))   ; out_close (+0xa0) = out_open + +0xbc
P_in = P_amb, H_in = mixture_H, T_in = T_intake, ρ_in = ρ_amb
turbo (if rpm_turbo_mul > 0 and rpm_turbo_range > 0):
   x = |rpm·mul − opt| ;  g = 1 if x < +0xe4 (=0) else max(0, 1 − (x − +0xe4)/(range − +0xe4))
   boost = min(P_turbo_max·g, P_turbo_waste) ;  s = sqrt(boost + 1)
   T_in ·= s ; ρ_in ·= s ; P_in ·= s²          // boost is a pressure ratio − 1
consumption limits (use the flow rates of the *previous* runCycle, i.e. previous rpm point):
   if 0 < max_fuel_consumption < fuel_rate(+0x198): H_in /= fuel_rate/max_fuel
   if 0 < max_air_consumption  < air_rate(+0x19c):  P_in (+0x158) /= air_rate/max_air   // pressure, not ρ (re-verified 2026-10; tools/dyno_model.py divides rho_in: wrong)
burn duration (+0x164) = time_burn·(1 − min(rpm·5e-5 (+0x20), 0.9))
```
`EngineModel__boostGauge(rpm)` evaluates the same boost `b` and returns `(sqrt(b+1) − 1)/(sqrt(min(P_max, P_waste)) − 1)` (gauge fraction; note `sqrt(P)` not `sqrt(P+1)` in the denominator — preserved quirk).

## One crank step (`stepCrank`, `0x4f50e0`) — state `P` Pa (+0x140), `T` K (+0x144), `m` kg (+0x13c), `H` J/kg of charge (+0x148), `b` burn fraction (+0x108), `V` (+0x134), `t` time in cycle (+0x120), `θ` (+0x12c)
```
Vprev = max(V,0) ; P0 = P ; t += dt
if t ≥ Tc:                                   // cycle boundary
    counter(+0x174)++ ; t −= Tc ; W_last(+0x17c) = W ; W = 0
    IMEP (+0x184) = W_last / Vd   (0 if Vd ≤ 1e-8)
    torque (+0x188)  = cyl·IMEP·Vd·0.125                 // ¹/₈, not ¹/(4π)
    P_ind  (+0x190)  = rpm·IMEP·Vd·cyl/60                 // inconsistent with torque·ω by a factor 1.27
    P_fric (+0x180)  = rpm³·2e-8 ;  P_brake (+0x194) = P_ind − P_fric ;  hp (+0x18c) = P_brake·0.001341
θ += ω·dt (wrap at 4π)
x = sqrt(L² − (r sinθ)²) + r cosθ ;  V = (r + L − x)·A + Vmin
if V < 1e-7: V = 1e-7, skip the rest of the step
q = Vprev/V ;  P *= q^1.3 ;  T *= q^0.3                 // polytropic, n = 1.3
a_in  = in_max  if t ∈ (in_open·Tc,  in_close·Tc)  else in_min     (window wraps if close ≤ open)
a_out = out_max if t ∈ (out_open·Tc, out_close·Tc) else out_min
ts = spark·Tc (+0x10c) ; te = ts + burn (−Tc if > Tc)
b' = (t − ts)/burn inside (ts, te] (wrap-aware) ; = 1 on the first step after the window if b > 0 ; else 0
if b' > 0:  m = max(m,1e-7), T = max(T,1e-7)
            T' = T + (b' − b)·H·m_old / (c_v·m)   (c_v = 712, +0x28) ;  P *= T'/T ; T = T'
            if b' ≥ 1: b' = 0, H = 0              // charge burnt
b = b'
for (valve, manifold) in (intake: a_in, P_in, ρ_in, T_in, H_in, clamp P ≥ 1e-6),
                          (exhaust: a_out, P_exh 111457.5 (+0x30), ρ_exh (+0x80), T 293.15 (+0x38), H 0 (+0x40)):
    if a > 0:
        k = min(a·dt·1000, 1) ;  P' = P + k·(P_man − P)
        V_eq = V·P/P'                              // volume the present gas would take at P'
        if V ≤ V_eq:  m −= (V_eq − V)/V · m         // outflow
        else:         Δm = (P'/P_man)·ρ_man·(V − V_eq) ; m = max(m,1e-7)
                      T = (T_man·Δm + T·m)/(m+Δm) ; H = (H_man·Δm + H·m)/(m+Δm) ; m += Δm
                      intake only: air(+0x16c) += Δm·AFR/(AFR+1), fuel(+0x168) += Δm/(AFR+1)
        m = max(m,0) ; P = P'
wall loss: k = min(dt/95, 1) (+0x54) ; d = T − 353.15 (+0x110) ; loss = d²·sign(d)·k, at most d
           Q_loss(+0x118) += loss ; P *= (T − loss)/max(T,1e-7) ; T −= loss
W (+0x178) += (P + P0)/2 · (V − Vprev)        // trapezoidal p·dV
```
`runCycle` (`0x4f4560`) clears fuel/air/heat accumulators and the counter, steps until the counter becomes 1, then: `fuel_rate (+0x198) = fuel·cyl/Tc`, `air_rate (+0x19c) = air·cyl/Tc` (kg/s), `BSFC (+0x1a0) = fuel_rate·7920/hp` (lb/(hp·h)), `VE (+0x1a4) = (air + fuel)/(Vd·ρ0)`, `Q_loss /= Tc`.

`resetCycle` (`0x4f4fc0`): `H, P, T, ρ = manifold values`, `t = θ = b = 0`, accumulators 0, `x = L + r` (TDC), `V = Vmin`, `m = Vmin·ρ_in`.

## Struct layout (0x1ec bytes, offsets in bytes; defaults from `setDefaults`)
| off | meaning (default) | off | meaning (default) |
|---|---|---|---|
| `+0x00` | crank radius r = stroke/2 (0.053175) | `+0x84` | dt = Tc/231 |
| `+0x04` | bore radius R (0.049) | `+0x88` | current rpm (1000) |
| `+0x08` | 1.029 (unused) | `+0x8c` | maxRPM |
| `+0x0c` | conrod length L (0.15) | `+0x90` | effective spark time (fraction of cycle) |
| `+0x10` | Vmin clearance volume (1e-4) | `+0x94..+0xa0` | effective in_open, in_close, out_open, out_close |
| `+0x14` | cylinders (4) | `+0xa4/a8/ac/b0/b4` | intake base open, duration, VVT advance, VVT rpm0/rpm1 |
| `+0x18` | 1.0 (unused) | `+0xb8/bc/c0/c4/c8` | exhaust ditto |
| `+0x1c` | time_burn (0.003 s) | `+0xcc..+0xd8` | spark min, inc, RPM0, RPM1 |
| `+0x20` | burn-time rpm factor (5e-5) | `+0xdc/e0/e4/e8` | turbo mul (20), opt (85000), full band (0), range (30000) |
| `+0x24` | ρ0 air density (1.2929) | `+0xec/+0xf0` | P_turbo_max (0), P_turbo_waste (3) |
| `+0x28` | c_v (712 J/kg/K) | `+0xf4` | mixture ratio AFR (14) |
| `+0x2c` | P_amb (101325) | `+0xf8/+0xfc` | max fuel / air consumption kg/s (100) |
| `+0x30` | P_exhaust (111457.5 = 1.1 atm) | `+0x100/+0x104` | current intake / exhaust flow coefficient |
| `+0x34` | T_intake (293.15 K) | `+0x108` | burn fraction b; `+0x10c` spark time (s) |
| `+0x38` | T of exhaust backflow (293.15) | `+0x110/+0x114` | wall temperature (353.15 K) |
| `+0x3c` | mixture_H (1e6 J/kg) | `+0x118` | heat-loss accumulator; `+0x11c` 0 |
| `+0x40` | H of exhaust backflow (0) | `+0x120..+0x148` | t, Tc, ω, θ, x, V, ρ, m, P, T, H |
| `+0x44..+0x50` | in_min, out_min, in_max, out_max (0.005, 0.005, 0.8, 0.8) | `+0x14c/+0x150` | air / fuel mass fractions |
| `+0x54` | wall-loss time constant (95, fixed) | `+0x154..+0x160` | manifold H, P, T, ρ |
| `+0x58` | RPM_limit (not used) | `+0x164` | burn duration (s) |
| `+0x5c/+0x60` | bore, stroke | `+0x168/+0x16c/+0x170` | fuel / air per cycle, piston speed |
| `+0x64` | displacement (m³) | `+0x174` | cycle counter; `+0x178/+0x17c` work (current / last) |
| `+0x68/+0x6c/+0x70` | A, Vd, Vtot | `+0x180..+0x194` | P_fric, IMEP, torque, hp, P_ind, P_brake |
| `+0x74` | compression ratio | `+0x198..+0x1a4` | fuel kg/s, air kg/s, BSFC, VE |
| `+0x78` | 45 m/s piston-speed limit | `+0x1a8..+0x1cc` | maxima: IMEP, torque, hp, P_ind, P_brake; rpm@ IMEP, torque, hp, P_ind, P_brake |
| `+0x7c/+0x80` | ρ_amb, ρ_exh | `+0x1d0/+0x1d4/+0x1d8` | step (int), count (int), table set |
| | | `+0x1dc/+0x1e0/+0x1e4/+0x1e8` | torque, nitro torque, power (W), nitro power tables (float[count]) |

## How the vehicle uses it (`Vehicle__refreshParameters`)
`d = e+0xc30`; if `d == 0` → 1-point zero tables. Else `e+0 = e+8 = d.count`, `e[4]` pairs `(i·step, d.torque[i])`, `e[0xc]` pairs `(i·step, d.nitroTorque[i] or d.torque[i])`, `e+0x14 = d.RPM_maxTorque`. `e+0xd4 = e+0xd8 = 1.0` (torque multipliers; `Chassis.setTorque` changes them) — so the in-game curve is the dyno's **indicated** torque in Nm; engine friction is added separately by `Engine__frictionTorque` (`notes/09-vehicle.md`), the dyno's `rpm³·2e-8` friction only affects the reported hp.

## Details verified while writing task cards (2026-10)
- Script fields are read with `Instance__getFloatField 0x4ec980`: a missing field or one whose descriptor is not `F` logs `vm_get_float_field: ...` and yields 0. All `DynoData.java` fields are `float` (also `cylinders`; struct `+0x14` holds a float, default 4.0f). `0x4ecff0` is a field-exists test (returns 0 when missing, 3 when present and no signature asked).
- `stepCrank` caches `Vmin (+0x10)` and `Vtot (+0x70)` in locals clamped to ≥ 0 and writes them back at the end; the cycle-boundary `Vd` is `Vtot − Vmin` (not `+0x6c`). Volume: `V = ((r + L) − x)·A + Vmin`; if `V < 1e-7` the step ends with `V = 1e-7` after `t`/`θ` (and a possible cycle boundary) were already advanced. Polytropic exponents read from `0x59ab6c` = 1.3 (P) and `0x593704` = 0.3 (T).
- Burn window (`ts = spark·Tc`, `te = ts + burn`, `te −= Tc` if `te > Tc`): inside = `ts < t ≤ te` (or `t > ts || t ≤ te` when `te ≤ ts`); inside → `b = (t − ts)/burn`, heat only if `b > 0` (a negative `b` is stored); outside → `b = 1` + heat if the previous `b > 0`, else 0.
- `buildTables`: `count = (int)maxRPM / step` (int division), rpm stepped as int; peaks use strict `>`. The nitro pass calls it with `(int)(maxRPM / tablesize)` without the ≥ 1 clamp (0 → integer divide fault in the exe).
- `setDefaults` calls `resetCycle` before any `updateGeometry` (reads uninitialised manifold/area on a fresh allocation; harmless, every rpm point resets again) and does not reset `fuel_rate/air_rate (+0x198/+0x19c)`: the first rpm point's consumption limit uses the previous `calcDyno`'s last rates (garbage on a fresh struct; only matters when `max_*_consumption > 0`).
- `getTorque/getHP` return 0 when class `java.lang.Native` is missing, when the instance has no native, when `!(rpm ≥ 0)` (NaN too), when `count ≤ 1` and when `rpm ≥ (count−1)·step`.

## Open points
- `+0x08` (1.029) and `+0x18` (1.0) are set but never read by the model.
- The torque/power inconsistency (`/8` vs `/60`) and `getTorque` ignoring its `nitro` argument are original behaviour; preserve them.

## When the curve is built in src (2026-10-24, card 151 RE)
No native recompute fills the curve: it is purely script-driven. `Vehicle(...)` ctor (sl/Scripts/game/Vehicle.java:115-119) runs `addStockParts` with `suspend_update = 1`, then `chassis.forceUpdate()` (0x4b5890 -> 0x4aebf0). That runs `Chassis.updatevariables` -> `engine.updatevariables()` (Block fills `dynodata`; the block subclass then calls `the_car.getSfxTable(0..2)`) -> `dyno.calcDyno(maxRPM/250)` -> refresh copies the dyno tables. In src the only missing link was the stub `Chassis.getSfxTable` (0x4b3890): it threw inside the block's updatevariables, calcDyno never ran, curve stayed 1 point 0 Nm (error swallowed by `ch_force_update`). With it implemented (scratch build): D18V RPM_limit 6750, maxRPM 8250, 33 points, max 159.05 Nm.
