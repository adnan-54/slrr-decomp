---
id: 062
status: todo
branch:
depends: []
---
# render/script: java.render.Text colour/align/scale/pos accessors

## Goal
Replace the seven `java.render.Text` registry stubs (`setScale`, `setDefaultColor`,
`getDefaultColor`, `changeColor`, `changeAlign`, `setPos`, `getPos`) with natives.
The exe keeps the per-object state in its render-instance typed data (`+0x174` align,
`+0x17c` colour) and in the `"bone00"` node matrix (position), reached through the
script field `renderinst` (a `RenderRef`). `src/` has **no** render-instance typed-data,
residency or bone service, so: the process-wide default colour (`DAT_005dadac`) is
ported as is; colour/align/position state is mapped onto the bound `Runtime::Resource`
(INVENTED mapping, same pattern as cards 053/061); `setScale` has an unresolved write
target and stays a no-op. With no bound `renderinst` resource every setter is a no-op
and `getPos` returns null, which is the exe's behaviour when the handle/typed data is
absent. `Text.create` (0x4e5b30) is still a stub, so no resource is bound yet.

## Exe evidence (cleaned pseudocode)
Helpers:
- `getArgs` = `Native__getArgs` 0x4b1af0: out0 = receiver, then one out per declared
  argument. When the receiver is unused the call passes `0` in its place
  (`FUN_004b1af0(ctx, 0, &arg)` in setDefaultColor).
- `renderinst` = `FUN_004ecfc0(self, "renderinst")` 0x4ecfc0:
  `FUN_004ecae0(self, "renderinst", "java.util.resource.ResourceRef")` then
  `FUN_004ecc70(field, DAT_007a2620)` (notes/06). Returns the field's `Native.ptr`
  handle, 0 when the field is missing/not a ResourceRef. In `src/` this is
  `vm.get_instance_field(self, "renderinst", &v, &e)` + `value_resource(v)`.
- `RI` = `*(int **)(h + 0xc)` = `h->target` in `src/`. Standard guard after it:
  `if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);` then
  `if (FUN_0051f4f0(mask, 0, 10.0f) < 0) T = 0; else T = RI->vf[+0xc](1.0f);`
  (`FUN_0051f4f0` = `Resource__getTyped`, 0x51f4f0). `src/` has no typed-data layer:
  the bound `Runtime::Resource` is used as `T`.
- `setFloatField` = 0x4ed080; `new Vector3` allocation is 0x567a20 + class lookup
  0x55add0 (src: `new_point`).
- Constants: `0x80000001`=2147483649, `0x80000000`=2147483648, `0xa0000001`=2684354561
  (typed-data masks); `0x3f800000`=1.0f; `0x41200000`=10.0f; `0x3dcccccd`=0.1f
  (0.10000000149…).

### 0x4e5cd0 Text.setDefaultColor(I)V
```
FUN_004b1af0(ctx, 0, &color);      // receiver slot not stored, color = arg0
DAT_005dadac = color;              // process-wide default text colour
```

### 0x4e58d0 Text.getDefaultColor()I
Reported by notes/12 as disassembly `MOV EAX,[DAT_005dadac]; RET` (no `getArgs`).
Ghidra 12.1.4 has **no analysed function** at this address (`procedure_pseudo_code`
fails: "File offset 0x4e58d0 has no loaded source mapping"), so the body is taken from
that note, not re-decompiled here. See UNRESOLVED.

### 0x4e5c40 Text.changeColor(I)V
```
self = recv; color = arg0;
h = renderinst(self); RI = *(int **)(h + 0xc);            // no null check on h
if (RI != 0) {
    if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);
    if (FUN_0051f4f0(0x80000001, 0, 10.0f) < 0) T = 0;
    else T = RI->vf[+0xc](1.0f);
    if (T != 0) *(u32 *)(T + 0x17c) = color;              // 0x4e5c6a
}
```

### 0x4e5bb0 Text.changeAlign(I)V
Identical body to changeColor; the store is `*(u32 *)(T + 0x174) = align;`.

### 0x4e5d80 Text.setScale(FF)V
```
self = recv; sx = arg0; sy = arg1;
h = renderinst(self); RI = *(int **)(h + 0xc);
if (RI != 0) {
    ... typed(RI, 0x80000001) -> T;
    if (T != 0) {
        U = *(void **)(T + 0x114);
        if (U != 0) {
            if (U->+0x7c != 1) U->vf[+0x14](1.0f);
            if (FUN_0051f4f0(0xa0000001, 0, 0) >= 0) {
                V = U->vf[+0xc](1.0f);
                if (V != 0) {
                    W = FUN_004d5f50(0x80000000, 1.0f);       // this = UNRESOLVED
                    if (W != 0 && *(int *)(W + 0x18) != 0) {
                        vec = {sx, sy, 0.0f};
                        FUN_004d8710(&vec);                    // this = UNRESOLVED
                    }
                }
            }
        }
    }
}
```
`FUN_004d8710` (0x4d8710) walks `this+0x64` groups (`this+0x60` array), each entry's
`+8` count / `+4` array, and calls `node->vf[+0x4c](vec)` on every node.

### 0x4e5d00 Text.setPos(FF)V
```
self = recv; x = arg0; y = arg1;
renderinst(self);                          // return value used as the implicit this below
FUN_005663c0(&mat4x4);                     // identity: [0]=[5]=[10]=1, [1][2][4][6][8][9]=0,
                                           // [3][7][11] and [12..15] untouched
mat4x4[12] = x; mat4x4[13] = y; mat4x4[14] = 0.0f;
bone = FUN_004e77f0("bone00");             // this = handle; FUN_0051b940(name) lookup
FUN_004ec470(bone, 0, &mat4x4[12], mat4x4);
```
Inside `FUN_004ec470` (0x4ec470): guard on `this+8`/`this+0xc` (handle id/RI), typed
mask `0x80000000`, `node = FUN_0051c450(bone)`; if `node` and both pointers are non-null:
`FUN_00567570(mat, &mat[12]); FUN_004ec0f0(node, local_44); FUN_004e84c0();`, then
`T2->+0x1c0 = DAT_005dadbc` (dirty/version marker).

### 0x4e58e0 Text.getPos()Ljava.lang.Vector3;
```
FUN_004b1af0(ctx, &self, local_1c, local_18);   // 3 out slots for a 0-arg signature (see UNRESOLVED)
renderinst(self);
bone = FUN_004e77f0("bone00");                  // this = handle
if (FUN_004ea7f0(bone, 0, mat16) != 0) {        // copies node +0x60..0xa0 (16 floats)
    v = new java.lang.Vector3;                  // 0x1c bytes
    setFloatField(v, <0x5a3410>, mat16[12] * 0.1f);
    setFloatField(v, <0x5a3428>, mat16[13] * 0.1f);
    setFloatField(v, <0x5a3458>, mat16[14] * 0.1f);
    return v;
}
return null;                                    // 0x4e58e0 returns 0 when the bone path fails
```
`mat16[12..14]` are the matrix translation words (`local_60` is 64 bytes; `local_30/2c/28`
are its floats 12/13/14). Field-name strings at 0x5a3410/0x5a3428/0x5a3458 were not read
here; `Vector3.java` declares `float x,y,z;` and `new_point` writes x/y/z in that order.

## Files
- edit: `src/game/runtime.h` — `Runtime::Resource`, after `float detail = 0.0f;` (~line 64):
  ```cpp
  // java.render.Text state. The exe keeps colour/align in the render-instance typed
  // data (+0x17c/+0x174, Text__create 0x4e8f40) and the position in the "bone00"
  // node matrix; src has no typed-data/bone service, so this resource stands in.
  // INVENTED mapping; defaults are Text__create's (notes/12).
  int32_t text_align = 2;                // typed+0x174
  uint32_t text_color = 0xffffffffu;     // typed+0x17c (DAT_005dadac default)
  float text_pos[3] = {};                // bone00 matrix translation [12..14], raw
  ```
- edit: `src/script/natives.cpp` — add `g_default_text_color`, `text_renderinst` and the
  seven `tx_*` functions at the end of the anonymous namespace (after
  `snd_enable_debug_dump`, before `}  // namespace`, line 4383); change registry rows
  4654-4656 and 4658-4661 from `nullptr, true` to the new functions with `false`.
- read only: `src/render/ui.h` (`kDefaultTextColor` = `DAT_005dadac` = 0xffffffff).

## Changes
1. File-scope global in the anonymous namespace:
   ```cpp
   // DAT_005dadac, the exe's process-wide default text colour (same value as
   // render::kDefaultTextColor in render/ui.h).
   // shortcut: render::TextRenderer::default_color_ is not driven by it; no script
   // text renderer is created yet.
   uint32_t g_default_text_color = 0xffffffffu;
   ```
2. Shared resolver:
   ```cpp
   // 0x4ecfc0: renderinst field (a RenderRef) -> its Native.ptr handle -> RI.
   Runtime::Resource *text_renderinst(Vm &vm, Object *self)
   {
       Value v;
       std::string e;
       if (!vm.get_instance_field(self, "renderinst", &v, &e)) return nullptr;
       return value_resource(v);
   }
   ```
3. `tx_set_default_color` (0x4e5cd0): `want(...,2)` + `get_i(..., a[1], &color)`;
   `g_default_text_color = (uint32_t)color;` return null. The exe does not read the
   receiver here, so do not require `a[0]` to be an object.
4. `tx_get_default_color` (0x4e58d0): no argument checks (the exe body has no `getArgs`);
   `*out = Value::make_int((int32_t)g_default_text_color);`.
5. `tx_change_color` (0x4e5c40) and `tx_change_align` (0x4e5bb0):
   `need_rt`, `receiver_object`, `want(...,2)`, `get_i(a[1])`;
   `if (Runtime::Resource *r = text_renderinst(vm, o)) r->text_color = (uint32_t)color;`
   (align: `r->text_align = align;`). Comment each with the typed mask and offset.
6. `tx_set_pos` (0x4e5d00): `need_rt`, `receiver_object`, `want(...,3)`,
   `get_f(a[1])`/`get_f(a[2])`; if `r`: `r->text_pos[0] = x; r->text_pos[1] = y;
   r->text_pos[2] = 0.0f;` (raw matrix translation, as the exe writes it).
7. `tx_get_pos` (0x4e58e0): `need_rt`, `receiver_object`; `r = text_renderinst(...)`;
   if `!r` return null (the exe's bone path fails); else
   `float p[3] = {r->text_pos[0] * 0.1f, ..., ...}` and `new_point(vm, "java.lang.Vector3", p, false, err)`.
8. `tx_set_scale` (0x4e5d80): `want(...,3)` + two `get_f` calls, then return null — no
   state write, because the write target is the font render-group chain and is
   UNRESOLVED (see below); the exe itself is a no-op when that chain is absent, which
   is always the case in `src/`. Mark it `shortcut:`.
9. Keep `Text.create` (4657), `update` (4662), `getWidthPixels` (4663) and the out-arg
   `getPos(Vector3)` (4664) as stubs.

## Done when
- each ported function has a `// 0xADDR java.render.Text.<name>` comment; the
  `Resource` mapping and the `0.1f`/raw-position handling are marked `INVENTED:`, the
  scale gap is a `shortcut:`
- `setDefaultColor(c)` then `getDefaultColor()` returns `c` (default 0xffffffff); this
  is process-wide, independent of any resource
- with no bound `renderinst` resource: the four setters are no-ops and `getPos` returns
  null; with a bound resource: colour/align are stored and `getPos` returns
  `text_pos * 0.1f` as x/y/z
- registry rows 4654-4656, 4658-4661 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `getDefaultColor` 0x4e58d0: no analysed function, the decompile fails with "File offset
  0x4e58d0 has no loaded source mapping"; body from notes/12 (`MOV EAX,[DAT_005dadac];
  RET`). The port returns `g_default_text_color`, which is the same global the setter
  writes; this is the exe's likely body but was not re-derived in this session.
- `getPos` argument binding: the raw line is
  `FUN_004b1af0(param_1,&local_14,local_1c,local_18)`, three out slots for the registered
  zero-arg signature `()Ljava.lang.Vector3;`; Ghidra's variadic inference is suspect.
  The port binds the receiver only (cf. the zero-arg `Viewport.getWidth` 0x4e6ee0,
  which passes one out slot).
- All five render-instance natives dereference the handle without a check:
  `iVar2 = FUN_004ecfc0(...); piVar1 = *(int **)(iVar2 + 0xc);`. A missing
  `renderinst` field would read address 0xc (crash); the port guards with `r == 0`
  (INVENTED safety, same fallback the exe takes when the RI/typed data is 0).
- `setScale` write target: `FUN_004d5f50(0x80000000,0x3f800000)` and
  `FUN_004d8710(&local_1c)` are thiscalls and the decompile does not show their ECX
  receiver; `T+0x114`, typed mask `0xa0000001` and `W+0x18` are untraced. Hence the
  no-op (exe behaviour only when the chain is absent). Raw lines:
  `iVar2 = FUN_004d5f50(0x80000000,0x3f800000);` /
  `FUN_004d8710(&local_1c);`.
- Bone path: `FUN_004e77f0` (0x4e77f0, name lookup `FUN_0051b940`), `FUN_004ea7f0`
  (reads node +0x60..0xa0), `FUN_004ec470`/`FUN_0051c450`/`FUN_00567570`/
  `FUN_004ec0f0`/`FUN_004e84c0` are untraced; `DAT_005dadbc` (marker written to
  `T2+0x1c0`) is unidentified. Storing the raw translation in `Resource::text_pos` and
  multiplying by 0.1f in the getter is an INVENTED mapping reproducing the exe's
  observed script-level round-trip (setPos raw, getPos x0.1), not the matrix itself.
- In `setPos`/`getPos`/`FUN_004ec470` the `FUN_004ecfc0` return value is discarded in
  the pseudocode, while the callee reads `this+8`/`this+0xc` as the handle-id/RI pair
  (`FUN_004ec470:` `if (*(int *)(in_ECX + 8) == 0) return 0;` `piVar1 = *(int **)(in_ECX + 0xc);`).
  The port assumes that implicit `this` is the `renderinst` handle; the register
  plumbing was not re-derived.
- Typed-data masks `0x80000001` / `0x80000000` / `0xa0000001` and the
  `RI+0x7c`/`+0x114`/`+0x1c0` layout are not resolved; `src/` collapses typed data
  onto the bound `Runtime::Resource`, which is `INVENTED`.
- Vector3 field-name strings at 0x5a3410/0x5a3428/0x5a3458 were not read; `new_point`
  writes `x`/`y`/`z` in that order (matches `Vector3.java`, not re-verified against the
  exe data).
- The shipped 2.2.1 `Text.java` declares only `changeColor/changeAlign/setPos/getPos` +
  `create/update/getWidthPixels` (no `setScale`/`setDefaultColor`/`getDefaultColor`);
  the target 2.3.1 exe registers all eleven (notes/06), so the source pair cannot be
  used to cross-check the three added methods.

## Gate (Claude) — SCOPE OVERRIDE, read this first
0x4e58d0 bytes `a1 ac ad 5d 00 c3` = `mov eax,[0x5dadac]; ret` (verified).
Implement ONLY Changes 1, 3, 4 (`g_default_text_color`, `setDefaultColor`,
`getDefaultColor`) and their two registry rows. Do NOT edit `src/game/runtime.h`
and do NOT add `text_renderinst`, changeColor/changeAlign/setPos/getPos/setScale:
they have no producer until `Text.create` 0x4e5b30 is traced, so they stay stubs.
Files for this card: src/script/natives.cpp only.
