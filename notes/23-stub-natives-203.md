# Remaining explicit native stubs (2026-10-25, cards 203-206)

Stubs left in src/script/natives.cpp (`nullptr, true`): Chassis.setHornSFX/setNitroSFX/setBuck,
Part.install_OK, GameLogic.kismajomCheck, GameRef.setActiveCollision, Object.runDLL.
Shipped .java callers (grep over `bin/the game/**/*.java`, src/ copies excluded):
setHornSFX Chassis.java:1236-1239 (setupHorn); install_OK Garage.java:1106, InventoryItem_Part.java:124
(Part.java:840 is commented out); kismajomCheck Garage.java:1926; runDLL Init.java:164-167 (Steam
workshop, keep stub); setNitroSFX/setBuck/setActiveCollision: declared only, no caller.

## 1. 0x4debd0 GameLogic.kismajomCheck(String[]) -> card 203
`PTR_DAT_005dadb8` (initial value 0x5fe3c0) is the write cursor of the 16-byte lastKey ANSI ring
0x5fe3c0..0x5fe3cf (Input.lastKey 0x4de9f0, src `HotkeyTable::last_ring/last_pos`). For n = length-1
down to 0: compare `s[len-1-k] - 1` (signed char) with ring[cursor-1-k] (wrapping); full match ->
clear ring[cursor-1] and return n. No match -> -1. Empty string matches at once.

## 2. 0x4b4f40 Chassis.setHornSFX(ref, pitch, index), 0x4b5130 setNitroSFX(ref, pitch) -> card 204
Horn: typed 0x80000000 gate; `index < 4` (unsigned); `if (ref) bind(e+0x838+index*0x10, ref.RI)`;
`e+0x878 + index*4 = pitch` (always when index < 4). Nitro (asm 0x4b51bf..0x4b51d8): `if (ref)
bind(e+0x818, ref.RI)`; pitch read but **not stored**. e = car+0x2050. src: `EngineSfx::horn_resource/
horn_pitch/nitro_start_resource` (src/audio/sfx.h:94-97).
setBuck 0x4b4410 (notes/13 native summary): no script caller; left as stub.

## 3. 0x4cd3a0 Part.install_OK(dest, slot, xpart, slot2, pos) -> card 206
getArgs order: self, dest node, slot (ignored, overwritten), xpart node (out), slot2 (in/out), pos.
`dest == 0 -> null`; typed 0xa0000000 + secondary 0x20000000 (0x468b00) gates; `bind(P+0x60, dest.RI)`
(0x4cd4c7 `LEA ECX,[P+0x60]`, same field the "install" command sets); local tref node = {0};
`r = Part__installSearch 0x4c96c0(P, &tref, &slot2, pos ? &(pos.x,y,z) : null)`; `if (xpart)
bind(xpart, tref.RI)` (the target actually used); r != 0 -> new int[2]{r, slot2}, else null.
src already has the search: `install_search` (src/game/car.cpp:672, used by part_command).

## 4. Exe review of merged cards 203-206 (2026-10-25)
Decompiled 0x4debd0, 0x4b4f40 (+asm), 0x4b5130, 0x4cd3a0, 0x4c1f90, 0x4b3220, 0x4b2680, 0x5032d0.
- 203 PASS: last index first, newest-first ring walk with p+15 wrap, signed `c-1` compare, clear ring[cursor-1].
- 204 PASS: defaults index 1/sfx 0/pitch 1.0, `index > 3` unsigned reject, bind e+0x838+i*0x10 only if sfx,
  pitch e+0x878+i*4 always; nitro binds only, pitch not stored.
- 205 FIXED: 0x5032d0(part_comp), run at the end of 0x4c1f90 and after installs, subtracts the children's COM C
  from every child offset X.t when |C|^2 > 1e-5. So X.t = rec.pos + rec.R*com - C for every car (not only the
  5 non-origin cfgs). src now uses `bbone_pos - car->com` (src flat compound COM; INVENTED: threshold residual and
  nested compounds). Walk/0.1/field order x,y,z (0x59c170/188/198) confirmed.
- 206 FIXED (edge): only a null dest *object* returns null early (dest ref without target runs the search and
  unbinds xpart). Gate failure returns {slot arg, slot2} in the exe (callers pass slot 0): left INVENTED.
