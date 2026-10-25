---
id: 175
status: implemented
branch: card/175
depends: [174]
---
# `native bot` tick: control-list hook 0x47f980, car snapshot, mode switch 0x484b90, mode 3 look-ahead 0x485e10, axes 0x480720

## Why (notes/19 s8.1)
Bots must tick and the bot cars must read the bot's axes. Exe: the brain's vf11 0x47f980 sits on the same mode-8
control list as `Vehicle__updateControls` 0x498300 (bot vf15 0x480720 returns 1 for GII 8), runs once per physics
tick with dt = ctrlAcc, after the cars (creation order), and the car reads the bot through
`getInfo(brain, 0x43/0x19, axis)`. This card ports the plumbing and the snapshot; the planner (176/177) and the
axis writers (178) come later, so the axes stay at their reset value 0 here.
Script does: nothing per frame (Bot.java only sends commands). Exe provides: everything below.

## Exe
```
0x47f980 bot vf11 (bot, dt):            // control list, ECX-less stdcall
  if mode(+0x259c) == 1 return
  (drop dead refs from controlled[]; when empty and +0x218c: camera event)   -- shortcut: src refs do not die
  car = controlled[active]; if !car return
  S = snapshot via getInfo(car, code)  (0x4a15c0 cases, notes/19 s8.1 table):
    S+0x00 gear (0x36) | +0x04 pos (2) | +0x10 vel dir: S+0xa4 = |v| 3D, then y = 0, xz normalised (3) |
    +0x1c fwd = -(R col 2), y = 0, xz normalised (4) | +0x28 R^T omega (0x33) | +0x34 Euler 0x569970(R) (0x32) |
    +0x40 wheels min(n,4), per wheel 6 floats f0..f5 (0x35), f2 of wheel i /= bot+0x2710+4i (1.0) |
    +0xa8 dt | +0xac rpm = car+0x1e80 * 9.549296 | +0xb0 redline e+0x1c (7000 default, 6000 typed+0x50 == 0) |
    +0xb4 wheelbase (0x40, 2.0 default) | +0xb8 nitro frac | +0xbc = bot+0x22a4 | +0xc0 = 0
  +0x25d4 = car+0x1e80 > e+0xf0 ? 1 : (car+0x1e80 < 1 ? 0 : keep)
  0x484b90(S); (tail: debug object/HUD only)
0x484b90 (S): old = +0x21e0; switch mode: 3 -> 0x485e10(S); 1 -> nothing; 2/4/5/6/7/8 -> other modes;
  default -> 0x482910(S, 0, 0)
  r = +0x2598 (510) * S.dt; +0x21e0 = clamp(+0x21e0, old - r, old + r)
0x485e10 (S): +0x2580 = +0x2588 * 1.2; +0x2584 = min(+0x258c * 2, 1)
  if spline: look = min(|v| * +0x2558 + +0x255c, +0x2554)       // ctor 10, 20, 55
             if 0x414830(S.x, S.z, &d, &lat): +0x268c = d - look; 0x486020(S, look, 1000); return
  0x482910(S, 0, 0)
0x480720 bot vf15 (bot, code, arg): 8 -> 1; 0x15 -> +0x242c; 0x2a -> mode;
  0x19 -> arg in 1..0x4b ? (int)axis[arg] : 0;   0x43 -> arg in 1..0x4b ? axis[arg] * 0.007874016 : 0
  axis[v] = bot+0x21dc + v*4: v1 steer +0x21e0, v28 throttle +0x224c, v29 brake +0x2250, v31 nitro +0x2258,
  v48 handbrake +0x229c, v49 clutch +0x22a0, v50 shift +0x22a4
Ctor 0x47ed10 / reset 0x484a40 values: notes/19 s8.1 (store the ones used here and in 176-178 in BotData).
```

## What to change (<= ~150 lines; files: src/game/car.h, src/game/car.cpp, src/script/natives.cpp)
1. car.h `BotData`: `float axis[0x4c]` (raw, index = axis id), the ctor/reset scalars (+0x2554..+0x2598, +0x25d4,
   +0x25d8, +0x2608, +0x268c, +0x2710..+0x271c, +0x2724) and a `BotSnapshot` struct = S (named fields, offsets in
   comments). AI_suspend/BeginRace/AI_spline already call reset: make reset 0x484a40 zero the axes it zeroes.
2. car.cpp `control_bots(float dt)` = 0x47f980 for every g_bots entry (std::map order is not creation order:
   INVENTED: iterate g_bots by resource id, which follows creation; say so) -> snapshot from the controlled
   car's CarData/sim (same sources control_cars and the Chassis natives use; case 2's y correction from the
   wheel records if available, else `INVENTED:` body y) -> 0x484b90 with mode 3 -> 0x485e10; the 0x486020 call is
   a stub that stores `d`, `look` and returns (card 176). Other modes: no-op (`shortcut:` card 179).
3. car.cpp control_cars: when `slot_player[driver]` is a bot (`g_bots`), read axes from it:
   `ax(v) = axis[v] * 0.007874016`, `ai(v) = (float)(int)axis[v] * (1/127)` (0x480720 0x43 / 0x19).
4. natives.cpp: call `game::control_bots(ctrl_acc)` right after `game::control_cars(ctrl_acc)` (list order: cars
   were created before the brains).

## vmboot checks (--selfcheck and --race, drive-start block, per mode-3 bot)
1. **Hard check**: after the first physics tick past beginRace, each bot's snapshot exists, `S.pos.xz` equals its
   car body position xz (exact), `S.speed` equals |body velocity|, `S.dt` equals the tick's ctrl_acc, 4 wheels,
   sum of wheel loads f1 > 0 (cars rest on the grid), gear equals the car's gear.
2. **Hard check**: `+0x268c == d - min(10 * S.speed + 20, 55)` with d from `path_project(S.x, S.z)`, and every bot
   car's control_cars took the bot branch (counter) with throttle/brake/steer 0 (axes still 0).
3. Print per bot: pos, speed, d, look, wheel loads.

## Done when
`make -C src -j4 check` green; vmboot `--selfcheck` and `--race` (ulimit -v 1500000; timeout 300) PASSED with 1-2.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`). `INVENTED:` for every gap with the exe address.
No unit tests, no map names in engine code. Do not write axes (card 178).

## Not this card
0x486020 body (176/177), 0x482910 (178), modes 2/4-8, debug tail of 0x47f980, bot_trigger (179).

## Result
Branch `card/175`. Implemented:
- `car.h`: `BotData::axis[0x4c]` (+0x21dc), ctor/reset scalars (+0x2554..+0x2598, +0x25d4, +0x268c,
  +0x2710..+0x271c, +0x2724), named `BotSnapshot` with the S offsets in comments; `control_bots(float)`.
- `car.cpp`: `bot_reset` now clears axes 1/28/29/49 and the new state (0x484a40); `bot_snapshot` builds S
  from CarData/sim (cases 2/3/4/0x36/0x33/0x32/0x35/0x40/0xac/0xb0/0xb8/0xbc); `bot_mode3` = mode-3 arm of
  0x484b90 + 0x485e10 with the 0x486020 call stubbed to store d/look; `control_bots` = 0x47f980 (mode 1 skip,
  controlled ref, car lookup, snapshot, engine latch +0x25d4, mode 3, steer slew +0x2598*dt); other modes
  no-op (cards 176-179). `control_cars` bot branch reads `ax/=127.0f` (0x43) and `ai=(int)` (0x19) from
  `axis[]`; controller path unchanged.
- `natives.cpp`: `control_bots(ctrl_acc)` after `control_cars` in `advance_sim`.
- `vmboot.cpp`: card-175 block after the settle: one forced `control_bots(0.016f)` then per-bot snapshot
  checks (pos xz == root body pos, speed == |velocity|, dt, 4 wheels, load sum > 0, gear, plan
  `+0x268c == d - min(10*speed + 20, 55)`, bot control branch + zero throttle/brake/steer) and print.
  The "no hull contact at rest" check now only counts the player's root/chassis bodies, since the bot
  cars are woken for these checks.

INVENTED:
- 0x47f980: iterate `g_bots` sorted by resource id (the exe walks creation order; `g_bots` is a pointer map).
- Vehicle y in the snapshot: wheel-hub y minus (radius + rest length - (9.81*m/n)/spring) averaged over 4
  wheels as the exe does; plain root body y when there are no wheel records (0x4a15c0 case 2).
- Redline when the typed car has no engine record: 7000 (exe: 7000 default, 6000 for typed+0x50 == 0).
- c0 of the snapshot = 0 (same as reset; 0x4a15c0's source was not read for that case).
- vmboot fixture: after the player wakeup the harness also queues "wakeup" to each bot car (axes stay 0, so
  no script input would ever wake them) and queues "suspend" after the checks; the exe sleeps until the
  card-178 update path runs.
- vmboot forces one `control_bots(0.016f)` after the settle so S.dt is the checked value.

Checks: `make -C src clean check` green (vmboot selfcheck 161 checks / race 151 checks, all PASSED).
Bots print pos/speed/d/look/loads; e.g. bot0 (33.975, 31.534) speed 0.2213 d 2141.44 look 22.21 loads
0.211/0.266/0.332/0.405.

Doubts: snapshot case 2/3/4 details come from the card's table only (no rea access); the bot cars stay at
rest because card 178 does not write the axes yet; the forced snapshot call means check 1 is evaluated
one tick later than "first tick past beginRace".
