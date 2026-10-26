# 33 — HUD leftovers: damage indicator + its part-damage feed (RE 2026-10-25)

Audit of src vs `type_car__vf13` 0x494910 HUD block and `OsdGauge__update` 0x4b1d50 (notes/21),
then RE of the largest exe-side gap. New names in analysis/names.json: `Hud__damageColor` 0x4b0f80,
`ResRef__partTear` 0x4b1cd0, `type_car__createHudElements` 0x4ad770, `Renderable__setColor` 0x4ebd20,
`cfg_slotdmgmode` 0x4c00d0, `Part__propagateDamageMode` 0x4c9530, `Car__partReport` 0x4942b0,
`Part__commitInstall` 0x4c7d20, `OsdGauge__update` 0x4b1d50.

## 1. What src has (cards 196/197) and what is left
Ported: 17 gauge records + osd_* keywords, `hud_cars` (gauge values 0x4954bc..0x495945), getOSD/setOSD.
Left, by kind:
| gap | exe | src | verdict |
|---|---|---|---|
| gauge placement | vf13 runs in the render pass per drawn car | `hud_cars` once per `advance_sim` (natives.cpp, INVENTED) | render-loop placement, nothing to RE |
| viewport / cockpit | R_s+0x58 viewport, R_s+0x38 cockpit view | INVENTED present / absent (car.cpp `hud_cars`) | render-only; set by the camera/viewport commands, not ported |
| element creation | 0x4ad770 (gauge mesh, letter glyphs) | value-only `HudGauge` | render-only |
| **damage indicator** | block 0x495946..0x495e07 | **missing** | exe-side, ported by card 223 |
| damage inputs | slotdmgmode -> P+0xb0 -> refreshParameters -> 0x4a8060 | missing (`shortcut: installed parts' wear records (0x4a8060) not re-applied`, natives.cpp ch_force_update) | exe-side, cards 221/222; also changes the sim (engine health, wheel wear rates) |
| HUD readout | (screen) | nothing reads `car->hud[]` | card 223 adds a vmboot print |
Not exe-side: the race results dialog, lap/position texts (script-only, Track.java).

## 2. Damage indicator: 0x4ad770 letters + block 0x495946..0x495e07
0x4ad770 (slot s == M+0x17f0, viewport present) creates Text elements with the `osd_damage` record
(pos M+0x1bac, font ref M+0x1bc8; cars.rpk 0x57 is a glyph font whose letters are car-part icons),
colour 0xff00ff00 at creation. Letters and element slots (ESI = car+0x2cc + s*0x4a8):
`A` ESI-0x100, `B` -0xd0, `C` -0xc0, `D` -0xe0, `E` -0xb0, `T` -0xa0, `L` +0x40, `O` -0x90;
per wheel i < min(4, M+0x1fb4): tyre `"dcba"[i]` ESI-0x80+i*0x10, brake `"1234"[i]` ESI-0x40+i*0x10,
suspension `"7856"[i]` ESI+i*0x10 (tables 0x5da9c0/0x5da9d0/0x5da9e0).

Per frame (same slot gate as the gauges, after them; EDI = car, M = type, e = car+0x2050):
```
color(x) = Hud__damageColor 0x4b0f80:  x >= 1 -> 0xff00ff00; x <= 0 -> 0xffff0000;
           x >= 0.5 -> 0xffffff00 - ((int)(x*512 - 256) << 16); else 0xffff0000 + ((int)(x*512) << 8)
           // (int) = truncation (cvttss2si); NaN falls to the last branch
tear(ref) = ResRef__partTear 0x4b1cd0: part payload of ref, P+0xb4 (tear); 0 when unresolved
A,B,C,D : c = (0 < car.tear(+0xb4)) ? color(car.tear) : 0xff000000         // one value, 4 letters
E       : if (car+0x20a0 engine ref set) { y = (1 - car+0x1e9c) * tear(car+0x2098); c = y > 1 ? 0xff00007f : color(y) } else 0xff000000
T       : x = 1 - car+0x1eb4 (clutch wear)
          if (clutch ref car+0x20c8 set) { y = x * tear(car+0x20c0); c = y > 1 ? 0xff00007f : color(y) }
          else c = (e+0x20 gears > 0) ? color(x) : 0xff000000
L       : x = 1 - car+0x1eb4; if (car+0x20c8 set) { y = x * tear(car+0x20d0 transmission ref); c = y > 1 ? 0xff00007f : color(y) } else color(x)
O       : same as L with tear(car+0x20e0 radiator ref)              // sic: L and O test the clutch ref and use clutch wear
wheel i < M+0x1fb4 (W = car+0x1414 + i*0x2e4; NOT capped at 4, so i >= 4 recolours the next group):
  tyre  : W+0x1e0 (tyre ref W+0x1d8 set) ? color((1 - W+0x15c) * tear(W+0x1d8)) : 0xff000000   // no >1 branch
  brake : color(1 - W+0x164)
  susp  : color(1 - max(W+0x16c, W+0xf8))
Renderable__setColor 0x4ebd20: typed render data +0x17c = colour.
```
Before the letters the block deletes the cockpit pin elements (ESI+0xf0/+0x100) when R_s+0x38 == 0
(src already clears hud[0]/hud[1]).

## 3. Damage-mode feed (who sets the inputs)
- **cfg `slotdmgmode`** (part cfg table 0x5da5c4/0x5dac34 -> 0x4c00d0): if M+0x120 == 0 and a slot was
  declared: `last slot (M+0x1194)+0x68 = Str__scan("%d")` (hex via 0x527490, default 0xffffffff when the
  scan fails). Type slot ctor 0x4bfc9d: +0x68 = 0xffffffff. Corpus: 267 cfgs, values 0x2,4,5,6,7,8,9,A,E,0x00010004.
- **Part__commitInstall 0x4c7d20** (0x4c7ec9..0x4c7f28), after T+0x74++: `S = T's type slot with id =
  my runtime slot's mate id (0x4c6f90)`; `old = P+0xb0; P+0xb0 = T+0xb0; lo = S+0x68 & 0xffff; if lo != 0xffff
  P.lo16 = lo; hi = S+0x68 & 0xffff0000; if hi != 0xffff0000 P.hi16 = hi>>16; if (P+0xb0 != old) Part__propagateDamageMode(P)`.
- **Part__propagateDamageMode 0x4c9530(P)** (ECX = P's type): walk P's type slots and runtime slots in
  step; for each runtime slot present with flags & 2 (a child sits on it) and child C resolves:
  `old = C+0xb0; C+0xb0 = P+0xb0` with the same lo/hi override from this type slot's +0x68;
  `if (C+0xb0 != old && C's +0x44 ref resolves to an object with +0xc != 0) recurse(C)`.
- **Vehicle__refreshParameters 0x4aebf0** (forceUpdate, and vf13 when car+0x70 & 0x800000): zero every
  wheel's +0x15c/+0x160/+0x164/+0x168/+0x16c/+0x170, run script `updatevariables`, unlink clutch ref
  car+0x20c0 (0x4aed65..0x4aedc3; transmission/radiator refs are NOT cleared here), then for each
  installed part (car+0xdc list, node+0x30) `Vehicle__applyPartWearRecord 0x4a8060(P)`:
```
k = P+0xb0 & 0xffff; w = P+0xb0 >> 16 (wheel); wear = P+0xb8; rate = P+0xbc
5: if w < M+0x1fb4: W[w]+0x15c = wear, +0x160 = rate        // tyre
6: if w < count:    W[w]+0x164 = wear, +0x168 = rate        // brake
7: if w < count:    W[w]+0x16c = wear, +0x170 = rate        // suspension
8:  car+0x1e9c = wear, +0x1ea0 = rate                        // engine
9:  car+0x1ea4 = wear, +0x1ea8 = rate, transmission ref car+0x20d0 = P
10: car+0x1eb4 = wear, +0x1eb8 = rate, clutch ref car+0x20c0 = P
12: car+0x1eac = wear, +0x1eb0 = rate, radiator ref car+0x20e0 = P
other kinds (2, 4, 0xE, ...): nothing
```
  Later in the same function (0x4af408..): for each wheel i < M+0x1fb4, `R = runtime slot 101+i of the
  car (PartType__findRuntimeSlot 0x4c6fd0)`; if R and R+8 (occupied) -> tyre ref W+0x1d8 = R's part, else unlink.
- **Car__partReport 0x4942b0** engine branch: `R = runtime slot 0x191 of the car; if R and R+8 == reported
  part` -> engine ref car+0x2098 = part (plus flag 0x4000 and the dynodata handle, already in src).

## 4. Open
- The wear accumulation in `Vehicle::update` (notes/09 §thermal/impact/brake/clutch wear) is not ported, so
  in vmboot wear only comes from part P+0xb8 (scripts' setWear on used cars). Next target after 221-223.
- 0x4ace87 / 0x4aed7b also unlink the engine/clutch refs (part removal path, not traced).
