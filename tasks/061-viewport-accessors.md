---
id: 061
status: todo
branch:
depends: [063]
---
# render: Viewport accessors (aspect / width / height / top / left / resize)

## Goal
Replace six stubs in the `java.render.Viewport` registry block (`src/script/natives.cpp`,
rows 4647-4651 and 4653) with natives that read and write the script viewport's relative
rect. The exe gets the native payload from the receiver's `Native.ptr` ref node and keeps
the rect in the handle's typed data block at `+0x10..+0x1c` (notes/12 §3, notes/13 §1).
`src/` has no typed-data layer, so the four floats go on the bound `Runtime::Resource`
(the exe's RI) — an `INVENTED:` mapping. `Viewport.create` (0x4e6af0) is still a stub, so
nothing binds a resource or fills the rect yet: without a handle the exe's four getters
return the raw receiver slot and the port returns `0.0f` (see UNRESOLVED), `resize` is a
no-op, and `getAspect` cannot compute the backbuffer ratio and returns the exe's own
fallback `1.0f`.

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers:
- `getArgs` = FUN_004b1af0 (0x4b1af0): out0 = receiver (script instance), then one out per
  signature char (`F`/`I` copy the 32-bit argument value).
- `unwrap` = FUN_004ecc70 (0x4ecc70, "Native__instanceToHandle", notes/06, notes/13 §1):
  receiver → its `Native.ptr` ref node; 0 when absent. In src this is
  `value_handle(a[0])` → `Runtime::Handle` (`target` = RI).
- `typed(h, mask)` = FUN_00421610 (0x421610): `RI = *(h+0xc)` (= `h->target`); if RI == 0
  return 0; if `*(int*)(RI+0x7c) != 1` call `RI->vf5(+0x14)(1.0f)` (make resident); if
  `Resource__getTyped(mask, 0.0f, 10.0f)` (FUN_0051f4f0) is negative return 0; otherwise
  return `RI->vf3(+0xc)(1.0f)` (typed data, may be 0).
- `aspectOf(h)` = FUN_004e9a10: same guard chain on RI with mask `0xa0000001`; RI == 0 or
  typed == 0 → `1.0f`; otherwise
      bb = backbufferSize();                                      // FUN_004210f0
      return ((float)bb.out1 * *(float*)(T+0x18)) /
             ((float)bb.out2 * *(float*)(T+0x18));                // = out1 / out2
  (the `typed+0x18` factor appears in numerator and denominator and cancels).
- `backbufferSize()` = FUN_004210f0 (0x4210f0): thiscall on the game object
  `DAT_005da014`; calls C3DLow vtable slot `+0x64` (index 25; notes/12 §2 names it
  `Game__getViewportSize`) with two `int*` out-args, then `CVTDQ2PS` both. Ghidra could
  not recover the indirect target (see UNRESOLVED).

### 0x4e6db0 Viewport.getAspect()F
    self = out0;
    h = unwrap(self);
    f = 1.0f;                                   // MOV [EBP-8], 0x3f800000
    if (h == 0) return f;                       // 0x4e6de9: FLD [EBP-8]
    return aspectOf(h);                         // tail call; no fallback assignment

### 0x4e6ee0 Viewport.getWidth()F
    self = out0; result = self;                 // result slot is the parsed receiver
    h = unwrap(self);
    if (h != 0) {
        T = typed(h, 0x80000001);
        if (T != 0) result = *(float*)(T + 0x18);
    }
    return result;                              // 0x4e6f1f: FLD float ptr [EBP-4]
### 0x4e6df0 Viewport.getHeight()F — identical body, reads `T + 0x1c`
### 0x4e6e90 Viewport.getTop()F    — identical body, reads `T + 0x14`
### 0x4e6e40 Viewport.getLeft()F   — identical body, reads `T + 0x10`

### 0x4e6f30 Viewport.resize(FFFF)V
    self, left, top, width, height = getArgs(...);
    h = unwrap(self);
    if (h == 0) return;
    T = typed(h, 0x80000001);
    if (T == 0) return;
    *(float*)(T + 0x10) = left;
    *(float*)(T + 0x14) = top;
    *(float*)(T + 0x18) = width;
    *(float*)(T + 0x1c) = height;

Offsets: notes/12 §3 records the res_viewport data as relative rect x,y,w,h at
`+0x10..+0x1c`; the four getters read exactly those words (left, top, width, height).

## Files
- edit: `src/game/runtime.h` — `Runtime::Resource`, after `float detail = 0.0f;` (~line 64):
  ```cpp
  // res_viewport data +0x10..+0x1c (notes/12 §3): relative rect x,y,w,h.
  // INVENTED default 0: src does not parse res_viewport (type 0x12) record data.
  float viewport_rect[4] = {};
  ```
- edit: `src/script/natives.cpp` — add six functions immediately before
  `namespace natives {` (line 4385), using `need_rt`, `receiver_object`, `value_resource`,
  `want`, `get_f` (lines 973-997 / 43-74); change registry rows 4647-4651 and 4653 from
  `nullptr, true` to the new functions with `false`. Leave `unproject` (4652) a stub.
- read only: none.

## Changes
1. `vp_get_aspect`: `need_rt("Viewport.getAspect")`, `receiver_object(...)`, then
   `Runtime::Resource *r = value_resource(a[0]); (void)r;` and
   `*out = Value::make_float(1.0f);`. Comment:
   ```
   // 0x4e6db0 Viewport.getAspect: exe = backbuffer w / backbuffer h (FUN_004210f0,
   // C3DLow vtable+0x64). shortcut: no backbuffer-size service in src/; 1.0f is the
   // exe's own no-handle / no-typed-data value (0x4e6de9 / 0x4e9ab0 FLD1). INVENTED:
   // the ratio is not computed even when a resource is bound.
   ```
2. `vp_get_width` (= `r->viewport_rect[2]`), `vp_get_height` (`[3]`), `vp_get_top`
   (`[1]`), `vp_get_left` (`[0]`): same guards; `*out = Value::make_float(r ?
   r->viewport_rect[i] : 0.0f);`. Comment on each:
   ```
   // 0x4e6ee0 Viewport.getWidth: exe returns typed viewport data +0x18.
   // INVENTED: missing handle/typed data returns 0.0f; the exe's fallback is the raw
   // receiver slot (0x4e6f1f FLD [EBP-4]), which has no rebuild equivalent.
   ```
   (adjust address/name/offset per function).
3. `vp_resize`: `need_rt`, `receiver_object`, `want("Viewport.resize", a, 5, err)`, read
   `a[1..4]` with `get_f` into `float v[4]`, then `if (r) for (int i = 0; i < 4; ++i)
   r->viewport_rect[i] = v[i];`, `*out = Value::make_null();`.
   Comment: `// 0x4e6f30 Viewport.resize: writes typed viewport data +0x10..+0x1c; no-op without a handle.`
4. Do not log the missing-handle case (src render/resource natives do not reproduce the
   exe's game log).

## Done when
- each function has its `// 0xADDR Viewport.<name>` comment; the resource mapping and the
  getter fallback are `INVENTED:`, the aspect limitation is a `shortcut:`
- with no bound resource: getters return 0.0f, `getAspect` 1.0f, `resize` is a no-op;
  with a bound `Runtime::Resource`, getters return `viewport_rect[0..3]` and `resize`
  stores left, top, width, height in that order
- registry rows 4647-4651, 4653 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- getWidth/Height/Top/Left when `unwrap` is 0 (or the typed block is 0): the exe returns
  the untouched receiver out-slot, `0x4e6f1f: FLD float ptr [EBP + -0x4]` (that slot was
  written by `FUN_004b1af0` with the receiver). This is not a defined script-level float;
  the port maps it to `0.0f` (INVENTED).
- `getAspect`'s real value: `FUN_004210f0` is an indirect call Ghidra could not resolve
  (`WARNING: Could not recover jumptable at 0x004210fc. Too many branches`; raw
  `(**(code **)(**(int **)(in_ECX + 0x404) + 100))();`). Which out-arg is width vs height
  is not re-derived; notes/12 names the slot `Game__getViewportSize`. No service in src/,
  so the port returns 1.0f.
- Typed-data contract: `FUN_0051f4f0` was observed returning only `0` / `0x80000001`, while
  callers test `< 0` (`0x421610`) resp. `JS` (`0x4e9a10`); the masks `0x80000001`
  (getters/resize) and `0xa0000001` (aspect) are not resolved. The `+0x10..+0x1c` rect
  layout is taken from notes/12 §3, not re-derived here.
- No producer: `Viewport.create` 0x4e6af0 is still a stub and nothing binds a Viewport
  script object to a `Runtime::Resource`. Its raw body is
  `uVar2 = FUN_00525120(0,local_1c,local_18,local_14,local_10,local_c); FUN_0040cac0(uVar2);`
  after the handle check; `FUN_00525120`/`FUN_0040cac0` are untraced. Until a create card
  lands, these six natives always take the fallback branches.

## Gate (Claude)
Blocked: without a producer (Viewport.create 0x4e6af0 -> 0x525120/0x40cac0) every
getter takes the fallback path, so porting now only adds an unused field. Re-open after
the Viewport.create/destroy card (063) is traced and merged; fold the rect into that
card's viewport object instead of `Runtime::Resource`.

## Gate 2 (Claude) — SCOPE OVERRIDE after card 063
Card 063 adds `Runtime::Resource::viewport_rect[4]` and binds it in `Viewport.create`.
Do NOT edit src/game/runtime.h here: use the 063 field as is. Getters read
`viewport_rect[0..3]` (left/top/width/height); `resize` writes it; both only when the
bound resource's `type == 0x12`, otherwise the fallbacks below. Files: src/script/natives.cpp only.
