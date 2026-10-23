---
id: 076
status: todo
branch:
depends: []
---
# render/script: java.render.Text setScale / changeColor / changeAlign

## Goal
Replace the last three `java.render.Text` setters — `setScale(FF)V` (0x4e5d80),
`changeColor(I)V` (0x4e5c40), `changeAlign(I)V` (0x4e5bb0) — with natives.
`changeColor` writes the render-instance typed data at `+0x17c` (colour) and
`changeAlign` at `+0x174` (align), exactly the fields card 062 mapped to
`Runtime::Resource::text_color` / `text_align`. `setScale` does **not** write the
typed data: it walks the render group hanging off the text instance and calls
`node->vf[+0x4c](&vec)` on every node with `vec = {sx, sy, 0.0f}`. `src/` has no
render-node / typed-data layer, so the vector is mapped onto the bound
`Runtime::Resource` (INVENTED, same pattern as cards 062/067). `Text.create` (card 067)
already binds that resource, and `text_renderinst` already exists.

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers (already documented; not re-traced):
- `getArgs` = `Native__getArgs` 0x4b1af0: out0 = receiver, then one out per declared
  argument.
- `renderinst` = `FUN_004ecfc0(self, "renderinst")` (string at 0x5a3830): returns the
  field's `Native.ptr` handle, 0 when the field is missing/not a ResourceRef.
  In `src`: `text_renderinst(vm, o)` (natives.cpp:5153) returning `Runtime::Handle *`.
- `RI` = `*(int **)(h + 0xc)` = `h->target` in `src`.
- Standard guards: `if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);` and
  `T = (FUN_0051f4f0(mask, 0, 10.0f) < 0) ? 0 : RI->vf[+0xc](1.0f);`
  (`FUN_0051f4f0` = `Resource__getTyped`, 0x51f4f0).
- Constants: `0x3f800000` = 1.0f, `0x41200000` = 10.0f, `0x80000001` = 2147483649,
  `0xa0000001` = 2684354561, `0x80000000` = 2147483648 (typed-data masks).

### 0x4e5c40 Text.changeColor(I)V
```
getArgs(ctx, &self, &color);
h  = renderinst(self);
RI = *(int **)(h + 0xc);                       // no null check on h
if (RI != 0) {
    if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);   // 0x3f800000
    T = (FUN_0051f4f0(0x80000001, 0, 10.0f) < 0) ? 0 : RI->vf[+0xc](1.0f);
    if (T != 0) *(u32 *)(T + 0x17c) = color;   // 0x4e5c6a
}
```
`src` mapping: `r->text_color = (uint32_t)color;` on the bound resource.

### 0x4e5bb0 Text.changeAlign(I)V
Byte-for-byte the same body as changeColor; the only difference is the store
`*(u32 *)(T + 0x174) = align;`. `src` mapping: `r->text_align = align;`.

### 0x4e5d80 Text.setScale(FF)V
```
getArgs(ctx, &self, &sx, &sy);
h  = renderinst(self);
RI = *(int **)(h + 0xc);                       // no null check on h
if (RI != 0) {
    if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);
    T = (FUN_0051f4f0(0x80000001, 0, 10.0f) < 0) ? 0 : RI->vf[+0xc](1.0f);
    if (T != 0 && (U = *(int **)(T + 0x114)) != 0) {
        if (U->+0x7c != 1) U->vf[+0x14](1.0f);
        if (FUN_0051f4f0(0xa0000001, 0, 0) >= 0 &&
            (V = U->vf[+0xc](1.0f)) != 0) {
            W = FUN_004d5f50(V + 0xc as this, 0x80000000, 1.0f);
            if (W != 0 && *(int *)(W + 0x18) != 0) {
                vec = { sx, sy, 0.0f };
                FUN_004d8710(this = *(void **)(W + 0x18), &vec);
            }
        }
    }
}
```

### 0x4d5f50 FUN_004d5f50(this, mask, arg2) — typed getter, ECX = V + 0xc
Confirmed from assembly (call site 0x4e5e70): `lea ecx,[eax+0xc]` with EAX = V;
args on stack are `0x80000000` (top) and `0x3f800000` = 1.0f.
```
void *FUN_004d5f50(this = V + 0xc, u32 mask, float arg2)
    R = *(int **)(this + 0xc);                     // = *(int **)(V + 0x18)
    if (R == 0) return 0;
    if (R->+0x7c != 1) R->vf[+0x14](arg2);         // arg2 = 1.0f here
    if (FUN_0051f4f0(mask, 0, 10.0f) < 0) return 0;  // mask = 0x80000000
    return R->vf[+0xc](1.0f);                      // -> W
```

### 0x4d8710 FUN_004d8710(this, const vec3 *v) — apply scale to every node
```
for (i = 0; i < this->+0x64; i++) {                // group count
    g = this->+0x60[i];                            // group array
    for (j = 0; j < g->+0x8; j++)                  // node count
        g->+0x4[j]->vf[+0x4c](v);                  // node set-scale
}
```
In `src` there is no group/node layer, so this whole chain collapses onto the bound
resource: `text_scale = sx`, `text_scale_y = sy` (`+0x178`-style per-object state).

## Files
- edit: `src/game/runtime.h` — `Runtime::Resource`, after `float text_scale = 1.0f;`
  (~line 72), add one field (do not rename anything else):
  ```cpp
  float text_scale_y = 1.0f;         // INVENTED sy half: exe setScale writes a {sx,sy,0}
                                     // vec3 to every render node (0x4d8710), not to +0x178
  ```
- edit: `src/script/natives.cpp` — add the three natives after `tx_get_default_color`
  (~line 5179), next to `text_renderinst`; change registry rows 5927, 5931, 5932
  from `nullptr, true` to the new functions with `false`.
- read only: `src/game/runtime.h` (`Runtime::Resource` text fields set by card 067),
  `src/script/natives.cpp` (`text_renderinst`, `tx_create`).

## Changes
1. `tx_change_color` (0x4e5c40):
   ```cpp
   // 0x4e5c40 java.render.Text.changeColor(I)V
   // INVENTED: typed data T+0x17c mapped onto the bound Runtime::Resource.
   bool tx_change_color(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("Text.changeColor", err)) return false;
       Object *o;
       int32_t color;
       if (!receiver_object("Text.changeColor", a, &o, err) ||
           !want("Text.changeColor", a, 2, err) ||
           !get_i("Text.changeColor", a[1], &color, err))
           return false;
       Runtime::Handle *h = text_renderinst(vm, o);
       Runtime::Resource *r = h ? h->target : nullptr;
       if (r) r->text_color = (uint32_t)color;
       *out = Value::make_null();
       return true;
   }
   ```
2. `tx_change_align` (0x4e5bb0): identical, with `Text.changeAlign`, `text_align = align`
   (store offset `+0x174`), `get_i(a[1])`.
3. `tx_set_scale` (0x4e5d80):
   ```cpp
   // 0x4e5d80 java.render.Text.setScale(FF)V
   // INVENTED: the exe walks the render groups (0x4d8710 -> node vf[+0x4c]) and
   // writes {sx,sy,0}; src has no node layer, so the vector is kept on the resource.
   bool tx_set_scale(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("Text.setScale", err)) return false;
       Object *o;
       float sx, sy;
       if (!receiver_object("Text.setScale", a, &o, err) ||
           !want("Text.setScale", a, 3, err) ||
           !get_f("Text.setScale", a[1], &sx, err) ||
           !get_f("Text.setScale", a[2], &sy, err))
           return false;
       Runtime::Handle *h = text_renderinst(vm, o);
       Runtime::Resource *r = h ? h->target : nullptr;
       if (r) { r->text_scale = sx; r->text_scale_y = sy; }
       *out = Value::make_null();
       return true;
   }
   ```
4. Registry rows:
   - `{"java.render.Text", "setScale", "(FF)V", tx_set_scale, false}` (was 5927)
   - `{"java.render.Text", "changeColor", "(I)V", tx_change_color, false}` (was 5931)
   - `{"java.render.Text", "changeAlign", "(I)V", tx_change_align, false}` (was 5932)
   Leave `setPos`, `getPos`, `getWidthPixels` as stubs (out of scope).
5. No `Runtime::Resource` redefinition and no new helper: reuse `text_renderinst`,
   `text_color`, `text_align`, `text_scale` from cards 062/067.

## Done when
- each ported function has a `// 0xADDR java.render.Text.<name>` comment; the
  `Resource`-state mapping (incl. `text_scale_y`) is marked `INVENTED:`
- with a bound `renderinst` resource (i.e. after `Text.create`):
  `changeColor(c)` stores `text_color == c`, `changeAlign(k)` stores `text_align == k`,
  `setScale(sx,sy)` stores `text_scale == sx`, `text_scale_y == sy`
- with no `renderinst` field / no bound target all three are no-ops returning null
  (the exe also does nothing when RI or the typed block is absent; the null-handle
  guard is INVENTED safety, the exe dereferences `h + 0xc` unchecked)
- registry rows 5927, 5931, 5932 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- What the `setScale` chain objects are: `U = *(T+0x114)`, then `V = typed(U, 0xa0000001)`,
  then `R = *(V+0x18)` and `W = typed(R, 0x80000000)`, then `this = *(W+0x18)` walked by
  0x4d8710. `T+0x114`, `V+0x18`, `W+0x18`, and `FUN_004d5f50`'s `this = V+0xc`
  convention are not identified. Whether `U` is non-null after `Text.create` (0x4e8f40)
  was not determined — if it is null in the exe, `setScale` never writes anything.
  Storing on the bound resource assumes the chain exists (INVENTED).
- `FUN_004d8710`'s group layout is taken from its decompile (`+0x64` count, `+0x60`
  array, entry `+0x8` count / `+0x4` array, `vf[+0x4c]` = node set-scale); the node
  class and `vf[+0x4c]` body are untraced.
- The exe store offsets `T+0x174` / `T+0x17c` are re-confirmed from the decompiles
  (0x4e5bb0 / 0x4e5c40); no assembly re-read was done for these two, they were
  already verified in card 062.
- `setScale` sy has no dedicated typed-data slot (`+0x178` is a single float set to
  1.0 by `Text.create`); `text_scale_y` is a `src`-only field, INVENTED.

## Gate correction (Claude, overrides the Files/Changes above for setScale)
- `setScale` does **not** write `+0x178`: it scales the render nodes reached through
  `T+0x114` (0x4d5f50 -> 0x4d8710 vf[+0x4c]). src has no `T+0x114` link and no node layer,
  so in src's state the exe's guard `U = *(T+0x114) != 0` fails and nothing happens.
  Port `tx_set_scale` as: parse receiver + two floats, return null, with
  `// shortcut: 0x4e5d80 scales the render nodes via T+0x114 (0x4d5f50/0x4d8710); src has no
  such link, so this is the exe's no-link path (no effect).`
- Do **not** add `text_scale_y` and do not touch `text_scale`.
- changeColor / changeAlign as written (`text_color`, `text_align`); keep the exe's
  `RI != 0` check (`h ? h->target : nullptr`), null `h` = INVENTED safety.
