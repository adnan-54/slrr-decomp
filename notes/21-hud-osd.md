# 21. Car HUD / OSD gauges (RE 2026-10-25, cards 196-197)

The race HUD (needles, speed/rpm/gear text, ABS/ASR/handbrake/warning lamps) is **engine code**,
driven by car **type cfg** `osd_*` keywords (e.g. `cars/racers/Axis_data/scripts/_main_200S.cfg:60-78`).
No script draws it. `M` = car type record (manager, `ECX` of the type cfg handlers), `car` = car instance.

## 1. Gauge record G (size 0x48), 17 records at M+0x1a44 + i*0x48
| off | field | notes |
|---|---|---|
| +0x00 | pos[3] | text: screen pos passed to 0x4e8df0; pin/lamp: bone00 position |
| +0x0c | ori[3] | Ypr of bone00; pin angle is added to ori.z |
| +0x18 | kind | 0 none, 1 pin (rotating mesh), 2 text, 5 lamp (shown when value > 0.01) |
| +0x1c | ref (0x10) | ResourceRef: pin/lamp mesh or text font; `+0x24` (= ref+8) != 0 means set |
| +0x2c | color | text color (0xAARRGGBB) |
| +0x30 | fmt[8] | printf format of the text (`%8s` in the cfg) |
| +0x38 | offset | value = clamp(scale*x + offset, min, max) |
| +0x3c | scale | |
| +0x40 | min | |
| +0x44 | max | |

Member ctor 0x4909b0 (M class ctor, 0x4909fb..0x491062): every G gets ref 0, kind 0, fmt[0]=0,
offset 0, **scale 1.0**, min 0, max 0. Type vf1 0x49ab20 (0x49ab4a..0x49b27e) then sets defaults
(floats decoded from the immediates):

| G (M+) | keyword | kind | pos | ori | offset/scale/min/max | ref default (rpk, id) | color, fmt |
|---|---|---|---|---|---|---|---|
| 0x1a44 | (cockpit rpm pin, no osd_ key) | 1 | -0.4 0.1 -0.7 | 0 0 0 | 2.2 / -0.0005 / -2.2 / 2.2 | none | |
| 0x1a8c | (cockpit speed pin) | 1 | -0.3 0.1 -0.7 | 0 0 0 | 2.2 / -0.02 / -2.2 / 2.2 | none | |
| 0x1ad4 | osd_rpmpin | 1 | 1.093 -0.866 1.1001 | 0 | 2.41 / -0.000535 / -2.414 / 2.41 | cars.rpk 0xd227 | |
| 0x1b1c | osd_spdpin | 1 | 1.657 -0.44 1.1001 | 0 | 2.3574 / -0.01305 / -2.353 / 2.3574 | cars.rpk 0xd226 | |
| 0x1b64 | osd_nospin | **2** (sic, 0x49ad90) | 0.73 -0.85 -0.0001 | - | 2.5 / -0.02 / -2.2 / 2.2 | none | |
| 0x1bac | osd_damage | 0 | 0.926 0.855 -0.0001 | - | ctor | cars.rpk 0x57 | |
| 0x1bf4 | osd_gauge | 0 | 1.41 -0.53 1.1 | 0 | ctor | cars.rpk 0xd225 | |
| 0x1c3c | osd_rpm | 2 | 0.72 0.755 0 | - | 0 / 1 / 0 / 99999 | none | 0xff000000 `%4.0f` |
| 0x1c84 | osd_speed | 2 | 0.76 0.57 0.0001 | - | 0 / 1 / 0 / 999 | cars.rpk 0x58 | 0xff000000 `%03.0f` |
| 0x1ccc | osd_gear | 2 | 0.512 0.889 0.0001 | - | ctor | cars.rpk 0x58 | 0xff000000 `%c` |
| 0x1d14 | osd_nos | 2 | 0.73 -0.85 -0.0001 | - | 0 / 8 / 0 / 8 | cars.rpk 0x5c | 0xff000000 `%01.0f` |
| 0x1d5c | osd_turbo | 2 | 0.73 0.9 -0.0001 | - | 0 / 8 / 0 / 8 | cars.rpk 0x5c | 0xff000000 `%01.0f` |
| 0x1da4 | osd_mileage | 2 | 0.73 0.73 -0.0001 | - | 0 / 1 / 0 / 999999 | cars.rpk 0x58 | 0xff000000 `%07.0f` |
| 0x1dec | osd_abs | 5 | 3.24 -2.4 -3.58 | 0 | ctor | none | 0xffff0000 |
| 0x1e34 | osd_asr | 5 | 3.24 -2.4 -3.58 | 0 | ctor | none | 0xffff0000 |
| 0x1e7c | osd_hbrake | 5 | 3.24 -2.4 -3.58 | 0 | ctor | none | 0xffff0000 |
| 0x1ec4 | osd_emergency | 5 | 3.24 -2.4 -3.58 | 0 | ctor | none | 0xffff0000 |

Gear characters: `M+0x1a38` char[9], vf1 copies `"0123456R"` (0x49abd2) then `"N123456R"` (0x49ae5a).
Default refs are loaded with `0x5189e0(rpk name, id, 0xe, 0)` -> `0x51fa40` into G+0x1c.

## 2. Type cfg keywords (table 0x5da7b8: name ptr, handler), all skipped when `M+0x120 != 0`
Scanner 0x569380 (sscanf-like); the first value is a resource id -> `0x51e860(id)` -> `0x51fa40(res, 0xe, 0)` into G+0x1c (only when the scan count is > 0, gear: > 1).
| keyword | handler | format -> fields |
|---|---|---|
| osd_gauge | 0x48e770 | `%d %f %f %f` id, G.pos (G=0x1bf4) |
| osd_rpmpin | 0x48e7f0 | `%d %f %f %f %f %f %f %f` id, pos, offset, scale, min, max (G=0x1ad4) |
| osd_spdpin | 0x48e890 | same (G=0x1b1c) |
| osd_nospin | 0x48e930 | same (G=0x1b64) |
| osd_damage | 0x48e9d0 | `%d %f %f %f` id, pos (G=0x1bac) |
| osd_damage_slot | 0x48ea50 | `%c` into a stack byte: no effect |
| osd_gear | 0x48eaa0 | `%d %f %f %f %d %8s` id, pos, color, gear chars -> M+0x1a38 (G=0x1ccc) |
| osd_speed | 0x48eb30 | `%d %f %f %f %d %8s` id, pos, color, fmt (G=0x1c84); offset..max pushed but not in the format |
| osd_rpm | 0x48ebe0 | same (G=0x1c3c) |
| osd_nos | 0x48ec90 | `%d %f %f %f %d %8s %f %f %f %f` id, pos, color, fmt, offset, scale, min, max (G=0x1d14) |
| osd_turbo | 0x48ed40 | same (G=0x1d5c) |
| osd_mileage | 0x48edf0 | same (G=0x1da4) |
| osd_abs / asr / hbrake / emergency | 0x48eea0 / 0x48ef20 / 0x48efa0 / 0x48f020 | id, pos, color only (G=0x1dec/0x1e34/0x1e7c/0x1ec4; Ghidra shows 5 targets for a 10-conversion format) |

## 3. Script natives Chassis.getOSD 0x4b1fd0 / setOSD 0x4b4850 (were stubs)
ids 0..3 = spdpin offset/scale/min/max (M+0x1b54..0x1b60), 5..8 = rpmpin (M+0x1b0c..0x1b18).
get 4 = `|root vel| * (1/measure_per_hour 0x5fe378) * M+0x1b58 + M+0x1b54`; get 9 = `|car+0x1e80 * 9.549296| * M+0x1b10 + M+0x1b0c`; other ids / no car -> 0. set 4/9/others: ignored. Only caller: Track.java:1951-1989 debug OSD.

## 4. Per-frame update: type_car__vf13 0x494910, block 0x4954bc..0x495e07
Runs when `car+0x2054 == 0`; for each control slot s < M+0x17ec with `R_s = car+0x150 + s*0x4a8`:
player id `R_s+8` != 0, HUD viewport id `R_s+0x58` != 0 (ref R_s+0x50) and `s == M+0x17f0`. Element
handles (0x10 each) live in R_s; `gauge_update(G, elem, parent, x)` = **0x4b1d50** (ECX = G):
```
if G.ref id (G+0x24) == 0: return
kind 1: v = clamp(G.scale*x + G.offset); if !elem: elem = instance(parent R_s+0x50, G.ref, 0x59d94c)
        bone00(elem).set(pos = G.pos, ori = (G.ori.x, G.ori.y, G.ori.z + v))      // 0x569b40, 0x4ec540
kind 2: v = clamp(...); if !elem: elem = text(parent, 0x100, pos = G.pos, font = G.ref, G.color, 2, 1.0)  // 0x4e8df0
        sprintf(elem text, G.fmt, (double)v)                                     // 0x4eb370 -> 0x5277d0
kind 5: if x <= 0.01: delete elem if any (0x4e9450); return
        if !elem: elem = instance(parent, G.ref, 0x59d958); bone00(elem).set(G.pos, G.ori)
clamp(v): out = min; if (min <= v) { out = max; if (v <= max) out = v }   // NaN -> min
```
Sources (asm 0x495516..0x495945; `rpm = |car+0x1e80 * 9.549296|`, `v = |root active-state vel|`):
| G | elem (R_s+) | x |
|---|---|---|
| gear 0x1ccc | +0x1fc | `sprintf(elem, M+0x1cfc fmt, M+0x1a38[car+0x1e5c gear])` via 0x4eb370 directly (no create, no clamp) |
| rpm 0x1c3c | +0x21c | rpm |
| speed 0x1c84 | +0x23c | v / measure_per_hour (0x5fe378) |
| mileage 0x1da4 | +0x1ec | car+0x2250 * measure_inv10 (0x5fe374) |
| nos 0x1d14 | +0x1cc | car+0x1f5c / e+0xa4c, only if e+0xa4c > 0 |
| turbo 0x1d5c | +0x1dc | EngineModel__boostGauge 0x4f5ab0(rpm), only if dyno e+0xc30 and dyno +0xdc,+0xe8,+0xec > 0 |
| rpmpin 0x1ad4 | +0x22c | rpm |
| spdpin 0x1b1c | +0x24c | v * 3.6 (0x59ab88) |
| nospin 0x1b64 | +0x25c | car+0x1f5c / e+0xa4c, only if e+0xa4c > 0 |
| abs 0x1dec | +0x28c | car+0x70 & 0x1000000 ? 1 : 0 |
| asr 0x1e34 | +0x29c | car+0x70 & 0x2000000 ? 1 : 0 |
| hbrake 0x1e7c | +0x2ac | car+0x1df4 (handbrake input) |
| emergency 0x1ec4 | +0x2bc | (car+0x1ec0 > 100 or rpm > e+0x1c or rpm > e+0x18) ? 1 : 0 |
| cockpit 0x1a44 | +0x26c (parent car+0x2c) | rpm, only if R_s+0x38 != 0 |
| cockpit 0x1a8c | +0x27c (parent car+0x2c) | v * 3.6, only if R_s+0x38 != 0; else both elems deleted |
(elem offsets: ESI = car+0x2cc + s*0x4a8 = R_s+0x17c; elem = ESI + {0x80 gear, 0xa0, 0xc0, 0x70, 0x50, 0x60, 0xb0, 0xd0, 0xe0, 0x110, 0x120, 0x130, 0x140, 0xf0, 0x100}.)
After the gauges: damage colours (0x4b0f80/0x4b1cd0, 0x4ebd20 colour sets; args lost in Ghidra, NOT traced) and per-wheel lamps.

## 5. Not traced / open
- HUD element creation of the static `gauge` mesh and debug texts: 0x4ad770 (slot camera + `gauge` instance at M+0x1bf4 with ref M+0x1c10, green debug texts at M+0x1bac). 0x4ade90 = driver IK (Bip01 bones), not HUD.
- Damage colour block 0x495946..0x495e07 (0x4b0f80, 0x4b1cd0) and its 0x4ebd20 arguments.
- Who fills R_s+0x50 (HUD viewport) and R_s+0x30/+0x38 (cockpit view) in src.
- Cockpit pin keywords (G 0x1a44/0x1a8c): none in table 0x5da7b8.
