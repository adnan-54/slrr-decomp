---
id: 223
status: merged
branch: card/223
depends: [221, 222]
---
# HUD: damage indicator colours + engine/tyre part refs + vmboot readout

## Goal
Port the HUD damage indicator of `type_car__vf13` 0x494910 (block 0x495946..0x495e07): one colour per
damage letter (body, engine, clutch, transmission, radiator, 4x tyre/brake/suspension) into
`CarData::hud_dmg[]` (card 221), set the two part refs it reads that card 222 does not (engine ref from
`Car__partReport` 0x4942b0, tyre refs from `Vehicle__refreshParameters`), and print the HUD once in the
vmboot race so it is observable. Evidence: notes/33 §2-3.

## Exe evidence
### 0x4b0f80 Hud__damageColor(x)
```
if (1.0 <= x) return 0xff00ff00
if (x <= 0.0) return 0xffff0000
if (0.5 <= x) return 0xffffff00 - ((int)(x*512.0 - 256.0) << 16)    // (int) truncates
return 0xffff0000 + ((int)(x*512.0) << 8)
```
### 0x4b1cd0 ResRef__partTear(ref): part payload of ref -> PartData::tear (+0xb4); 0.0 when null/unresolved
### Block 0x495946..0x495e07 (inside the per-slot gauge loop, after the gauges, same gate)
hud_dmg index: 0 A, 1 B, 2 C, 3 D, 4 E, 5 T, 6 L, 7 O, 8+i tyre, 12+i brake, 16+i susp (i < 4)
```
t = car part tear (same lookup as control_cars: g_parts[car resource]->tear)       // car+0xb4
c = (0 < t) ? color(t) : 0xff000000;  A = B = C = D = c
E = engine_part ? (y = (1 - engine_damage) * tear(engine_part), y > 1 ? 0xff00007f : color(y)) : 0xff000000
x = 1 - clutch_wear
T = clutch_part ? (y = x * tear(clutch_part), y > 1 ? 0xff00007f : color(y))
                : (gears(e+0x20) > 0 ? color(x) : 0xff000000)
L = clutch_part ? (y = x * tear(trans_part),    y > 1 ? 0xff00007f : color(y)) : color(x)   // sic: clutch test
O = clutch_part ? (y = x * tear(radiator_part), y > 1 ? 0xff00007f : color(y)) : color(x)   // sic
for i < wheel count (NOT capped at 4: exe element i>=4 overwrites the next group; in src write
                     index 8+i / 12+i / 16+i only while < 20, mark the clamp `INVENTED:`):
   tyre[i]  = W.tyre_part ? color((1 - W.tyre_wear) * tear(W.tyre_part)) : 0xff000000   // no >1 branch
   brake[i] = color(1 - W.brake_wear)
   susp[i]  = color(1 - max(W.susp_damage, W.wobble_phase(+0xf8)))   // maxss: if NaN, takes +0xf8
```
(order in the exe: brake loop 0x495c71, susp loop 0x495d1a, both after the tyre loop.)
### 0x4942b0 Car__partReport, engine branch (src: `report_install`, replaces half of its shortcut line)
```
R = car's runtime slot 0x191 (PartType__findRuntimeSlot 0x4c6fd0)
if (R && R.attached == reported part) car.engine_part = reported part   // ResRefLink__set car+0x2098
```
(src: `slot_by_part(part_data(rt, car_ref), 0x191)` and compare `->attached` with `rt.script_instance(P)`.)
### 0x4aebf0 refreshParameters tail 0x4af408..0x4af49a (tyre refs; add in `apply_part_wear_records` of card 222, after the records)
```
for i < wheel count: R = car's runtime slot 101+i
    W[i].tyre_part = (R && R occupied (attached != null)) ? part on R : null
```

## Files
- edit: src/game/car.cpp (`hud_cars`, `report_install`, `apply_part_wear_records`), src/tools/vmboot.cpp
- read only: src/game/car.h

## vmboot
In the `--race` run, once when the race ends (where lap/finish is already printed), print one line per
player car: `hud: gear=<hud[9].text> rpm=<hud[7].text> speed=<hud[8].text> dmg=` + the 20 colours as
`%08x`. Print only, no check (no hand-made expected values).

## Done when
- `// 0x495946` / `// 0x4b0f80 Hud__damageColor` / `// 0x4b1cd0 ResRef__partTear` comments
- `make -C src clean check` green; vmboot --race prints the hud line
- card frontmatter: status implemented, branch name
