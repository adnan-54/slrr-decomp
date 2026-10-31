---
id: 259
status: draft
branch:
review:
depends: [257]
---
# traffic: instance reparent 0x4ec210, CarContainer T+0x311c and parkInstance 0x417080

## Goal
Make the instance side of `car_sync_instance` / `car_bind_object` act on real instances:
type-0 script cars (GroundRef.addTrafficCar) get reparented under the road/cross object or
the CarContainer; typed instances get parked. Evidence: `notes/42-traffic-410680-services.md`
§1, §2, §6, §7, §8.

## Exe evidence
- 0x410680: reparent calls have ECX = `&car+0x130` (`0x4106b8`, `0x410701`); type 0:
  `!live || !bound -> 0x4ec210(T+0x311c)` else `0x4ec210(link)`.
- 0x4ec210 = GameRef.setParent body (src `Runtime::reparent`, runtime.cpp:887).
- 0x416000: `T+0x311c = createScripted(game root 0x5fde50, null type, DAT_0059380d, "CarContainer")`,
  `obj+0x84 |= 0x10`, `0x51f7f0(link)`, `0x417350()`.
- 0x417080 parkInstance: `getInfo(link, 0xf) < 0x5a -> Handle__queueDelete`; else pool link,
  `getInfo(p, 0x38, T+0x3118)`, matrix y = -10000.0 (0xc0c3880000000000), reparent under
  T+0x311c, `ParkMap__insert(T+0xe0, type, p)`.

## UNRESOLVED (blocks todo)
- `DAT_0059380d` params text, the `+0x84 |= 0x10` flag, `0x51f7f0`, `0x417350`.
- ECX of `Handle__queueDelete` in 0x417080 and of the `0x417080`/`ResRefLink__set` calls in 0x40fae0.
- traffic::Graph stores object ids only; a Graph -> Runtime bridge is a design decision (INVENTED).
- `getInfo(.., 0xf)` = `(int)(inst+0xb4 * 100)` meaning unknown (notes/29 §5); `0x4eb3f0`.

## Files (when promoted)
- edit: src/game/traffic.cpp, src/game/runtime.cpp, src/script/natives.cpp (addTrafficCar reparent shortcut)
