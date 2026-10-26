---
id: 224
status: implemented
branch: card/224
depends: []
---
# Start values: measure globals (fixes vmboot `hud: speed=999`) + car wear rates

## Goal
src starts `measure`, `measure_per_hour` and `measure_inv10` at 0 until a script calls
`System.setMeasure` (shipped scripts only do that from OptionsDialog). The exe has start values, so the
speed gauge divides by 0 in src (`v / measure_per_hour` = inf -> clamped to the gauge max, 999).
Give the Scheduler fields the exe's start values. Also give the car-level wear rates their
Vehicle__createCar start values (needed by card 225: a 0 engine rate is a divisor). Evidence: notes/34 §0, §2.

## Exe evidence
- `0x5da524` measure: initialised .data, bytes `00 40 c9 44` = `0x44c94000` = **1610.0f**.
- CRT dynamic initialiser at 0x409af0 (runs before main):
  `movss xmm0,[0x5da524]; mulss xmm0,[0x59dd50]; movss [0x5fe378],xmm0`
  with `0x59dd50` = `0x3991a2b4` (= 0.00027777778f, the same constant setMeasure uses).
- CRT dynamic initialiser at 0x409a90:
  `movss xmm0,[0x59373c]; divss xmm0,[0x5da524]; movss [0x5fe374],xmm0` with `0x59373c` = 10.0f.
- 0x4e57b0 `System.setMeasure` is the only other writer (already ported, `system_set_measure`).

### 0x4992e0 Vehicle__createCar (car+ offsets; src CarData has 0 for all four rates)
```
car+0x1e9c engine_dmg = 0      car+0x1ea0 engine_rate   = 0x3356bf95 (5.0e-8f)
car+0x1ea4 trans_wear = 0      car+0x1ea8 trans_rate    = 0x3356bf95
car+0x1eb4 clutch_wear = 0     car+0x1eb8 clutch_rate   = 0x3356bf95
car+0x1eac radiator_dmg = 0    car+0x1eb0 radiator_rate = 0x36a7c5ac (5.0e-6f)
```
`Vehicle__refreshParameters` (forceUpdate) does not reset these; only installed-part records
(kinds 8/9/10/12, `apply_part_wear_records`) overwrite them.

## Files
- edit: src/script/natives.cpp (struct Scheduler, the three fields near line 160)
- edit: src/game/car.h (CarData engine_rate, trans_rate, clutch_rate, radiator_rate initialisers;
  write bit-exact values, e.g. the shortest decimal that round-trips to that bit pattern, and put the
  hex in the comment)

## Change
```
// 0x5da524 .data 1610.0; 0x409af0 / 0x409a90 CRT initialisers derive the other two.
float measure = 1610.0f;
float measure_per_hour = 1610.0f * 0.00027777778f;   // 0x5fe378
float measure_inv10 = 10.0f / 1610.0f;              // 0x5fe374
```
Remove the `INVENTED: the exe's initial measure was not traced` comment. Compute the two derived values
exactly like that (float multiply / float divide), not as decimal literals.

## Done when
- `make -C src clean check` green; the `--race` output line `hud:` shows a speed number instead of 999
  (do not add a check with a hand-made expected speed)
- card frontmatter: status implemented, branch name
