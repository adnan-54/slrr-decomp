---
id: 228
status: implemented
branch: card/228
depends: [226]
---
# Parts: intdamage parse follows the exe (fix for card 226 review)

## Goal
Make the part cfg `intdamage` -> `max_wear_inv` path match the exe (opus trace, read-only).

## Exe evidence
- cfg keyword `intdamage` (string 0x59efd4, table entry 0x5dac88) -> handler 0x4c0620:
  `sscanf(value, "%f", &M+0x121c)` only while `M+0x120 == 0`. No `> 0` check, no stop at
  the first match: **the last `intdamage` line wins**.
- Defaults come from the model constructors: `type_part__vf1` 0x4c3995 sets M+0x121c = 1e7,
  `type_car__vf1` 0x49ab64 sets 1e8.
- Read at 0x4c27ce (Part__initInstance, P+0xbc = 1/M+0x121c) and 0x499598 (createCar, divss,
  no guard).
- 287 shipped cfgs set `intdamage` (188 at 1e9, 47 at 1e7, others 10/0.6/100/0.1...).

## Files
- edit: src/game/car.cpp (fill_part, ~line 176-183), src/game/car.h (line ~173 default)

## Changes
1. In fill_part's `intdamage` loop remove the `break` so the last occurrence wins.
2. Drop the `if (max_wear > 0.0f)` guard (exe divides unguarded) OR keep it with a
   `// INVENTED:` comment (keep it: a zero would give inf).
3. `CarData`/part default of `max_wear_inv`: part model default `1.0f / 1e7f` (0x4c3995).
   Car/chassis model default `1.0f / 1e8f` (0x49ab64): apply where the chassis part is
   created if fill_part also runs for it; if you cannot tell, leave the chassis default and
   say so in the report (`shortcut:` comment).
4. Cite `// 0x4c0620` / `// 0x4c3995` / `// 0x49ab64` on the lines.

## Done when
- `make -C src clean check` green; no new tests or hand-made expected values
- race numbers may move slightly: report them
- card frontmatter: status implemented, branch card/228
