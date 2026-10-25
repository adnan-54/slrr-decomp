---
id: 182
status: merged
branch: card/182
depends: []
---
# Trigger sensors: sphere sensors in the trigger pass + `type_trigger` vf7 0x4d4af0 move/resize

## Why (notes/19 s8.2)
The bot's obstacle sensor `bot_trigger` is a `native trigger` sphere (`"x,y,z,0,0,0,sphere,100"`, type
system.rpk 0x34, the same type Trigger.java uses), moved and resized every bot tick through `type_trigger` vf7
0x4d4af0. src's trigger pass only tests box sensors (`trigger_pass`, world.cpp: "Sphere/rod sensors are
skipped") and has no move entry. This card adds both; card 183 uses them.

## Exe evidence
Sphere shape (notes/11 s1 table): kind 1, `+0xb4 = r`, `+0x10 = r` (margin radius), `+0xc = r + 0.05` (bound);
its V-Clip polyhedron is `Poly__buildPoint`: one vertex at the body origin. Pair test (notes/11 line 104,
0x4fd910): V-Clip distance `d` between the sensor poly and the other hull; contact when `d < rA + rB`
(rB = 0 for box/mesh hulls) -> `0x502d60` addTriggerContact, exactly as the box path does today.
```
0x4d4af0 type_trigger vf7 (typed, params "fffffff" = x,y,z,yaw,pitch,roll,r):
  x = y = z = 0; ypr: 0x56a1a0 leaves unparsed ones at the Vec3 default; r = -1
  scan params "x,y,z,ya,pi,ro,r" (0x56a1a0 "fffffff")
  R = Math__eulerToMat 0x5666d0(ypr)
  shape = body.def.shape (typed+0x1c -> def+0x90)
  if (r > 0 && shape.kind == 1 /*sphere*/) {
      shape+0xb4 = r; shape+0xc = r + 0.05; shape+0x10 = r          // radius, bound, margin
      (+0x30 render helper resize: debug render object only, no effect)
  }
  0x4eac50(&pos, R)               // place the instance/body at pos with rotation R
  body = typed body (0x4eabc0 -> +0xb4)
  body.state.vel (+0x70..+0x78) = 0; 0x506ce0(&zero) (angular velocity = 0)
  clear state flag 2 (+0xa0 & ~2) in both state slots                 // wake/"moved" bit
```
Caller (bot tick 0x47f980 tail, card 183): `0x4eb3f0(+0x26f8 ref, str)` resolves the ref's typed native object
and calls its vtable+0x1c (= 0x4d4af0 for vtable 0x5a00e0) with `str`.

## Files
- edit: src/physics/world.cpp — `trigger_pass`: accept `SHAPE_SPHERE` sensors: sensor poly = one vertex at the
  origin (build once like `box_poly`, e.g. via the same `poly_from_triangles`-free path or a 1-vertex Poly;
  see src/physics/vclip.h), contact when `vclip_closest(...) < a->def.shape.radius` (box stays `< 0`)
- edit: src/game/runtime.cpp — new `void move_trigger(Runtime::Resource *r, const std::string &params)` next to
  `create_trigger_body`: parse with the same scan helpers (7 floats, stop at first mismatch), radius update only
  when r > 0 and the shape is a sphere (`radius`, and whatever src uses as bound: `bounding_radius()` already adds
  0.05), set `pos`, `rot` (euler_to_mat + mat_to_quat as in create), zero linear and angular velocity
- edit: src/game/runtime.h — declare `move_trigger` (or put it in the game namespace header that exposes
  create_trigger_body's neighbours)

## Done when
- `// 0x4d4af0` and `// 0x4fd910 sphere` comments; untraced bits (`+0xa0` flag 2, 0x4eac50 internals) `INVENTED:`
  or shortcut-commented
- `make -C src clean check` green; the 42 box race triggers behave as before (existing vmboot checks); no new tests
- card frontmatter: status implemented, branch card/182
