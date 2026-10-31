---
id: 263
status: draft
branch:
review:
depends: [259, 262]
---
# traffic: parkInstance 0x417080, ParkMap 0x4171d0, takeParked 0x418f60, pseudo-car 0x417350

## Goal
Port the park side: dematerialized typed instances are deleted (condition < 90%) or parked
under the CarContainer at y = -10000, owned by the pseudo-car T+0x3118, and reused by
materialize's takeParked branch. Notes: `notes/42-traffic-410680-services.md` §5, §6, §11, §14.

## Exe evidence (asm-checked, notes/42 §6)
```
0x417080 Traffic__parkInstance(ECX = T, typeId, link):
if (getInfo(ECX = 0x5fde90, link, 0xf, 0) < 0x5a) { 0x4e9450 Handle__queueDelete(ECX = link); return }
    // type_car vf15 case 0xf = (int)(M+0xb4 * 100.0), M+0xb4 = PartData::tear
p = 0x51df40(ECX = T+0xcc) pool link; p = {0}; 0x40cac0(ECX = p, link+0xc)
getInfo(p, 0x38, &T+0x3118)                       // card 262
params = "fffffffffffffff" (0, -10000.0 (0xc0c3880000000000), 13 x 0); 0x4eb3f0(ECX = p, &params)
0x4ec210(ECX = p, &T+0x311c); ParkMap__insert(ECX = T+0xe0, &typeId, &p)
```
Callers: 0x410680 (ECX = T, 0x410777), 0x40fae0 typed branch (ECX = T, 0x40fb05), 0x40ed30/0x40ebb0.
0x417350 (notes/42 §14): pseudo-car = new pool car (0x40c400), live 0, world (0, -10000.0f
(0xc61c4000), 0), e0/e4/e8 = T+0x84 + 1.0f; every T+0xcc link -> getInfo(.., 0x38, &pseudo).

## UNRESOLVED (blocks todo)
- 0x4eb3f0 with the 15-float block: typed manager vf7 (`+0x1c`) on a car — meaning of the 15
  values (pose? matrix?) not traced; materialize uses it with "ffffff" (pos + ypr).
- 0x4171d0 / 0x418f60 bodies only summarized (notes/42 §6); asm-check before carding.
- What 0x47c640 getInfo returns for a non-car instance (case 0xf on another type).
- Handle__queueDelete on the traffic link: src `Runtime::queue_delete(Handle *)` takes a Handle;
  traffic keeps an id (bridge design, extend notes/42 §15).

## Files (when promoted)
- edit: src/game/traffic.h, src/game/traffic.cpp, src/game/runtime.cpp
