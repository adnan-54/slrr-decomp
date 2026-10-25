---
id: 175
status: todo
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
