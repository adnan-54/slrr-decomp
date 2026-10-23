---
id: 071
status: todo
branch:
depends: []
---
# render: RenderRef getTypeID / setColor / getBoneId / getDetail natives

## Goal
Turn four `java.util.resource.RenderRef` registry rows from `nullptr` into handlers:
`getTypeID()I` (0x4e23b0), `getDetail()F` (0x4e3330), `setColor(I)V` (0x4e2830),
`getBoneId(String)I` (0x4e2290). All four resolve the script receiver's ref node and
check the resource; the renderer-side data they touch (typed render data, colour
override, bone table) has **no service in src/**, so those branches are shortcuts that
return the exe's own fallback value (0 / 0.0f / void). `getDetail` is the same exe
function already implemented for `GameRef` — reuse `gr_get_detail`, do not duplicate it.

## Exe evidence (cleaned pseudocode, renamed fields)

Common: `FUN_004b1af0(self, &outs...)` parses the native signature (src: `want` +
`get_i`/`get_s`). `FUN_004ecc70(self, DAT_007a2620)` returns the int field holding the
ref-node pointer (src equivalent: `value_handle(self)`; the field name global
`DAT_007a2620` is runtime-initialised, zero in the file image). Ref node layout from
notes/06 + src `rr_id`: `+0xc` = RI (src `Handle::target`), `+8` = raw id (src
`Handle::id`). `RI+0x7c` = resource type (src `Resource::type`); 3 = `ins_render`
(notes/01).

### 0x4e23b0 RenderRef.getTypeID()I
```
h = value_handle(self)                       // FUN_004ecc70
if (h == 0) {
    log "vm_renderref_gettype: scripted resource <id> not found in file \"...\" line <n>";
    return 0;
}
ri = h->target                               // *(int*)(h + 0xc)
if (ri != 0 && ri->type == 3) {              // *(int*)(ri + 0x7c) == 3
    blk = FUN_00421610(0xa0000001);          // thiscall on h; resident/typed-data fetch
    if (blk != 0) return *(uint32_t*)(blk + 0x110);   // UNRESOLVED: see below
}
return 0;
```

### 0x4e3330 RenderRef.getDetail()F   (already implemented as gr_get_detail)
```
h = value_handle(self)
if (h == 0) { log "vm_resourceref_getdetail: scripted resource ... not found ..."; return 0.0f; }
ri = h->target
if (ri != 0 && (ri->type == 1 || ri->type == 3)) {
    if (h->id == 0) return 0.0f;             // *(int*)(h + 8) == 0
    return *(float*)(ri + 0x9c);             // src Resource::detail
}
log "vm_resourceref_getdetail: Wrong ResourceType ... line 2651 (0xa5b)";
return 0.0f;
```
`gr_get_detail` (src/script/natives.cpp:1868) already returns `r->detail` for types 1/3
and 0.0f otherwise, reusing the `handle->id == 0` implication (bind sets id and target
together). Register it for RenderRef too.

### 0x4e2830 RenderRef.setColor(I)V
```
color = int arg                              // signature (I)V
h = value_handle(self)
if (h != 0) { FUN_004ebd20(color); return; } // thiscall on h (ECX = h)
log "vm_renderref_setcolor: scripted resource ... not found ...";
```
callee `FUN_004ebd20` (thiscall, receiver = ref node):
```
ri = h->target
if (ri != 0) {
    if (ri->type != 1) ri->vtbl[5](1.0f);              // vtbl+0x14: make resident
    if (FUN_0051f4f0(0x80000001, 0, 0) >= 0) {
        blk = ri->vtbl[3](1.0f);                       // vtbl+0xc: typed data
        if (blk != 0) *(uint32_t*)(blk + 0x17c) = color;
    }
}
```

### 0x4e2290 RenderRef.getBoneId(String)I
```
name = string arg                            // signature (Ljava.lang.String;)I
h = value_handle(self)
if (h != 0) return FUN_004e77f0(name);       // thiscall on h (ECX = h)
log "vm_renderref_getboneid: scripted resource ... not found ...";
return 0;
```
callee `FUN_004e77f0` (thiscall, receiver = ref node):
```
FUN_0040cac0(h->target);                     // bind/link the RI
ri = h->target
if (ri != 0) {
    if (ri->type != 1) ri->vtbl[5](1.0f);
    if (FUN_0051f4f0(0x80000000, 0, 0) >= 0) {
        blk = ri->vtbl[3](1.0f);
        if (blk != 0) return FUN_0051b940(name);         // bone name -> id
    }
}
return 0;
```

## Changes (src/script/natives.cpp only)
Add three handlers next to `gr_get_detail` / the ResourceRef block, same style
(`need_rt`, `receiver_object`, `want`, `get_i`/`get_s`, `value_handle`,
`value_resource`, `Value::make_int/make_float/make_null`):

1. `// 0x4e23b0 RenderRef.getTypeID()I` — `rr_get_type_id`: validate 1 arg; if
   `!value_handle(a[0])` print `vm_renderref_gettype: scripted resource not found\n`;
   result 0; when `r && r->type == 3` leave 0 with
   `// shortcut: render typed data (0x51f4f0 + vtbl+0xc, field +0x110) is not modeled; the exe returns 0 when the fetch fails (0x4e23b0).`
   Return the int.
2. `// 0x4e2830 RenderRef.setColor(I)V` — `rr_set_color`: 2 args + `get_i` into
   `int32_t color`; output null; if `!value_handle(a[0])` print
   `vm_renderref_setcolor: scripted resource not found\n`; else
   `if (Runtime::Resource *r = value_handle(a[0])->target) r->text_color = (uint32_t)color;`
   with `// 0x4ebd20: typed data +0x17c, the same slot Text__create 0x4e8f40 writes (card 067's
   INVENTED mapping Resource::text_color).` and
   `// shortcut: make-resident (vtbl+0x14) and the 0x51f4f0(0x80000001) fetch are collapsed.`
   (Gate note, Claude: the exe stores only when the typed fetch succeeds; src has no fetch
   failure, so it always stores for a bound resource.)
3. `getBoneId` 0x4e2290: **not in this card** (gate decision: bone lookup 0x4e77f0 is being
   traced for card 075). Leave its registry row a stub.

Registry (`src/script/natives.cpp`): replace the `nullptr` handler at
- `{"java.util.resource.RenderRef", "setColor", "(I)V", ...}` -> `rr_set_color`
- `{"java.util.resource.RenderRef", "getTypeID", "()I", ...}` -> `rr_get_type_id`
- `{"java.util.resource.RenderRef", "getDetail", "()F", ...}` -> `gr_get_detail`

## UNRESOLVED (not needed for the port; shortcuts return 0)
- `0x4e23b0`: `blk = FUN_00421610(0xa0000001); return *(undefined4 *)(blk + 0x110);` —
  what mask/id `0xa0000001` selects and what field `+0x110` holds.
- `0x4e77f0`: `uVar3 = FUN_0051b940(param_1);` — how the typed block reaches the bone
  lookup (Ghidra shows only the name argument) and what it returns for an unknown name.
- `0x4ebd20`/`0x4e77f0`/`0x421610`: `FUN_0051f4f0(0x80000001 | 0x80000000, 0, ...)` and
  `vtbl+0xc`/`vtbl+0x14` semantics; only the receiver (`in_ECX` = ref node, deref +0xc)
  is established.
- `DAT_007a2620` is zero in the file image (runtime-initialised field-name pointer);
  irrelevant to the port, `value_handle()` is the src equivalent.

## Files
- edit: src/script/natives.cpp (two handlers + three registry rows)
- read only: src/script/natives.cpp (ResourceRef/GameRef helpers above), src/game/runtime.h (`Handle`, `Resource`)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `shortcut:`/`INVENTED:`
- no renderer service invented; the exe fallbacks 0 / 0.0f / void are returned instead
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
