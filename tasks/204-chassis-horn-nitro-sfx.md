---
id: 204
status: implemented
branch: card/204
depends: []
---
# Chassis natives: setHornSFX, setNitroSFX

## Goal
Replace the explicit stubs `Chassis.setHornSFX(Ljava.util.resource.ResourceRef;FI)V` and
`Chassis.setNitroSFX(Ljava.util.resource.ResourceRef;F)V` (src/script/natives.cpp rows ~7895-7896,
`nullptr, true`). Caller: parts/scripts/bodypart/Chassis.java:1236-1239 `setupHorn` (indexes 1, 3,
then 2 with a ResourceRef set to 0). Today `EngineSfx::horn_resource[]` is never written, so
`Vehicle.getHorn` (natives.cpp ~4047) always returns 0 and the horn never sounds
(src/audio/sfx.cpp:271). setNitroSFX has no script caller; port it anyway (4 lines).
`setBuck` stays a stub (no caller). RE: notes/23 §2.

## Exe evidence (cleaned)
Prologue of both: getArgs; `H = instanceToHandle(self)`; RI = H+0xc; make resident; typed
`0x80000000` (0x51f4f0) must be >= 0; `T = RI->vf3(1.0)`, `car = T+0x4c`; `e = car+0x2050`.
Any failure: return (void). In src: `game::CarData *c = car_of(a)`; `c->engine_sfx` is `e`.
### 0x4b4f40 setHornSFX(ResourceRef sfx, float pitch, int index)
```
defaults: index = 1, sfx = 0, pitch = 1.0 (locals before getArgs)
if (car && (unsigned)index < 4) {
    if (sfx != 0) bind(e+0x838 + index*0x10, sfx.RI);   // 0x40cac0; node+8 = resource id
    *(float*)(e+0x878 + index*4) = pitch;               // always, even when sfx == 0
}
```
### 0x4b5130 setNitroSFX(ResourceRef sfx, float pitch)   (asm 0x4b51bf..0x4b51d8)
```
if (car && sfx != 0) bind(e+0x818, sfx.RI);             // pitch is read but NOT stored
```
`bind(node, RI)` = intrusive link; the node's id becomes RI's id (0 when RI null). In src the
node is the int field: `id = (h && h->target) ? (int32_t)h->target->id : 0` with
`Runtime::Handle *h = value_handle(a[1])` (natives.cpp:1503). "sfx != 0" = the script object
is non-null (`a[1].kind == Value::Obj && a[1].obj`); a ResourceRef after `set(0)` is non-null
with no target -> id 0 (unlinks).
Field map: e+0x838+i*0x10 -> `engine_sfx.horn_resource[i]`; e+0x878+i*4 ->
`engine_sfx.horn_pitch[i]`; e+0x818 -> `engine_sfx.nitro_start_resource` (src/audio/sfx.h:93-97).

## Files
- edit: src/script/natives.cpp: add `ch_set_horn_sfx`, `ch_set_nitro_sfx` next to `veh_get_horn`
  (~4047); registry rows -> `ch_set_horn_sfx, false` / `ch_set_nitro_sfx, false`.
- read only: src/audio/sfx.h (EngineSfx), src/game/car.h (CarData::engine_sfx).
Args: a[0] receiver, a[1] sfx, a[2] pitch (`get_f`), a[3] index (`get_i`, horn only).

## Done when
- `// 0x4b4f40` / `// 0x4b5130` comments; nothing invented beyond the id mapping above
- `make -C src clean check` green (stub count drops by 2); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
