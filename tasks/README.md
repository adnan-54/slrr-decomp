# Task cards

Open cards live here; merged ones are in `done/` (`git mv` on merge). Status: `draft | todo | blocked | implemented (branch) | merged | rejected`.

## Card format (`tasks/NNN-slug.md`), sized for a Flash agent
A card must let the agent finish **without rea and without reading large files**: <= ~5 exe functions, <= 3 source files to touch, all exe evidence pasted in. Larger -> split.

```markdown
---
id: 042
status: todo
branch:                 # filled by the implementing agent
depends: [041]
---
# <subsystem>: <what>

## Goal
One paragraph: behaviour to port, which natives/functions stop being stubs.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x4b6420 WheelRef.setFriction
<pseudocode, struct offsets mapped to our field names>

## Files
- edit: src/game/car.cpp (WheelData::set_friction), src/script/natives.cpp (registry row)
- read only: src/game/car.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
```

## Frozen DeepSeek implementer prompt (fill only `<card>`; cache-friendly, never edit)
```text
You implement one task card in this repo. Read CLAUDE.md, then the card, then only
the files the card lists, in order. Do not use rea/Ghidra tools; all exe evidence is
in the card. Steps: git switch -c card/<id> main; implement; make -C src clean check;
set the card's status to implemented and branch to card/<id>; commit (message ends
with "Co-Authored-By: DeepSeek V4 Flash <noreply@deepseek.com>"); git switch main. Do not push.
Report (max 15 lines): branch, commit, what changed, INVENTED items, doubts.
Card: <card>
```
