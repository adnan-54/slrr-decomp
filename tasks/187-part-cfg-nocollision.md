---
id: 187
status: todo
branch:
depends: []
---
# part cfg `nocollision` (0x4bc940): part bodies get body flag 0x20 too

## Why (notes/09 "def+0xb4 bit 8")
src only honours `nocollision` on the car type's own body lines (`type_records`). The exe's part-cfg handler
0x4bc940 sets `def+0xb4 |= 8`, and every body init 0x501f30 turns def bit 8 into body flag 0x20, the flag
skipped by Collide__bodyPair 0x4fb370 and by the 0x4feb20 child recursion. So part bodies with `nocollision`
must be marked exactly like the type records.

## Exe evidence
### 0x4bc940 part cfg `nocollision` (table 0x5dab10)
```
i = state+0x868                               // current body record
if (i > 0 && (def = *(state + i*0x74 + 0xf4)) != 0) def+0xb4 |= 8
```
### 0x4bfba0 car type `nocollision` (table 0x5daba8): record+0x44 |= 0x100 and def+0xb4 |= 8
### 0x501f30 body init(body, def, pos, rot) (called by 0x4f7830 addBody and 0x4f78d0 addChildBody)
```
...
if (!(def+0xb4 & 4))   body+0x58 |= 1
if (def+0xb4 & 8)      body+0x58 |= 0x20      // <- nocollision
if (def+0xb4 & 0x40)   body+0x58 |= 0x100
if (def+0xb4 & 2)      body+0x58 |= 2
```
The src cfg parser (src/parts/cfg.cpp:314) already sets `CfgBody.flags |= 0x100` for `nocollision` in every
cfg (car type and part alike), so only the consumer has to change.

## Current src (src/game/car.cpp ~594-612)
```cpp
// type_records: the car type's own body records (0x4c1f90), whose
// `nocollision` (record flag 0x100) gives the child body flag 0x20, which the
// 0x4feb20 child recursion skips. Part cfgs use another handler (0x4bc940:
// def+0xb4 |= 8 only), so their bodies keep colliding.
void add_body_lines(std::vector<CompoundChild> &out, const CfgFile &cfg,
                    const Mat34 &rot, Vec3 pos, bool type_records)
...
        if (type_records && (b.flags & 0x100u)) out.back().no_soup = true;
```
Callers: car.cpp:662 (`true`, car type cfg) and car.cpp:693 (`false`, part cfgs).

## Change
- `if (b.flags & 0x100u) out.back().no_soup = true;` for both kinds; drop the now unused `type_records`
  parameter from the definition and both callers (and the declaration, if any: grep `add_body_lines`).
- Rewrite the comment: `nocollision` (0x4bfba0 type / 0x4bc940 part) sets def+0xb4 |= 8, which 0x501f30
  turns into body flag 0x20, skipped by 0x4fb370 and the 0x4feb20 child recursion.
- Expected effect today: none in races (shipped part `nocollision` bodies are `sphere 0.1`, and the soup list
  at car.cpp:736 takes only mesh children); this is correctness for meshes.

## Files
- edit: src/game/car.cpp (add_body_lines + 2 call sites)

## Done when
- `make -C src clean check` green; no new tests; card frontmatter status/branch filled.
