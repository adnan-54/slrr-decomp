---
id: 148
status: merged
branch: card/148
depends: [147, 149]
---
# Control list: Vehicle__updateControls reads the player's axes once per physics tick

## Why
After 147 the car knows its driving player and vehicle::Car sees CarData's inputs, but nothing
writes those inputs. In the exe `Game__physicsTick` 0x47e050 runs the mode-8 control list once
per tick before the sub-steps: `Game__runControlCallbacks` 0x47bc20(ctrlAcc) ->
`Game__controlCallback` 0x47bc80 -> `_type_car` vf11 `Vehicle__updateControls` 0x498300, which
reads the player's Controller axes into the car inputs, shifts gears and wakes a sleeping car on
input. Full pseudocode: notes/09 "Control path" (copy from there; key parts below).

## Exe functions (5; behaviour to match)
- 0x47e050 (part): `ctrlAcc += step; if (ctrlAcc > 1e-4) { 0x47bc20(ctrlAcc); ctrlAcc = 0 }`
  before the sub-step loop (`step = min(warp*dt, 0.1)`, the same value advance_sim computes).
- 0x47bc20/0x47bc80: every car (vf15 GII 8 -> 1) gets `updateControls(car, ctrlAcc)`.
- 0x498300 `Vehicle__updateControls` (P = slot `driver_slot`, ax(v) = player Controller
  `get_axis_val(v)`, ai(v) = `int(get_axis_val(v)*127) * (1/127.0f)` i.e. *0.007874016):
```
old_* = steer, throttle, brake, clutch
if (traffic) return
if (!(tear > 0))                      { handbrake=0; brake=0; throttle=0 }   (+0x1f64/68/6c=0)
else if (driver_slot < 0)             { horn=0; brake=0; throttle=0 }
else if (!slot_player[driver_slot])   { throttle=brake=0; horn=0 }
else {
  steer=ax(1); throttle=ax(28); brake=ax(29); handbrake=ax(48); clutch=ai(49); nitro=ai(31)
  shift=ai(50); horn_ax=ai(54); gearset=ai(51)
  if (flags & 0x40000) { throttle=0; brake=1; handbrake=1; nitro=0; shift=0; gear=0 }
  horn: on each crossing of horn_ax over 0.1 (odd count = pressed): horn_id = (horn_id+1) & 3
  gear shift, latch bit 0x400000 (cleared while |shift| <= 0.1), now = process_world().time:
    shift >  0.1, latch clear: set latch; if (gears) { gear==7 -> 0, t=now+0.2 ; gear<gears -> gear+1, t=now+0.3 ; shift_timers(up=1) }
    shift < -0.1, latch clear: set latch; if (gears) { gear==0 -> 7, t=now+0.3 ; gear!=7 -> gear-1, t=now+0.2 ; shift_timers(up=0) }
    (t -> +0x1e68 shift_end)
  gear set (ai(51)) as notes/09 (DAT_005fe31c taken as 0: only gearset > 0 acts)
}
if (flags & 0x40000) { brake=1; handbrake=1; nitro=0; gear=0; throttle=0 }   // as in the exe
0x4b04a0: if (starter_timer >= 0) throttle=0
          if (shift_end > 0) { if (shift_end <= now) { shift_end=-1; shifting=0 }
                               else if (!(flags&0x30)) { throttle=0; clutch=1; shifting=1 }
                               else { clutch=0; shifting=1 } }
          +0x1e70/+0x1e74 expire to -1 (0x4b03c0 when both < 0: not ported, shortcut)
flags &= ~0x03000000
k=min(10*dt,1): throttle = old_t+(throttle-old_t)*k; brake = old_b+(brake-old_b)*k
clutch = old_c + (clutch-old_c)*min(33.333336*dt, 1)
if (!(flags&1)) { if (|steer_copy-steer| <= 0.001) return }
else { if (root asleep && |throttle|<=0.01 && |steer-steer_copy|<=0.01 && gear==gear_copy) return
       flags = (flags&~1)|2; if (root asleep) wake root (0x507060, world.wake) }
steer_copy = steer        // 0x4aa830 wheel steer geometry: vehicle::Car does it from steer
```
- 0x4b0190 (timers only): `up==0`: `+0x1e74=now+0.3, +0x1e70=now+0.7`; up: `+0x1e70=now+0.5,
  +0x1e74=now+1.0`. Shift SFX and gearstick bone: shortcut (no consumer in src).
- `Vehicle__update` §0 (0x49c984), needed for the wake path: when the car's root is asleep and
  `flags & 1` clear: `steer_copy=steer; gear_copy=gear; flags|=1`. Port it in `drive_cars`
  where sleeping roots are skipped (before the `continue`).

Not ported (each a `shortcut:` comment): flags 0x1000 speed limiter, 0x2000 / e+0xa34 steer
help, 0x10/0x20/0x40 auto gearbox / semi-auto / auto clutch (later card), 0x80/0x100/0x4000000
helpers, aero/slot-object axis lists (+0x2144/+0x2190), wheel slip average +0x2278, force
feedback getInfo 0x44, look axis 7, cheats 39/40, hinged-part release.

## Files (3 + check)
- src/game/car.h / src/game/car.cpp: CarData `float shift_end = -1, shift_t70 = -1,
  shift_t74 = -1; float horn_axis = 0; float gearset_last = 0;` (exe offsets in comments);
  `void control_cars(float dt)` (the list above for every CarData with a sim); the §0 bit0 port
  in `drive_cars`.
- src/script/natives.cpp `advance_sim`: static `ctrl_acc += step; if (ctrl_acc > 1e-4f)
  { game::control_cars(ctrl_acc); ctrl_acc = 0; }` before the sub-step loop (0x47e050).
- src/tools/vmboot.cpp: the drive check below.

## vmboot check (real data, hard)
After the card-146/147/149 checks, drive the freeride car with the synthetic SysKeyboard
(`g_input->devices[0].raw.keys[dik] = 0x80`; DIK codes from the shipped
`save/controls/active_control_set` group 1: Up 200 throttle, A 30 gear up, Left 203 / Right 205
steer). Each phase runs `loop.frame` like the existing code:
1. A held 10 frames, released 10 frames: `gear == 1`, `gear_ratio == ratio[1]`.
2. Up held 60 frames: `throttle_input > 0.9` while held; the car root is awake (input wake path).
3. Up + Left 60 frames, then Up + Right 60 frames: `steer_input > 0.9` / `< -0.9`.
Print gear, throttle, steer, car speed and rpm per phase (print only: no speed/rpm/yaw check,
the engine has 0 Nm until parts are installed, card 150; a drive check is a later card).
Release all keys afterwards.

## Stop rule
Max 2 build+vmboot runs per failing check. If an axis reads 0 (binding/ControlSet issue) or
the car stays asleep, report the numbers and stop; do not add invented control logic, do not
tune constants, do not debug the input layer beyond printing `get_axis_val(1/28/29)`.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`; report
  the per-phase numbers.

## Not this card
Auto gearbox/clutch 0x497930/0x4977a0 (+0x4b05a0/0x4b06b0) = later card (unnumbered); steer help; camera.
