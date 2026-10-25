---
id: 179
status: merged
branch: card/179
depends: [178]
---
# `native bot` mode 8 (grid, `AI_BeginRace`): 0x484ca0 random revving with the clutch in

## Why (notes/19 s8.2)
Script order per bot: `AI_suspend` (mode 1) -> `AI_BeginRace f` (mode 8, `+0x25dc = clamp01(f)`) -> at beginRace
`AI_spline` (mode 3). src's `control_bots` only runs mode 3 and the default branch; mode 8 writes nothing, so the
bot cars sit with zero axes during the countdown. The exe revs them randomly with the clutch disengaged, holds
1st gear and keeps brakes off.

## Exe evidence
`0x484b90` mode switch (ECX bot): `case 8: 0x484ca0(S)` (then the common steer slew, already in src).
```
0x484ca0 (ECX bot, S):
  saved = +0x224c                                   // throttle axis (kBotThrottle)
  if (0x482da0(S, 0, 0)) {                          // gate, src bot_gate(b, s, 0, 0)
      +0x224c = saved
      f = +0x25dc; if (f <= 0.1) f = 0.1             // begin_race
      r = rand()                                    // 0x571110 MSVC CRT rand, src msvc_rand() (natives.cpp)
      +0x224c = (f <= (float)(r & 0x7fff) * 3.0517578e-05) ? 0.0 : 127.0
  }
  queue EVENT_COMMAND "transmission 2" to controlled[active]   // 0x47d550, every tick (src bot_send)
  +0x22a4 = (S.gear > 0 || S.gear_axis(+0xbc) >= 127.0) ? 0.0 : 127.0   // kBotGear
  +0x22a0 = 127.0   // clutch    kBotClutch
  +0x2250 = 0.0     // brake     kBotBrake
  +0x229c = 0.0     // handbrake kBotHandbrake
  +0x2258 = 0.0     // nitro     kBotNitro
```
Notes: the throttle written by the gate when it returns 0 (127, or 127/127/127 while the engine latch is 0) is kept
(only the gate==1 branch rolls the dice). `S.gear_axis` is the snapshot copy of `+0x22a4` taken before 0x484b90
(`BotSnapshot::gear_axis`). `rand()` shares the CRT state with java.lang.Math.random (same `msvc_rand`).

## Files
- edit: src/game/car.cpp — new `static void bot_grid(BotData &b, const BotSnapshot &s)` next to `bot_launch`;
  in `control_bots` call it for `b.mode == 8` (the `if (b.mode == 3) ... else if ...` chain); declare
  `uint32_t msvc_rand();` (defined in src/script/natives.cpp) at file scope if no header exports it.
- read only: src/game/car.h (BotData, BotSnapshot), src/script/natives.cpp (msvc_rand)

## Done when
- `// 0x484ca0` comment on bot_grid; mode 8 dispatched from the 0x484b90 switch spot in control_bots
- `make -C src clean check` green (vmboot --race keeps all its current checks; bots still leave the grid only after
  AI_spline); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/179
