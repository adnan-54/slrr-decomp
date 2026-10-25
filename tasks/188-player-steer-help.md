---
id: 188
status: blocked  # port done; 3 vmboot player checks still assert raw steering, see Result
branch: card/188
depends: []
---
# Vehicle::updateControls 0x498300: steer help (type cfg `steerhelp`, e+0xa34)

## Why (notes/09 "Steer help")
Every shipped car type cfg has a `steerhelp` line (SunStrip/Duhen: `1.0 30.0 0.5 0.0 1.57 1.0`); the exe
blends the player's steer axis with a yaw-rate damper and a slip-angle term above `lo` m/s, then rate-limits
it. src skips it (`[not ported]`), so player steering is raw. Bots send `steerhelp 0` (flag 0x200), so they
are unaffected.

## Exe evidence
### 0x48fa30 type cfg key `steerhelp` (table 0x5da878)
```
if (M+0x120 == 0)   // M+0x120 = type load counter (INC at end of the type ctor 0x49ab20 @0x49be8d): first parse only
    sscanf(rest_of_line, "%f %f %f %f %f %f",
           M+0x132c lo, M+0x1330 hi, M+0x1334 keep, M+0x1338 slipGain, M+0x133c slipMax, M+0x1340 yawGain)
```
Ctor defaults (0x49b41b..0x49b44d): lo 0, hi 0, keep 1.0, slipGain 0, slipMax 3.15 (0x4049999a), yawGain 0.01.
A line with fewer values keeps the defaults for the missing ones (sscanf).
### 0x4a2e60 command / cfg `steerhelp v` (already partly ported at car.cpp ~1504)
`v > 0: flags &= ~0x200, e+0xa34 = v;  else flags |= 0x200, e+0xa34 = 0`. Template default e+0xa34 = 1.0.
### DAT_005fe320 = gameplay config `steerhelp_turn` (0x47c4e3, only if the key exists): not in the shipped
files -> 0. Keep it as a `static const float kSteerHelpTurn = 0.0f; // DAT_005fe320` in car.cpp.
### 0x498300 steer help block (asm 0x498a6e..0x498d44)
Position: inside the "driver present" branch, after the axes read, the 0x40000 "brake" block and the horn
toggle, before the gear-shift block. `old` = `+0x1ddc` read at function entry (before the axes read).
`v` = root body linear velocity, `R` = root rotation (Mat34 row-major, `R00 = m[0]`, `R01 = m[1]`, `R02 = m[2]`,
`R10 = m[4]`, `R11 = m[5]`, `R12 = m[6]`, `R20 = m[8]`, `R21 = m[9]`, `R22 = m[10]`), `w` = root angular velocity.
```
if (!(flags & 0x200) && e+0xa34 > 0 && hi > 0) {
    h   = e+0xa34
    lat = (R10*v.y + R00*v.x) + R20*v.z           // keep this summation order
    fwd = (R02*v.x + R12*v.y) + R22*v.z
    q   = (fwd*fwd) * (h*h)
    if (q > lo*lo) {
        f = ((hi/h)*(hi/h) > q) ? (sqrtf(q) - lo) / (hi - lo) : 1.0f
        a = (float)atan2((double)-lat, (double)-fwd)          // x87 FPATAN
        s = fabsf(a) > 1.5707964f ? 0.0f : (a > slipMax ? slipMax : max(-slipMax, a))
        t = (steer / max_steer) * DAT_005fe320                // max_steer = e+0xa20
        yr = (R11*w.y + R01*w.x) + R21*w.z
        d  = (yr - yaw_prev) + yr;  yaw_prev = yr             // yaw_prev = car+0x1fbc, only updated here
        steer = (1 - (1 - keep) * f) * steer + (yawGain * (t - d) + slipGain * s) * f
        steer = clamp(steer, -1, 1)                           // > 1 -> 1, < -1 -> -1
        steer = (steer - old) * min(dt * 10, 1) + old
        if (steer * old < 0) { r = (fabsf(old) * 0.5f) / fabsf(steer); if (r < 1) steer *= r; }
    }
}
```
`dt` is the 0x498300 argument = the `dt` that `control_cars(float dt)` already receives.

## Files
- edit: src/game/car.h (CarData: `float steer_help = 1.0f; // e+0xa34`, `float yaw_prev = 0.0f; // +0x1fbc`,
  `float sh_lo = 0, sh_hi = 0, sh_keep = 1.0f, sh_slip_gain = 0, sh_slip_max = 3.15f, sh_yaw_gain = 0.01f;
  // M+0x132c..+0x1340`)
- edit: src/game/car.cpp
  - `fill_car`: `if (ln.key == "steerhelp")` -> up to 6 `num(ln.values[i])` into sh_* (only present values);
    `continue`. (src parses the type cfg once per car: that is the M+0x120 == 0 first parse.)
  - command handler (~1504): keep the flag logic, also store `c->steer_help = on ? 0.0f : f` for `steerhelp`.
  - `control_cars`: `float old_steer = c->steer_input;` at the top of the loop body; the block above after the
    horn toggle. If `c->root_body` is null skip it (`// INVENTED: no root body, no help`).
- read only: src/physics/physics.h (`RigidBody::vel`, `rotation()`, `angular_velocity()`)

## Done when
- each ported piece carries its `// 0xADDR` comment; `make -C src clean check` green (clean build).
- If a vmboot `--race` / player check fails, report the check and numbers (status `blocked`) instead of
  weakening the port.
- no new tests or hand-made expected values; card frontmatter status/branch filled.

## Result
Port is complete (0x48fa30 cfg, 0x4a2e60 command, 0x498a6e..0x498d44 block); `make -C src clean check`
fails 3 old vmboot player checks that assert raw steering (`main` is green, 166/166):
- `FAIL Left (axis 1) gives steer > 0.9 (got 0.756091)`
- `FAIL Right (axis 1) gives steer < -0.9 (got -0.631610)`
- `FAIL auto clutch slips at launch (0x4977a0): max clutch_input 0.000000`
The numbers match the ported formula (SunStrip `1.0 30.0 0.5 0.0 1.57 1.0`: keep term `1-0.5*f`);
the checks' `>0.9` bounds predate steer help. Not updated: no exe-run value available, and a value from
this port would be a hand-made expectation. Needs a card to re-derive them from the exe.
