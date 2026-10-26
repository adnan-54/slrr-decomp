---
id: 222
status: todo
branch:
depends: [221]
---
# Parts: damage-mode propagation + refreshParameters wear records (0x4a8060)

## Goal
Give each installed part its damage-mode word (P+0xb0) at install time and re-apply the installed
parts' wear records in `Chassis.forceUpdate` (replaces `shortcut: installed parts' wear records
(0x4a8060) not re-applied` in natives.cpp `ch_force_update`). This feeds engine damage (already used by
the torque health factors), wheel tyre/brake wear and their rates, and the clutch/transmission/radiator
refs read by the HUD (card 223). Fields come from card 221. Evidence: notes/33 §3.

## Exe evidence
### 0x4c7d20 Part__commitInstall, 0x4c7ec9..0x4c7f28 (after T+0x74++, before the car ref)
P = installed part, T = target part, S = T's type slot whose id = P's runtime slot mate id (0x4c6f90)
```
old = P.dmg_mode
P.dmg_mode = T.dmg_mode
lo = S.dmg_mode & 0xffff;      if (lo != 0xffff)     P.dmg_mode = (P.dmg_mode & 0xffff0000) | lo
hi = S.dmg_mode & 0xffff0000;  if (hi != 0xffff0000) P.dmg_mode = (P.dmg_mode & 0x0000ffff) | hi
if (P.dmg_mode != old) Part__propagateDamageMode(P)
```
In src `part_attach` (car.cpp), S is `ts` (target's slot = type slot in src's merged Slot). Put this
right after `tp->installed++`.
### 0x4c9530 Part__propagateDamageMode(P)
```
for each slot s of P (type slot and runtime slot walked in step):
    if (s present && (s.flags & 2) && child C on s resolves):
        old = C.dmg_mode
        C.dmg_mode = P.dmg_mode, then the same lo/hi override from s.dmg_mode
        if (C.dmg_mode != old && C's +0x44 ref resolves with +0xc != 0) recurse(C)
```
src: iterate `pp->slots` like `report_install` does (flags & 2, `slot_part(rt, s.attached)`).
INVENTED: the +0x44 test is not mapped; recurse whenever C changed (mark it).
### 0x4aebf0 Vehicle__refreshParameters (parts relevant here)
```
for each wheel: +0x15c = +0x160 = +0x164 = +0x168 = +0x16c = +0x170 = 0     // 0x4aeceb..0x4aed1d
script updatevariables()                                                      // already in src
clutch_part = null                                                            // 0x4aed65..0x4aedc3 (only this ref)
for each installed part P (car+0xdc list): Vehicle__applyPartWearRecord(P)    // 0x4aede9..0x4aee7b
```
src: zero tyre_rate/brake_rate/susp_damage/susp_rate next to the existing tyre_wear/brake_wear zeroing,
then after `updatevariables` clear `clutch_part` and walk `c->part_list`.
INVENTED: car+0xdc (installed-part list) approximated by `part_list` (car+0xfc, reported parts).
### 0x4a8060 Vehicle__applyPartWearRecord(P, M, car)
```
k = P.dmg_mode & 0xffff; w = P.dmg_mode >> 16; wear = P.wear (+0xb8); rate = P.max_wear_inv (+0xbc)
switch k:
 5: if (w < wheel count) { W[w].tyre_wear = wear; W[w].tyre_rate = rate }
 6: if (w < count)       { W[w].brake_wear = wear; W[w].brake_rate = rate }
 7: if (w < count)       { W[w].susp_damage = wear; W[w].susp_rate = rate }
 8: car.engine_damage = wear; car.engine_rate = rate
 9: car.trans_wear = wear; car.trans_rate = rate; car.trans_part = P
10: car.clutch_wear = wear; car.clutch_rate = rate; car.clutch_part = P
12: car.radiator_damage = wear; car.radiator_rate = rate; car.radiator_part = P
default: nothing
```
`w` is signed int compare in the exe (`(int)(dmg >> 16) < count`).

## Files
- edit: src/game/car.cpp (part_attach merge, propagate helper, `apply_part_wear_records(Runtime&, CarData&)`
  exported via car.h declaration), src/script/natives.cpp (`ch_force_update`: call it, drop the shortcut line)
- read only: src/game/car.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- the existing health computation in ch_force_update runs after the records (exe order)
- `make -C src clean check` green (race numbers may move if shipped scripts set part wear; report it)
- card frontmatter: status implemented, branch name
