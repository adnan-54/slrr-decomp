# 34 — Wear / damage build-up while driving (RE 2026-10-25)

Follow-up of notes/33 §4. Sources: `Vehicle__update` 0x49c8e0 (decompile + asm of the 4 `damageRoll`
call sites), `Vehicle__damageRoll` 0x4afb50, `Vehicle__healthFactor` 0x4b0ff0, `FUN_00497520`
(renamed **`Part__syncWear`**), `Vehicle__createCar` 0x4992e0 (defaults), `Config__loadGameplaySettings`
0x47c240. Cards: 224 (measure bug, §0), 225 (engine heat/wear), 226 (drivetrain/brake wear + bearing),
227 (§8 general wear + part sync + odometers). Not carded yet: tyre impact wear / flat tyre / suspension
break (§4), tyre/brake temperatures beyond what 226 needs, nitro (sim has none).

## 0. Open items from card 223 (resolved)
- **speed=999 is a src bug.** The exe's measure globals are not 0 before `System.setMeasure`:
  `0x5da524` is initialised data = `0x44c94000` = **1610.0**, and two CRT dynamic initialisers derive
  the others at startup: 0x409af0 `0x5fe378 = 0x5da524 * 0x59dd50 (0x3991a2b4 = 1/3600)`;
  0x409a90 `0x5fe374 = 0x59373c (10.0) / 0x5da524`. Only 0x4e57b0 (`System.setMeasure`) writes them later,
  and the shipped scripts call it only from OptionsDialog (options change / reset). src's Scheduler starts
  all three at 0 (`INVENTED: ... 0 until setMeasure`, natives.cpp ~160), so `v / measure_per_hour` = inf
  and `gauge_clamp` returns the gauge max (999). Card 224.
- **rpm= empty is correct.** `OsdGauge__update` 0x4b1d50 returns when `G+0x24` (ref) is 0; the rpm text
  gauge gets a ref only from cfg `osd_rpm`, which 3 of 70 car cfgs have (all 70 have `osd_rpmpin`).
- **Race time 655.1 -> 651.6 s after card 222** comes from **engine wear records** (kind 8). Measured with
  a temporary env-gated print/skip in `apply_part_wear_records` (reverted, not committed): with kind 8
  skipped the race is 655.112 s again; skipping 5/6/7/9/10, the tail (tyre refs), the propagation, or
  only creating the PartData leaves 651.594 s. In the race 9 engine parts carry P+0xb8 in
  0.00004..0.246 (used cars' `setWear`), so `torque_health = torque_base * (1 - 0.6 w^2)` drops (e.g.
  0.964 at 0.246) and bots/player drive differently. Expected exe behaviour; tyre/brake records (also
  non-zero) do not reach the sim because the effective friction/brake are computed by the setters that
  ran (in updatevariables) while the wear was still 0, as in the exe (0x4aebf0 order).

## 1. Helpers
```
// 0x4afb50 Vehicle__damageRoll(amount), ECX = &pair{wear, rate}. NOT random (notes/09 called it a roll).
int wear_add(float *pair, float amount):
    if (amount <= 0) return 0
    pair[0] = pair[1] * amount + pair[0]
    if (pair[0] < 0) { pair[0] = 0; log "intdamage_data: part with invalid wear detected!" }
    if (pair[0] < 1) return 1          // (<= 1 && != 1)
    pair[0] = 1; return 2
// 0x4b0ff0 Vehicle__healthFactor(w) = w < 1 ? 1 - w*w*0.6 : 0.100000024   (src: health_factor / wear_factor)
K = DAT_005dacd4 = Config "internal_damage" (static init 1.0, 0x47c240 reads Config.internal_damage)
```
Several sites inline `wear_add` (clamp to [0,1], same log) instead of calling it: transmission,
clutch, brake, general wear.

## 2. createCar defaults (0x4992e0), car+…
`+0x1ec0/+0x1ec4` temp/ambient 20.0, `+0x1ebc` 320000, `+0x1ec8` 9.375e-5 (src has these);
`+0x1e9c` 0, **`+0x1ea0` 0x3356bf95 (5.0e-8)**; `+0x1ea4` 0, **`+0x1ea8` 5.0e-8**; `+0x1eb4` 0,
**`+0x1eb8` 5.0e-8**; `+0x1eac` 0, **`+0x1eb0` 0x36a7c5ac (5.0e-6)**; `+0xb4` 1.0, `+0xb8` 0,
`+0xbc` 1/M+0x121c. src car.h has 0 for the four rates (the engine one is a divisor, §3).
Per wheel: wear/rate `+0x15c..+0x170` = 0; tyre heat cap `+0x174` 30000 (0x46ea6000), temp `+0x178`
20, ambient `+0x17c` 20, cooling `+0x180` 0x3ada740e (0.0016667); brake heat cap `+0x184` 50000
(0x47435000), temp `+0x188` 20, ambient `+0x18c` 20, cooling `+0x190` 0x3a83126f (0.001).
`refreshParameters` zeroes only the six wheel wear/rate fields; car-level pairs keep their values.

## 3. Engine (§1 of notes/09; only while engine running, inside `if |rpm| > 0.001`), 0x49cfd0..0x49d2a5
`heat = thr * 1000 * T_max` (thr after idle governor / starter / bucking; nitro adds `2000*ΔT`).
```
if (!(car.flags & 0x400)) {
    if (heat*dt > 0) temp += heat*dt / heat_cap                        // +0x1ec0 / +0x1ebc
    c = (temp <= 90 || radiator_dmg >= 1) ? e.cool_low                 // e+0xa5c
        : health(radiator_dmg) * (|v_root|^2 * e.cool_speed + e.cool_air) + e.cool_low   // a64, a60
    cooling_coeff = c / heat_cap                                       // +0x1ec8
    if (temp > 100) wear_add(&radiator_dmg, (temp-100)^2 * dt * K)    // asm 0x49d071: LEA ECX,[ESI+0x1eac]
    if (engine_dmg < 1) {                                              // +0x1e9c
        fric = rpm < 0 ? e.fric_rev : e.fric_fwd                       // e+0xf8 / e+0xf4 (signed rpm)
        r = |rpm| / e.max_rpm;  r16 = ((r^2)^2)^2 ^2                   // four squarings
        t = |temp - 90| * 0.01
        p = ((dt + dt) / engine_rate) * r16 * fric * 100 * (t*t + 1)  // engine_rate +0x1ea0
        if (p > 0 && wear_add(&engine_dmg, K * p) != 0) {              // 0x49d169: ECX = +0x1e9c
            h = health(engine_dmg)
            torque_health = e.torque_base * h; nitro_health = e.nitro_base * h      // +0x1e04/08 = e+0xd4/d8 * h
            if (engine_dmg >= 1) { torque_health *= 0.5; nitro_health *= 0.5;
                                   e.fric_fwd *= 50; e.fric_rev *= 50; post event M+0x1f4c (sound) }
        }
    }
}
// always (engine on), after the |rpm| block:
temp -= |temp - ambient| * cooling_coeff * dt * (temp - ambient)
```
Note `engine_rate` cancels (`rate * (2dt/rate)`), but a 0 rate gives inf*0 = NaN -> wear 1 (blown):
src must start it at 5e-8 like createCar.

## 4. Tyre impact / flat / suspension (§2, per wheel with S, contact, not 0x400) — NOT carded
```
q = |R_S * S+0xe4| * |W+0x140| * dt*50 ; if (S+0xac ground material) q *= gm+0x50
if (q > 0.1) {
    W+0x178 += q / W+0x174                                             // tyre heat
    if (W+0x15c < 1 && wear_add(&W+0x15c, 2*q*K) != 0) {             // 0x49d729 ECX = W+0x15c
        w = W+0x15c; f = w >= 1 ? 0.5 : 1 - w*w*0.4                     // 0.4 here, 0.6 in the setters
        W+0x70 = W+0x68*f ; W+0x6c = W+0x64*f
        if (w >= 1) { W+0x15c = 1; r0 = W+0x60; W+0x60 = 0.8*r0; W+0x5c += W+0x60 - r0;
                      Suspension__configure(S.k, S.bump, W+0x5c, W+0x54+r, W+0x58+r, S.rebound, r);
                      W+0x21c (pacejka[13]) = 0.4; event M+0x1f5c }
    }
}
if (W+0x16c < 1 && (q2 = S+0xfc * dt*50 * 3) > 0.1 && wear_add(&W+0x16c, q2*K) != 0 && W+0x16c >= 1) {
    W+0x16c = 1; Suspension__configure(S.k*0.8, S.bump*0.3, W+0x5c, W+0x54+r, W+0x58+r, S.rebound*0.3, r)
    event M+0x1f7c }
if (!0x400) W+0x178 -= |W+0x178 - W+0x17c| * W+0x180 * dt * (W+0x178 - W+0x17c)   // tyre cooling
```
Blocked in src: `S+0xe4` (spring force vector) and `S+0xfc` are not modelled by the sim's spring
(vehicle.cpp `update_suspension` has only load/velocity), and Suspension__configure re-runs belong to
the WheelData setters. Needs a mapping card first.

## 5. Drivetrain (§3, coupled branch only, not 0x400), 0x49e1bf..0x49e2f6
```
k = 1 - clutch ; (wd', we') = shaft/engine speeds after the sync (vehicle.cpp: wd_new, we_new)
x = |(wd' - we') * k * 0.1| * K ;            if (x > 0) trans_wear  = clamp01(trans_rate  * x + trans_wear)
y = (|clutch_slip| * 0.01)^4 * dt * 1e-4 * K; if (y > 0) clutch_wear = clamp01(clutch_rate * y + clutch_wear)
clamp01: < 0 -> 0 (+ log), >= 1 -> 1
```
Nothing in the sim reads trans/clutch wear (HUD T/L/O letters only).

## 6. Brakes (§4, per wheel with S and |ω| > 0.001, not asleep), wear at 0x49e6db..0x49e8de
```
T_b = (W+0xfc + brake)*e+0xdc*W+0xd8 + e+0xe0*hbrake*W+0xdc + W+0xf0(bearing)
if (T_b > 0) {
    ... ω update (ported) ...
    if (flags & 0x400) skip to next wheel (no cooling either)
    q = |ω'| * (T_b - bearing) * 10 * 0.001 * dt * 50
    if (q > 0) { W+0x188 += q / W+0x184
                 if (W+0x164 < 1 && q*K > 0) {
                     W+0x164 = clamp01(W+0x168 * q * K + W+0x164)
                     h = health(W+0x164); W+0xd8 = W+0xd0*h; W+0xdc = W+0xd4*h
                     if (W+0x164 >= 1) { W+0xf0 += 100; W+0xd8 *= 0.2; W+0xdc *= 0.2; event M+0x1f6c } } }
}
if (!(flags & 0x400)) W+0x188 -= |W+0x188 - W+0x18c| * W+0x190 * dt * (W+0x188 - W+0x18c)
```
**src gap found:** W+0xf0 is the bearing friction (`WheelRef.setBearing` writes it; src names it
`WheelData::damage_visual`), but `drive_cars` never copies it to `vehicle::Wheel::bearing`, so the sim
brakes with bearing 0 (also `extra_brake` W+0xfc is never copied). Fixed in card 226.

## 7. §8 general wear + Part__syncWear (0x4a089a..0x4a0bf3, end of Vehicle::update, gate dt > 1e-5)
```
speed = car+0x1fd8 = |v_root| * 0.2 + speed * 0.8
odo: +0x1e90 += |ω_e|*dt ; +0x1e94 += |shaft|*dt ; +0x1e98 += speed*dt
if (!(flags & 0x400) && (speed > 0.01 || |ω_e| > 0.01 || |shaft| > 0.01)) {
    x = speed*dt*K; if (x > 0) car+0xb8 = clamp01(car+0xbc * x + car+0xb8)   // the car's own part wear
    for each installed part node (car+0xdc list, object node+0x30 -> typed obj, payload obj+0x4c):
        Part__syncWear(obj, P, M, car, dt)
    mileage(car+0x2250) += speed*dt
}
// 0x497520 Part__syncWear(obj, P, M, car, dt)
if (P.wear >= 1) return
k = P.dmg_mode & 0xffff, w = P.dmg_mode >> 16
switch k:
  2:  wear_add(&P.wear, |car+0x1e7c ω_e| * K * dt)
  3:  wear_add(&P.wear,  car+0x1fd8 speed * K * dt)        // no abs
  4:  wear_add(&P.wear, |car+0x1e78 shaft| * K * dt)
  0xe:wear_add(&P.wear, |car+0x1e8c * car+0x1e7c| * K * dt)
  5/6/7: if (w < M+0x1fb4) P.wear = wheel[w] +0x15c / +0x164 / +0x16c   else nothing
  8: P.wear = +0x1e9c   9: +0x1ea4   10: +0x1eb4   12: +0x1eac
  other: nothing
if (P.wear < 0) { log "Part %s has invalid wear!"; P.wear = 0 }
if (P.wear >= 1 && obj+0x50 script instance) callScriptMethod(obj, "updateDamage")   // Part.java:706 empty
```
So the car-side wear (engine, clutch, …) is written back into the parts each tick: this is how a
blown engine survives a `forceUpdate` (0x4a8060 reads P+0xb8 back).

## 8. src status
| piece | src |
|---|---|
| measure defaults | wrong (0) -> card 224 |
| engine heat, radiator overheat, engine wear, torque health drop | missing (engine_temp stays 20) -> 225 |
| transmission / clutch wear | missing -> 226 |
| brake heat / wear / broken brake; bearing + extra brake not copied to the sim | missing -> 226 |
| tyre impact wear, flat tyre, suspension break, tyre heat | missing, not carded (§4) |
| speed smoothing, odometers, general wear, Part__syncWear, mileage | missing (getMileage never moves) -> 227 |
| nitro torque / heat / tank use | missing in the sim (separate gap; heat term then gains `2000*ΔT`) |
| damage events M+0x1f4c/5c/6c/7c | sound events, not ported (shortcut in the cards) |
