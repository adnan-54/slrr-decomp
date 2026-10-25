---
id: 173
status: implemented
branch: card/173
depends: [172]
---
# `native bot`: create (0x4803a0) + command state (0x480850) + AI_spline file load (0x482630), no driving

## Why (notes/19 s8)
Probe 2026-10-24 (scratch copy of vmboot): the career event `JuniorCurcuit` has `racers` 4, `gBot.length` 4, every
`gBot[i].car` exists (14 car instances with bodies), but every `gBot[i].brain` (`GameRef(world, sl:0x6Er, "",
"BOTBRAIN")`, Bot.java:217) is a plain instance with event mask 0: all `controllable`/`AI_*` commands are dropped.
Script does: Bot.enterCar sends `AI_level n`, `renderinstance id`, `controllable <car id>`, `AI_suspend`;
Gamemode.prepareBots sends `AI_BeginRace 0.5` then `AI_BeginRace 1.0`; CircuitRacing.beginRace -> applySplines ->
Bot.followSplineTrack = `AI_spline <width> <splinesAI[k]> <opponent car id>` (bot 0: splinesAI[0] vs player car;
bot i>0: splinesAI[1], [2], then [0] again (vi wraps), opponent = gBot[i-1].car).
Exe provides: the `native bot` object and its command parser. This card only creates it and stores the command
state; nothing drives (planner = card 174).

## Exe (all `native bot`, vtable 0x59aaf0; sl.rpk record 0x6E rsd `native bot`)
```
0x4803a0 vf create(param_1 = type?, params):
  flag = 0; f = 0.5; scan(params, "%d,%f", &flag, &f)        // Bot.java passes "" -> defaults
  o = new 0x2730 (ctor 0x47ed10); o+0x2690 = flag != 0
  o+0x26a8 (skill) = ((clamp01(f) - 0.5) + 1) * 0.5           // 0.5 for the default
  o+0x230c (controlled count) = 0; o+0x2424 (active index) = 0; ...
  0x4ebd90(o, 0x10, 0)                                        // event mask 0x10 (traced, not INVENTED)
0x480850 vf14 (this, ?, code, str): returns 1 for every code (no script on the type).
  code 0x10: word = scan "%s"; stricmp (0x57c11e) chain, 31 words in this order:
  device viewport osd renderinstance camera controllable leave control lost team AI_suspend AI_NightRace AI_race
  AI_BeginRace AI_spline AI_follow AI_GoToTraffic AI_GoToTrafficSlow AI_GoToTarget AI_stop AI_level AI_smooth
  AI_horn AI_params AI_throttle AI_turn AI_brake AI_handbrake AI_clutch AI_gear AI_nitro
  "controllable": id = "%*s %d"; if id already in list (+0x2318 + i*0x10, i < +0x230c) ignore;
                  else ref(id) -> 0x4d2ab0(this, ref)  (same function as the native player, notes/09 s:
                  car gets "activate 0x<bot id> 0x<+0x218c> 0x<+0x219c>" and "damage_multiplier %.3f" (+0x21c4,
                  ctor 0), and the bot itself gets "control 0x<car id>")
  "control":      id = "%*s %d" (0x527140 scanner reads 0x.. too); ignore if already listed; else append ref
                  at +0x2310 + n*0x10, and if getInfo(7) (0x47c640) == 5 (vehicle): +0x2424 = n; n++.
  "AI_suspend":   0x4877a0: 0x484a40 reset; mode +0x259c = 1
  "AI_BeginRace": f = 1.0; "%*s %f"; 0x4828e0(clamp01(f)): 0x484a40 reset; mode = 8; +0x25dc = f
  "AI_spline":    "%*s %f %s %d" -> width, file, opp; 0x482630(width, file, opp)
  "AI_level":     f = +0x26a8; "%*s %f"; +0x26a8 = ((clamp01(f) - 0.5) + 1) * 0.5
  others: accept (return 1), no state in this card.
0x482630 AI_spline(width, file, opp):
  0x484a40 reset (frees the old spline +0x2604); base = this+0x2594 + width; spline = new 0x3c (0x411fb0)
  buf = File__readAll(file) (0x565610); per line (0x569350):
    w = base; f1 = 1.0; i1 = 1; f2 = 10000; f3 = 10000; i2 = 1
    n = scan("%lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %d", x,y,z, dx,dy,dz, w, f1, i1, f2, f3, i2)
    if n > 5: if w < 0: w = base; first line only remembers the point, every later line adds segment
              (prev pos/dir, this pos/dir, w, f1, i1, f2, f3, i2) via 0x4122b0 (0x54-byte record)
  mode +0x259c = 3; opponent ref +0x25e0 = opp; +0x2720 = game time; +0x26a4 = 1
  queue "transmission 0" to the active controlled car (+0x2310 + active*0x10)
  if no segment was added: 0x484a40 again (spline freed)
```
Shipped `.spl` lines have 7 fields (`x y z<TAB>dx dy dz<TAB>width`, CRLF), Norisring route_1..3 have 38 lines each.

## What to change (<= ~150 lines; files: src/game/car.h, src/game/car.cpp, src/game/runtime.cpp + vmboot)
1. car.cpp create hook (next to the `native player` block, `has_native_ref(rt, type, "bot")`): event mask 0x10,
   insert a `BotData` (new struct in car.h: id flag, skill, `std::vector<Runtime::Resource*> controlled`, active,
   mode, begin_race, opponent id, spline file, `struct SplinePoint {Vec3 pos, dir; float w, f1; int i1; float f2,
   f3; int i2;}` vector, segment count) into a `g_bots` map; parse the create params with "%d,%f" as above. Clear
   g_bots in `reset` like g_players. Export `const BotData *bot_data(Runtime::Resource *)` for vmboot.
2. car.cpp `bot_command(rt, bot, code, param)` = 0x480850 code 0x10 for: controllable (reuse what player_command
   does for the car, plus queue "damage_multiplier 0.000" to the car and "control 0x<car id>" to the bot itself),
   control, AI_suspend, AI_BeginRace, AI_level, AI_spline (file via `rt.resources().resolve_path(file)` +
   `fs::read_file`, as runtime.cpp load_ground does for File__readAll; store points as read, count segments =
   lines with >5 fields minus 1; queue "transmission 0" to `controlled[active]`). Every other word: return 1.
   Use scan_id for the ids (0x527140 reads 0x..). Width/float scans: std::sscanf is fine.
3. runtime.cpp queue_event: add `!game::bot_command(...)` to the native handler chain.

## vmboot checks (--selfcheck, in the card-167 "drive start" block where raceActive is 1, i.e. after beginRace)
1. **Hard check**: for each `gBot[i]` (i < gmcEvent.racers, 4): `brain` resolves to a resource with `bot_data`,
   `controlled.size() == 1`, `controlled[0]` is `gBot[i].car`'s resource, active 0, and that car's
   `slot_player` holds the brain (activate from 0x4d2ab0).
2. **Hard check**: mode 3 and begin_race 1.0 (AI_suspend -> 1, BeginRace 0.5/1.0 -> 8, AI_spline -> 3), spline file
   == `splinesAI.elementAt(k)` with k = 0,1,2,0 for bots 0..3, opponent id == player car id for bot 0 and
   `gBot[i-1].car` id for i > 0, points 38, segments 37, every point width 2.0 (the file's 7th field).
3. Print per bot: skill, cmd width, file, points; all existing checks pass in --selfcheck and --race.

## Done when
`make -C src -j4 check` green; `ulimit -v 1500000; timeout 300 src/build/vmboot --selfcheck` and `--race` PASSED
with 1-3. Result: the per-bot print, any check that changed because bot cars now get activate/transmission.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, Result names the first blocker). No driving, no
per-frame update, no spline math (Hermite build 0x414fe0, length): store the file rows only. Mark gap logic
`INVENTED:` with the exe address it replaces. No unit tests, no map names in engine code (only vmboot may).

## Not this card
Planner/update (0x47f980, 0x484b90, 0x486020), controls (0x482910), bot_trigger (0x483bd0), renderinstance/camera/
osd/leave/AI_NightRace/AI_race/AI_follow/AI_GoTo*/AI_params/AI_throttle..AI_nitro (cards 174-176 or later).

## Result
- `BotData` payload + create hook (`has_native_ref(rt, type, "bot")`: mask 0x10, flag/skill from "%d,%f")
  and `bot_command` 0x480850 (controllable -> activate/damage_multiplier/control, control, AI_suspend,
  AI_BeginRace, AI_level, AI_spline) in src/game/car.*; `bot_command` added to the native chain in
  Runtime::queue_event; `bot_data` exported for vmboot.
- AI_spline 0x482630: rows scanned with the exe's 12-field format, points stored, segments = rows-1,
  mode 3, opponent, spline path resolved through `ResourceManager::resolve_path` + `fs::read_file`.
  No Hermite build, no per-frame planner (card 174).
- vmboot drive-start block: both hard checks pass. 4 racers, brain 0x80000871, cars 0x80000822/…, each
  brain controls exactly its car (slot_player), mode 3, beginRace 1.0, 38 points / 37 segments, point
  width 2.0, opponent = player car for bot 0 and the previous bot's car after; per-bot skill 0.750,
  cmd widths 1.515 / 4.855 / 0.507 / 1.030, route_1/2/3/route_1.
- `make -C src -j4 check` green; `--selfcheck` 155 checks / 0 failed, `--race` 145 / 0 failed. No other
  check changed (the bots' transmission 0 leaves the player checks untouched).
- INVENTED: "damage_multiplier 0.000" text (car stores nothing), spline `base = this+0x2594 + width`
  uses 0 for the untraced +0x2594, controllable duplicate test uses the control list (+0x2310), and
  getInfo(7)==5 is g_cars membership.
