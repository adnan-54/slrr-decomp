---
id: 110
status: merged
branch: card/110
depends: []
---
# render: Text.getWidthPixels / Viewport.unproject / GfxEngine.printScreen (no-service ports)

## Goal
Turn the three remaining render stubs of the native table into real functions with
the exe's argument binding and its no-service behaviour. None of them can compute
anything in the rebuild: `src/script/natives.cpp` includes no `render/` header (the
`render::Renderer`/`TextRenderer`/`Device` objects are not reachable from
`Runtime`), the font-atlas loader behind the text service is unread
(`notes/12-render.md` §11 "Open — font atlas": "Atlas load plus glyph metrics
behind `vf131 setFont` / `FUN_004d8710` are still unread"), the game's viewport/camera node lists
(`game+0x198` / `game+0x14c`) and the camera matrices behind unproject are not
modelled, and `render::Device` has no backbuffer readback. So each native must
return exactly what the exe returns on those failure paths: `0.0f`, null, and
(void) nothing. Do not invent a render service and do not approximate a visible
result.

## Exe evidence (cleaned pseudocode, renamed fields)

Argument unpacking (unchanged conventions, already in src): `getArgs` =
`FUN_004b1af0` (0x4b1af0) writes out0 = receiver, then one out per declared
parameter in declaration order (`want()` + `get_*`/`value_*` in src);
`instanceToHandle` = `FUN_004ecc70` (0x4ecc70) = `game_rt()->handle(o)`.
"A null String" below is `Value::Null` / a non-`Str` value in src.
Hex floats: `0x3f800000` = 1.0f. Multiplier `0.1` = `0x3dcccccd`, `-0.1` = `0xbdcccccd`.

### 0x4e5ae0 java.render.Text.getWidthPixels(String, ResourceRef)F
```c
getArgs(ctx, 0, &str, &font);          // receiver skipped
if (str == 0) return 0.0f;             // null String -> 0
return Game__textWidth(font, str, 0x3f800000 /*1.0f*/, 0);   // 0x421100
```
### 0x421100 Game__textWidth (only helper needed)
```c
if (*(int *)(font + 8) == 0) return 0;              // font ref id 0
svc = FUN_00421610(0x80000000); if (!svc) return 0;
text = *(int **)(svc + 0x18);  if (!text) return 0;
if (text[0x1f] != 1) call [text + 0x14](1.0f);      // font object method +0x14
if (FUN_0051f4f0(0x80000000, 0, 0) < 0) return 0;
fontTex = call [text + 0xc](1.0f); if (!fontTex) return 0;   // font texture
n = *(int *)(fontTex + 0x18);
if (!n || (int)*(uint *)(n + 0x38) < 0 || (*(uint *)(n + 0x38) & 8) == 0)
    return 0.0f;                                     // atlas not loaded
renderer = **(int **)(PTR_DAT_005da014 + 0x404);     // decompiler line, two loads
call [renderer + 0x20c](texture);                    // vf131 setFont
call [renderer + 0x218](param4 /*0*/);               // vf134 set scale
return call [renderer + 0x214](str, 1.0f);           // vf133 textWidth
```
`+0x20c/+0x214/+0x218` = the note's vf131/vf133/vf134 text pair (notes/12 §4
table). Every early-out returns 0.0f, so the rebuild's result is 0.0f.

### 0x4e6fb0 java.render.Viewport.unproject(Vector3, I)Vector3
```c
getArgs(ctx, &vpInst, &vec, &camId);              // vpInst = receiver
out = {0,0,0};
h = instanceToHandle(vpInst);                     // 0x4ecc70 -> vp resource
out.x = getF(vec, "x"); out.y = getF(vec, "y"); out.z = getF(vec, "z");  // 0x4ec980
node = FUN_00422320(h);                           // 0x422320
if (node) {
  for (child = FUN_00421260(node); child; child = FUN_004210d0(child)) {
    if (*(int *)(child + 0xa98) == camId) {       // camera id on the render node
      FUN_00421e20(child, &world, &out);          // 0x421e20 -> 0x461c20
      v = new java.lang.Vector3;                  // 0x567a20(0x1c) + ctor, x=y=z=0
      setF(v, "x", world.x * 0.1f);               // 0x4ed080, DAT_005a4d28
      setF(v, "y", world.y * -0.1f);              // DAT_005a4d30
      setF(v, "z", world.z * 0.1f);               // DAT_005a4d40
      return v;
    }
  }
}
return 0;                                         // null
```
Helpers, decoded:
- `0x422320`: `in_ECX` = game; walks `game+0x198` then `game+0x14c`; a node matches
  when `*(int *)(node + 0x34) == *(int *)(h + 8)` (the viewport resource id;
  `notes/12` §3: `+0x34` = res_viewport id). Returns 0 when no node matches.
- `0x421260` = `0x465100(node)` → `n = *(int*)(node+8)`, return `n` when
  `n != 0 && *(int*)(n+8) != 0`, else 0 (first child).
- `0x4210d0(child)` = `c = *(int*)(child+8)`; return `c` when `c && *(int*)(c+8)`,
  else 0 (next child).
- `0x421e20(node, out, in)`: `if (node) 0x461c20(out, in)`; returns `out`. The
  disassembly (bytes at 0x421e20: `8b 4d 08` … `e8 eb fd 03 00`) shows the node is
  passed in ECX, so 0x461c20 is a method on the camera render node.
- `0x461c20` (unproject math; D3DX row-major storage, `m(i,j)` at
  `base + 0x10*i + 4*j`, row-vector convention `p' = p·M`). With `p = (x,y,z,1)`
  and the 4x4 at node `+0x45c`:
  `d = 1 / (m(0,3)*x + m(1,3)*y + m(2,3)*z + m(3,3))`  // 0x468,0x478,0x488,0x498
  `vx = (m(0,0)*x + m(1,0)*y + m(2,0)*z + m(3,0)) * d`   // 0x45c,0x46c,0x47c,0x48c
  `vy = (m(0,1)*x + m(1,1)*y + m(2,1)*z + m(3,1)) * d`   // 0x460,0x470,0x480,0x490
  `vz = (m(0,2)*x + m(1,2)*y + m(2,2)*z + m(3,2)) * d`   // 0x464,0x474,0x484,0x494
  then with the 4x4 at node `+0x39c`:
  `out.x = m(0,0)*vx + m(1,0)*vy + m(2,0)*vz + m(3,0)`  // 0x39c,0x3ac,0x3bc,0x3cc
  `out.y = m(0,1)*vx + m(1,1)*vy + m(2,1)*vz + m(3,1)`  // 0x3a0,0x3b0,0x3c0,0x3d0
  `out.z = m(0,2)*vx + m(1,2)*vy + m(2,2)*vz + m(3,2)`  // 0x3a4,0x3b4,0x3c4,0x3d4
  Compare the project path `0x4609d0` (world→screen): first the 4x4 at `+0x3dc`
  (no divide), then the 4x4 at `+0x41c` with the reciprocal divide. So the four
  matrices are laid out at `+0x39c` (camera→world), `+0x3dc` (view), `+0x41c`
  (projection), `+0x45c` (inverse projection; the writer of this slot was not
  traced — inferred from the project/unproject pair).
All failure paths (`h` null → the exe faults in 0x422320; no node / no child with
that id) return null; src has no node list at all, so the lookup never matches.

### 0x4e5780 java.render.GfxEngine.printScreen(String)V
```c
getArgs(ctx, 0, &name);                  // receiver skipped
Game__printScreen(name);                 // 0x421b40, returns void
```
### 0x421b40 Game__printScreen
```c
if (!name || name[0] == '\0') return;                 // nothing written
pixels = game->c3dlow(vf[0x50])(&dim0, &dim1);        // lock backbuffer copy; w,h
if (!pixels) return;
f = FUN_005657b0(name);                               // open: '/' -> '\', VFS open
if (!f) return;
raw = alloc(dim0*dim1*3 + 0x40);                      // 0x40 header + BGR pixels
dst = alloc(dim0*dim1*3);
raw[0] = 1; raw[1] = dim0*dim1*3 + 0x40; raw[2] = dim0; raw[3] = dim1;
raw[4] = 0x40; raw[5] = dim0*dim1*3; raw[6] = 0; raw[7] = 0;
for (i = 0; i < dim0*dim1; i++) {                     // 4-byte source -> RGB, at raw+0x40
    raw[0x40+3*i+0] = pixels[4*i+2];
    raw[0x40+3*i+1] = pixels[4*i+1];
    raw[0x40+3*i+2] = pixels[4*i+0];                  // source 0<->2 swap, alpha dropped
}
size = dim0*dim1*3;
FUN_00527060(dst, &size, raw, DAT_005da334);          // compress (quality global)
FUN_00565950(f, dst, size);                           // write
FUN_00564dd0(f);                                      // close
game->c3dlow(vf[0x54])();                             // unlock
return;
```
`UNRESOLVED`: 0x5657b0/0x565950/0x564dd0 are file open/write/close by role (the
decompiler types 0x5657b0 void although its return is tested); the exact output
container written by 0x527060 is not decoded. Not needed for this card.

## Files
- edit: src/script/natives.cpp
  - new `tx_get_width_pixels` next to the other `java.render.Text` natives,
    new `vp_unproject` next to the Viewport accessors (after `vp_resize`), new
    `gfx_print_screen` next to the other `GfxEngine` natives.
  - registry rows (replace `nullptr, true` with the function, `false`):
    line 7604 `GfxEngine.printScreen`, line 7628 `Viewport.unproject`,
    line 7639 `Text.getWidthPixels`.
- read only: src/render/ui.h (FontAtlas/TextRenderer — the future service),
  src/game/runtime.h (viewport resource fields).

## Changes
1. `tx_get_width_pixels(Vm&, const Args&, Value*, std::string*)`:
   `want("Text.getWidthPixels", a, 3, err)`; a non-`Str` `a[1]` is the exe's null
   String (`INVENTED` safety: return 0.0f, no error, matching `if (str == 0)`);
   then `*out = Value::make_float(0.0f)`.
   Comment:
   ```
   // 0x4e5ae0 Text.getWidthPixels(String, ResourceRef)F
   // SERVICE MISSING: the exe measures with the D3D text object (0x421100: vf131
   // setFont, vf134 scale 1.0, vf133 textWidth(str, 1.0)). src has no font-atlas
   // loader (notes/12 §4), so every path returns the exe's no-service 0.0f
   // (null String, font id 0, no service, atlas flag +0x38 bit 3 clear).
   ```
   Do not read the ResourceRef and do not touch render::TextRenderer.
2. `vp_unproject(Vm&, const Args&, Value*, std::string*)`:
   `need_rt("Viewport.unproject", err)`, `want("Viewport.unproject", a, 3, err)`,
   then `*out = Value::make_null()`. Do not read the receiver, the Vector3 or the
   int id: in src every path ends in null, and reading them would only add error
   paths the exe does not have. Comment:
   ```
   // 0x4e6fb0 Viewport.unproject(Vector3,I)Vector3
   // SERVICE MISSING: 0x422320 finds the viewport node in the game lists
   // +0x198/+0x14c by node+0x34 == resource id, walks its children and unprojects
   // through the child whose +0xa98 == camId with the camera matrices at +0x45c
   // (inverse projection) and +0x39c (camera->world). src has no game node list,
   // no viewport->camera link (cam_create, card 072) and no camera matrices, so
   // the lookup never matches: the exe returns null there (0x4e6fb0 fallthrough).
   // shortcut: the Vector3 x/y/z reads (0x4ec980) are skipped: their values are
   // only consumed by the transform that never runs.
   ```
3. `gfx_print_screen(Vm&, const Args&, Value*, std::string*)`:
   `want("GfxEngine.printScreen", a, 2, err)`; `*out = Value::make_null()`.
   Comment:
   ```
   // 0x4e5780 GfxEngine.printScreen(String)V -> 0x421b40 Game__printScreen
   // SERVICE MISSING: 0x421b40 locks a copy of the backbuffer (C3DLow vf[0x50]),
   // converts the 4-byte pixels to 3-byte RGB, compresses (0x527060) and writes; src has no
   // backbuffer readback (render::Device) and no image writer. Void native, so the
   // observable result is the exe's null/empty-name and lock-failure path:
   // nothing is written.
   ```

## Done when
- each function carries its `// 0xADDR Name` comment; missing services are marked
  `SERVICE MISSING:`, every src-side guess `INVENTED:`; no render service added
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch `card/110`
