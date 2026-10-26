---
id: 221
status: todo
branch:
depends: []
---
# Parts: cfg `slotdmgmode` + damage-mode/wear fields

## Goal
Parse the part cfg keyword `slotdmgmode` (267 shipped cfgs) into the type slot, and add the struct
fields that cards 222/223 fill and read: part damage-mode word P+0xb0, wheel wear rates and suspension
damage, car transmission/radiator/clutch wear and the part refs, HUD damage-letter colours. No logic
beyond the parser and the copy into `Slot` (notes/33 §3).

## Exe evidence
### 0x4c00d0 cfg_slotdmgmode (part cfg tables 0x5da5c4 / 0x5dac34)
```
if (M+0x120 != 0) return                      // same gate as the other slot keywords
if (type slot list empty) return
S = M+0x1194                                  // the slot declared last
v = 0xffffffff
Str__scan(value, "%d", &v)                    // 0x527140 %d: "0x.." goes to the hex reader
S+0x68 = v                                    // failed scan keeps 0xffffffff
```
### 0x4bfc9d type slot ctor (`slot` keyword): S+0x68 = 0xffffffff
Corpus values: `slotdmgmode 0x00000002/4/5/6/7/8/9/A/E` and `0x00010004` (always `0x%08X`).

## Changes
- `src/parts/cfg.h` CfgSlot: `int32_t dmg_mode = -1;  // type slot +0x68, cfg slotdmgmode (0x4c00d0)`.
- `src/parts/cfg.cpp`: add `slotdmgmode` to `known_key`; in the parser next to `slottype`: one value,
  read with the exe %d rules (hex with `0x` prefix, else decimal; see `vscan_int` in src/game/car.cpp
  ~line 260 for the exact rules, duplicate a small static copy in cfg.cpp or reuse if exported), store into
  `cfg->slots.back().dmg_mode` when a slot exists; a failed read keeps -1 (do not fail the cfg).
- `src/game/car.h` (fields only, defaults 0 unless stated):
  - `Slot`: `int32_t dmg_mode = -1;  // type slot +0x68 (0x4c00d0)`
  - `PartData`: `uint32_t dmg_mode = 0;  // +0xb0 damage kind lo16 / wheel hi16 (0x4c7d20)`
  - `WheelData`: `tyre_rate` (+0x160), `brake_rate` (+0x168), `susp_damage` (+0x16c), `susp_rate` (+0x170)
  - `CarData`: `engine_rate` (+0x1ea0), `trans_wear` (+0x1ea4), `trans_rate` (+0x1ea8),
    `radiator_damage` (+0x1eac), `radiator_rate` (+0x1eb0), `clutch_wear` (+0x1eb4), `clutch_rate` (+0x1eb8);
    `Runtime::Resource *engine_part` (+0x2098), `*clutch_part` (+0x20c0), `*trans_part` (+0x20d0),
    `*radiator_part` (+0x20e0), all nullptr
  - `WheelData`: `Runtime::Resource *tyre_part = nullptr;  // +0x1d8 (0x4af431)`
  - `HudGauge` stays; add to `CarData`: `uint32_t hud_dmg[8 + 12] = {};  // damage letters A B C D E T L O, then 4x tyre/brake/susp (notes/33 §2)`
- `src/game/car.cpp` `fill_slots`: copy `s.dmg_mode = cs.dmg_mode;`.
  (car.cpp: only this one line; car.h + car.cpp count as the struct change.)

## Files
- edit: src/parts/cfg.h, src/parts/cfg.cpp, src/game/car.h (+ one line in src/game/car.cpp fill_slots)

## Done when
- `// 0x4c00d0 cfg_slotdmgmode` comment at the parser branch
- cfg corpus check still parses 3,189/3,194 (no new failures from slotdmgmode)
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
