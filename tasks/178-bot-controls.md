---
id: 178
status: implemented
branch: card/178
depends: [177]
---
# `native bot` controls: 0x482910 + gate 0x482da0 + drive 0x483220 + launch 0x4830a0 + reverse 0x482f50 -> axes; bots drive

## Why (notes/19 s8.1)
After card 177 the bot has (steer angle, speed target) per tick but writes no axes, so the bot cars stay on the grid.
This card writes the axis table that card 175 already feeds into the car (0x480720 -> 0x498300) and sends the
gearbox commands. Exe: once per physics tick (control list), the car applies the axes on its next control pass.
Script does: race positions (CircuitRacing.handleRacePos :720-780, `rpTotal[i+dlt]` for bot i) and bot laps.

## Exe (ECX = bot; S = snapshot of card 175; "send(x)" = queue EVENT_COMMAND x to controlled[active] (0x47d550))
```
0x482910 (S, steer, speed):
  if (|S.euler[2]| (S+0x3c) > 1) { old = +0x25c8; +0x25c8 += S.dt
      if (old < 5 && +0x25c8 >= 5) queue "ai_info_rolled" (code 0x10) to the bot's parent (+0xc ref)
      +0x22a0 = +0x224c = +0x2250 = +0x229c = +0x21e0 = +0x2258 = 0; return }
  +0x25c8 = 0
  switch (+0x25bc) { 1: if (S.speed < 0.5) { +0x25bc = 4; +0x25c1 = 1 }  2: ... -> 5   3: ... -> 6   7: +0x25bc = 0 }
  if (+0x25bc in 4..6) { 0x482f50(S, steer); return }
  switch (+0x2590) {
   0: send("abs 0"); send("asr 0"); send("steerhelp 0")
      t = getInfo(car, 0x34)  (0x4a15c0 case 52: driven-wheel layout; 0 none, 1, 2, 3, 4 cross)
      +0x2590 = t==1 ? 4 : t==2 ? 2 : t==3 ? 3 : 1  (1 also for 0/4/other, with a log line)
      if (+0x2590 == 1) +0x2590 = 4
      0x482910(S, steer, speed); return
   2,3,4,5: 0x483220(S, steer, speed)    // then fall into the common tail
   default (common tail):
      if (+0x26a4) 0x4830a0(S)
      if (S.speed < 1 && +0x224c > 16 && S.gear < 7) {
          if (+0x25d0 < -0.5) { +0x25cc = 1; return }
          if (now - +0x25d0 <= 3) return
          +0x25c1 = 1; +0x25bc = 5 }
      +0x25d0 = -1 }
0x482da0 gate (S, steer, speed) -> bool:
  +0x22a0 = 0; +0x224c = 127; +0x2250 = 0; +0x21e0 = 0; +0x2258 = 0; g = S.gear
  if ((g > 0 && g != 7) || speed <= 1e-4) {
    if (g > 6 || speed >= -1e-4) {
      if (g == 0 || |speed| > 1e-4 || |S.speed| > 1) {
        +0x22a4 = 0
        if (+0x25d4) { if (+0x25c1) { +0x25c1 = 0; +0x25c4 = now } if (+0x25cc) { +0x25cc = 0; +0x25d0 = now } return 1 }
        +0x224c = 127; +0x2250 = 127; +0x22a0 = 127; return 0 }
      if (+0x22a4 != 0) { +0x22a4 = 0; return 0 }
      +0x22a4 = (g != 7) ? -127 : 127; return 0 }
    if (+0x22a4 > -127) { +0x22a4 = -127; return 0 }
  } else if (+0x22a4 < 127) { +0x22a4 = 127; return 0 }
  +0x22a4 = 0; return 0
0x483220 drive (S, steer, speed):      // W_i = wheel i of S: f1 load, f2 grip, f3 slip-x, f5 (S+0x44 + i*0x18)
  if (!gate(S, steer, speed)) return
  sk = min(2 * +0x26a8, 1); load = W0.f1 + W1.f1 + W2.f1 + W3.f1; cap = 1
  type 2: ws = (W1.f3 + W0.f3) * 5 + W3.f3 + W2.f3          | type 3: ws = sum f3, cap = 10
  type 4: ws = (W1.f3 + W0.f3) * 3 + W3.f3 + W2.f3, cap = 5 | else ws = steer
  ws /= (S.speed + 1)
  g = (W0.f2 + W1.f2 + W2.f2 + W3.f2) * 0.25; if (+0x25d8 < g) +0x25d8 = g
  y = S.speed * 0.4 * 0.036; y = (y*y + 0.2) * S.yawrate(S+0x2c); y = y >= 0 ? max(y - 0.05, 0) : min(y + 0.05, 0)
  st = steer; if (y * steer <= 0 && |steer| < |y|) { st = 2*steer + y; st = steer >= 0 ? max(st, -0.1) : min(st, 0.1) }
  fd = max((S.speed - 10) * 0.0005, 0); st = st >= 0 ? max(st - fd, 0) : min(st + fd, 0)
  s3 = st / max(S.dt * S.speed * 2, 1e-6) * S.wheelbase
  if (|s3| > |st| * 1.2) s3 = st * 1.2; else if (|s3| < |st| * 0.2) s3 = st * 0.2
  stc = clamp(s3, -0.75, 0.75)
  u = S.wheelbase / (S.speed^2 / (((W0.f2*W0.f1 + W1.f2*W1.f1) * 9.81 + 1e-4) / (W0.f1 + W1.f1 + 1e-4)) + 1e-4)
  u = clamp(u, 0, 0.75); if (S+0xc0) u *= 4; else if (mode == 3) u *= 2
  a = acos(S.fwd.z * S.vel.x - S.fwd.x * S.vel.z) - 1.5707964
  hR = (max(a, 0) + u) * 169.33333; hR = hR < cap ? cap : min(hR, 127)
  hL = (u - min(a, 0)) * 169.33333; hL = hL < cap ? cap : min(hL, 127)
  +0x21e0 = clamp(stc * -169.33333, -hR, hL)
  sp = min(S.speed, speed); o = +0x25fc; dc = max((+0x25f8 - sp) / +0x25f4, 0); +0x25fc = dc
  sp2 = speed - (o + dc) * 0.5 * S.dt; +0x25f8 = sp; ps = +0x2600; +0x2600 = stc; +0x25f4 = S.dt
  v = sp2; as = |stc|
  if (as > 0.375 && (|ps| - as) / +0x25f4 < 0.2) {
      vc = sqrt(+0x25d8 * 9.81 * S.wheelbase / (2 * (as - 0.375))); if (S+0xc0) vc = max(S.speed, vc); v = min(sp2, vc) }
  e = clamp((0.100000024 - (S.speed - v) / ((v + 1) * 0.1)) - clamp(2 - v, 0, 2), -1, 1.1)
  if (S+0xc0) ws *= (S+0xc4) + 2; else if (mode == 3 && S.nitro > 0) ws *= 0.3
  c0 = clamp(int(mode == 3 ? load * 500 + 40 : load * 400 - 50), 0, 128)
  c1 = clamp(127 - int(|ws| * 150 - 20), 0, c0); c2 = clamp(128 - int((sk - 1) * -28), 0, c1); ct = min(c2, 127)
  thr = clamp(int(e * 127), 0, ct); +0x224c = thr; +0x22a0 = clamp(127 - thr * 10, 0, 127)
  if (S.nitro > 0 && e > 1.05 && c2 == 128 && S.redline * 0.33 < S.rpm && S.gear > 1) +0x2258 = 127
  br = clamp(int(e * -127), 0, 127); sf = max(S.speed, 10); m = S+0xc0 ? 2 : 1
  cF = clamp(127 - int((|(W1.f3 + W0.f3) * 0.5 / sf| - 0.05) * 423.3333 * m), 0, 127)
  cR = min(max(127 - int(((W0.f5 + W1.f5) * 211.66666 / sf - 21.166666) * m), 0), cF)
  cL = clamp(int(load * 400 - 50), 0, 127)
  p1 = clamp(127 - int(|S.yawrate + stc| * 300), 0, cR); p2 = clamp(127 - int(|S.yawrate + stc| * 200), 0, cL)
  brake = min(br, min(p2, p1)); +0x2250 = brake; +0x229c = clamp(br - brake, 0, p2 - brake)
  if ((|+0x21e0| <= 5 || S.rpm >= S.redline * 0.9 || S.rpm <= S.redline * 0.2) && load >= 0.001) send("transmission 5")
  else send("transmission 0")
0x4830a0 launch (S):
  if (+0x2250 <= 0 && +0x229c <= 0 && (S.rpm <= S.redline * 0.8 || |W0.f5 + W1.f5 + W2.f5 + W3.f5| / (S.speed + 1) >= 0.3
      || S.speed <= 5)) {
    send("transmission 2"); +0x22a4 = (S.gear > 0 || S.bc(S+0xbc) >= 127) ? 0 : 127
    +0x224c = 127; +0x2250 = 0; +0x229c = 0; +0x2258 = 127; +0x22a0 = max(80 - S.speed * 20, 0); return }
  send("transmission 5"); +0x26a4 = 0
0x482f50 reverse (S, steer):
  if (gate(S, -steer, -1.0)) { +0x2710..+0x271c = 1.0; 0x4876a0 (send "ai_cheat %.5f x5" of +0x270c..+0x271c, target
      not traced: shortcut); +0x224c = 64; +0x2250 = 0; +0x21e0 = clamp(steer * 169.33333, -127, 127)
      if ((S.speed < 1 && now - +0x25c4 > 2) || (|steer| < 0.5 && now - +0x25c4 > 0.3)) { +0x25bc = 7; +0x224c = 0; +0x2250 = 127 }
      +0x22a0 = 0 }
```
"now" = `*PTR_DAT_005dadf0` (game time, the src world/game time used by 0x482630's +0x2720).

## What to change (<= ~150 lines + the pasted logic; files: src/game/car.h, src/game/car.cpp)
`bot_controls(Bot&, S, steer, speed)` = 0x482910 with the four callees, called from card 177's `bot_steer` and from
the 0x484b90/0x485e10 `0x482910(S, 0, 0)` fallbacks. getInfo(car, 0x34) from the car's driven-wheel layout (find the
src equivalent of 0x4a15c0 case 52; if missing: `INVENTED:` with the address). Commands go through the existing
car command path (`transmission`, `abs`, `asr`, `steerhelp` handlers; unknown words are ignored like the exe).
Keep int truncation (`int(...)`) where the exe converts.

## vmboot checks (--race, plus --selfcheck unchanged)
1. **Hard check**: every bot car moves: displacement from its grid pose > 50 m by the end of the race window.
2. **Hard check**: the script's race positions fire for bot cars: `rpTotal[1..racers]` all > 0 at the end of the
   window (CircuitRacing.handleRacePos :760-780, EVENT_TRIGGER_ON from the rp triggers, card 168 contacts).
3. No failed script thread; existing checks unchanged. Print per bot: distance driven, rpTotal, max speed, gear.

## Done when
`make -C src -j4 check` green; vmboot `--race` and `--selfcheck` (ulimit -v 1500000; timeout 300) PASSED with 1-3.

## Rules
**Stop and report after 2 failed runs** (status `blocked`, Result: first blocker, e.g. which axis stays 0 or where
the car stops). `INVENTED:` with the exe address for any gap. No unit tests, no map names in engine code, never
"improve" the exe logic (bots may crash into each other: obstacles are card 179).

## Not this card
Mode 8 grid behaviour 0x484ca0, bot_trigger obstacles, opponent block, `ai_cheat` target (179).

## Result
Implemented (src/game/car.cpp, src/game/car.h, harness in src/tools/vmboot.cpp): `bot_controls` = 0x482910
with `bot_gate` 0x482da0, `bot_drive` 0x483220, `bot_launch` 0x4830a0, `bot_reverse` 0x482f50; roll block
(+0x25c8, "ai_info_rolled" to parent), reverse state +0x25bc, drive-type probe +0x2590, `abs/asr/steerhelp`
+ `transmission` through the existing car command path, `int()` truncation kept. Called from bot_steer (card
177) and from both 0x484b90/0x485e10 fallbacks. Everything works far enough that 3 of 4 bots drive and the
race-position triggers fire.

Finish (Claude, 2026-10-25): both blockers had one root cause, not the controls. The car type cfg body line
`_phys_chassis_water_object.scx` is followed by `noclick` / `buoy` / `nocollision`; src ignored the modifier
lines, so the buoyancy hull collided with the map soup. Exe: `nocollision` 0x4bfba0 sets record flag 0x100,
0x4c1f90 turns it into child body flag 0x20, and the 0x4feb20 child recursion skips flag-0x20 children.
(1) Bot 2 (Codrac) rested on that hull: it hangs 3-8 cm below the at-rest tyre plane, so the rear wheel rays
stayed at max length (loads 0), and with load 0 0x483220 capped the throttle at 40 (mode 3). (2) The player's
water hull reached a wall top's down-facing face at frame 3425 (the player line moved ~0.1 m because it had
passed close to stalled bot 2; no contact with a bot at the fall); the 0x4fc7e0 deep branch pushed the car
0.5 m down through the road. Fix: CfgBody.flags (`nocollision` -> 0x100) and type body records with it give no
soup hull (src/parts/cfg.*, src/game/car.cpp). Deep-branch hull contacts per race ~30000 -> 4. notes/09.
Card-175 checks: `load_sum > 0` restored in the snapshot check; the wiring check stays `control_ticks > 0`
(equality inputs == axes/127 fails because the bot tick rewrites the axes after the car's control pass).
names.json: 0x47f980 = `type_BOT__vf11_tick` (was Camera__chaseAndDebugHud).
Final: --selfcheck PASSED (166 checks), --race PASSED (156 checks); race: bots drove 87/968/66/102 m from the
grid at the window end (all lapping), rpTotal[1..4] = 238/252/266/228, player finished lap 2 (fWindow set,
min y -5.23, max |v| 30.83); `make -C src clean check` green.

INVENTED:
- `driven_layout()` for getInfo(car, 0x34): src has no case-52 field; counts `wheels[i].drive` front/rear ->
  all/front/rear/none, else 4 (0x4a15c0 case 52 not in notes/09). `INVENTED:` in the code.
- `abs/asr/steerhelp` handlers are flag-only (0x80/0x100/0x200, steerhelp inverted); the flags at +0xa34/
  +0xa3c/+0xa40 have no consumer yet (`shortcut:` in the code).
- 0x4876a0 `ai_cheat` target not traced: `shortcut:` in bot_reverse.
