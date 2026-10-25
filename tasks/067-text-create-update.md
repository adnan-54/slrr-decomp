---
id: 067
status: merged
branch: card/067
depends: []
---
# render/script: java.render.Text create + update (render instance, string field)

## Goal
Replace the two `java.render.Text` stubs `create` (0x4e5b30) and `update` (0x4e5eb0).
`create` makes the engine render instance `"r_text"` (resource type 3 = `INSTANCE_RENDER`)
under the parent argument, binds the receiver's `renderinst` field handle to it, and writes
the instance typed data: flags `|= 0x8000`, empty string, align 2, scale 1.0, colour =
the process-wide default, bone00 translation `(x, y, 0)`. `update` copies the script's
`text` String field into the instance string slot (`+0x170`), empty when the field is
missing/null/not a String. `src/` has no render-instance creation service and no
render typed-data layer (cards 063/062 are still open), so the port creates the node with
the existing `Runtime::instantiate` (type overridden to 3), keeps the text state on the
bound `Runtime::Resource` (INVENTED mapping, same pattern as cards 053/061/062) and stores
the position in `Resource::text_pos`.

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers (already documented; no need to re-trace):
- `getArgs` = `Native__getArgs` 0x4b1af0: out0 = receiver, then one out per declared
  argument (ResourceRef args arrive as native handles: `RenderRef.create` 0x4e2140 reads
  `arg+8`/`arg+0xc` straight off them).
- 0x4ecfc0 `FUN_004ecfc0(self, "renderinst")` = 0x4ecae0(self, "renderinst",
  "java.util.resource.ResourceRef") then 0x4ecc70(field, DAT_007a2620). Returns the
  instance's `Native.ptr` handle (0 if the field is absent/not a ResourceRef). In `src`:
  `vm.get_instance_field(self, "renderinst", &v, &e)` + `value_handle(v)`.
- 0x4ecae0 `FUN_004ecae0(inst, name, "class")`: read object field by name, 0 when
  missing or wrong class. In `src`, `vm.get_instance_field` + `v.kind == Value::Str`.
- 0x4ecc70 `FUN_004ecc70(inst, DAT_007a2620)`: the instance's `Native.ptr` int field
  (the ref-node handle for a ResourceRef, the C `char*` of the string for a String).
- 0x40cac0 `FUN_0040cac0` = `Runtime::bind(Handle*, Resource*)` (notes/12 §9).
- 0x51f4f0 `Resource__getTyped(mask, 0, 10.0f)`, then `RI->vf[+0xc](1.0f)` = typed block.
- String data verified in the exe string table (VA -> file offset map checked against
  `analysis/strings.tsv`): 0x5a3654 `"renderinst"`, 0x5a3660 `"java.lang.Thread"`,
  0x5a3674 `"java.lang.String"`, 0x5a3688 `"(F)V"`, **0x5a3690 `"text"`**,
  0x5a3698 `"sleep"`; DAT_005a02aa and DAT_005a02ab are NUL bytes = `""`.
- Constants: `0x3f800000` = 1.0f, `0x41200000` = 10.0f, `0x8000` = 32768,
  `2` = align left (`Text.java ALIGN_LEFT`), type 3 = `INSTANCE_RENDER`
  (`ResourceRef.java`), `DAT_005dadac` = default text colour = `0xffffffff`
  (card 062 / `render/ui.h kDefaultTextColor` / `Text.java DEF_COLOR`).

### 0x4e5b30 Text.create(ResourceRef parent, ResourceRef charset, float x, float y)
    FUN_004b1af0(ctx, &self, &parent, &charset, &x, &y);
    h = FUN_004ecfc0(self, "renderinst");          // 0x5a2bc8; asm 0x4e5b9b MOV ECX,EAX -> thiscall this
    v3 = { x, y, 0.0f };
    FUN_004e8f40(parent, &DAT_005a02aa, &v3, charset, DAT_005dadac, 2, 0x3f800000);
    // thiscall: ECX = renderinst handle

### 0x4e8f40 FUN_004e8f40 (Text__create), ECX = renderinst handle
    identity = FUN_005663c0();                     // 4x4 identity (diag 1)
    if (*(int *)(charset_handle + 8) != 0) {       // handle +8 = resource id (notes/12 §9) = our Handle::id
        res = FUN_00524840(parent_handle, charset_handle, "r_text", 0);
        FUN_0040cac0(res);                         // ECX = renderinst -> bind
        bone = FUN_004e77f0("bone00");             // ECX = renderinst
        FUN_004ec470(bone, 0, &v3, identity);      // bone00 translation (x,y,0)
        RI = *(Resource **)(ECX + 0xc);
        if (RI) { RI->+0xb0 = 2; RI->+0xb4 .. +0xc8 = 0; }   // bounds kind none / zero
        if (RI) {
            if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);          // make resident
            if (FUN_0051f4f0(0x80000001, 0, 0.0f) < 0) T = 0;   // create passes 0.0f (asm 0x4e9004)
            else T = RI->vf[+0xc](1.0f);
            if (T) {
                T->+0x16c |= 0x8000;           // text flag
                T->+0x17c = DAT_005dadac;      // default colour
                T->+0x170 = &DAT_005a02aa;     // "" initial string
                T->+0x174 = 2;                 // align left
                T->+0x178 = 1.0f;              // scale
            }
        }
    }
    return;                                        // void

### 0x524840 FUN_00524840 Render__createInstance (parts used by Text__create)
    instance = FUN_005188e0(mgr, parent_id, {type = 3, flags = 1, ...}, 0, 0)   // parent->res == 0
             | FUN_00518880(mgr, parent_id, {type = 3, flags = 1, ...}, parent->res, 0, 0);
    name = "r_text";                               // param_3
    make resident; instance->+0x84 |= 0x200; instance->+0x98 = 1.0f;
    create typed block;
    FUN_0051e090(charset_handle);                  // link type
    FUN_0040cac0(instance);                        // bind (caller's handle)
    // param_4 (bone count) is 0 here; type switch sets instance kind +0x11c (not modelled)
    return instance;

### 0x4e5eb0 Text.update()V
    FUN_004b1af0(ctx, &self);
    inst = FUN_004ecfc0(self, "renderinst");       // handle
    s    = FUN_004ecae0(self, "text", "java.lang.String");
    str  = (s == 0) ? 0 : FUN_004ecc70(s, DAT_007a2620);
    RI = *(Resource **)(inst + 0xc);
    if (RI != 0) {
        if (RI->+0x7c != 1) RI->vf[+0x14](1.0f);
        T = (FUN_0051f4f0(0x80000001, 0, 10.0f) < 0) ? 0 : RI->vf[+0xc](1.0f);
        if (T != 0) T->+0x170 = (str != 0) ? str : &DAT_005a02ab;   // "" fallback
    }

## Changes
1. `src/game/runtime.h`, `Runtime::Resource`, after `float detail = 0.0f;`:
   ```cpp
   // java.render.Text state, render-instance typed data +0x16c..+0x17c
   // (Text__create 0x4e8f40). src has no typed-data/bone service yet, so it
   // lives on the bound Resource. INVENTED mapping; same as card 062.
   int32_t text_flags = 0;            // +0x16c (create: |= 0x8000)
   std::string text_data;             // +0x170 current string (create: "")
   int32_t text_align = 2;            // +0x174 (2 = left)
   float text_scale = 1.0f;           // +0x178
   uint32_t text_color = 0xffffffffu; // +0x17c (DAT_005dadac)
   float text_pos[3] = {};            // bone00 matrix translation, raw (INVENTED)
   ```
   Do not rename/duplicate existing fields.
2. `src/script/natives.cpp`: `g_default_text_color` already exists (card 062, ~line 4970,
   next to `tx_set_default_color`); reuse it, do not add another. Add after it:
   ```cpp
   // 0x4ecfc0: receiver's "renderinst" field (RenderRef) -> its native handle.
   Runtime::Handle *text_renderinst(Vm &vm, Object *self)
   {
       Value v;
       std::string e;
       if (!vm.get_instance_field(self, "renderinst", &v, &e)) return nullptr;
       return value_handle(v);
   }
   ```
3. `tx_create` (0x4e5b30): `need_rt("Text.create")`, `receiver_object`, `want(...,5)`,
   `get_f(a[3])`/`get_f(a[4])`.
   - `Runtime::Handle *font = value_handle(a[2]);`
     `if (!font || font->id == 0) { *out = Value::make_null(); return true; }`  // 0x4e8f54: charset handle +8 (id) == 0 -> nothing (null handle: INVENTED safety, exe faults)
   - `Runtime::Handle *h = text_renderinst(vm, o);`
     `if (!h) { *out = Value::make_null(); return true; }`  // INVENTED safety (exe would fault)
   - `Runtime::Resource *r = game_rt()->instantiate(nullptr, value_resource(a[1]), "r_text", std::string(), false, err);`
     `if (!r) return false;`
     `r->type = 3;` (exe creates INSTANCE_RENDER; instantiate makes ins_game) and
     `r->flags |= 0x200;` (instance->+0x84).
   - `game_rt()->bind(h, r);`
   - `r->text_flags |= 0x8000; r->text_data.clear(); r->text_align = 2;`
     `r->text_scale = 1.0f; r->text_color = g_default_text_color;`
     `r->text_pos[0] = x; r->text_pos[1] = y; r->text_pos[2] = 0.0f;`
   - `*out = Value::make_null();`
4. `tx_update` (0x4e5eb0): `need_rt`, `receiver_object`, `want(...,1)`;
   `Runtime::Handle *h = text_renderinst(vm, o);`
   `Runtime::Resource *r = h ? h->target : nullptr;`
   `if (r) { Value v; std::string e;`
   `if (vm.get_instance_field(o, "text", &v, &e) && v.kind == Value::Str) r->text_data = v.s;`
   `else r->text_data.clear(); }`
   `*out = Value::make_null();`
5. Registry rows (`"java.render.Text", "create"` / `"update"`, ~lines 5462/5467): `create` -> `tx_create`, `update` -> `tx_update`, both `false`.
   Leave every other `java.render.Text` row a stub (card 062 scope).

## Files
- edit: `src/game/runtime.h` (`Runtime::Resource` text fields)
- edit: `src/script/natives.cpp` (`text_renderinst`, `tx_create`, `tx_update`, registry rows)
- read only: `src/render/ui.h` (`kDefaultTextColor`), `src/game/runtime.h` (`instantiate`)

## Done when
- `tx_create` carries `// 0x4e5b30 java.render.Text.create`, `tx_update`
  `// 0x4e5eb0 java.render.Text.update`, `text_renderinst` `// 0x4ecfc0`; the
  Resource-state mapping and `text_pos` are marked `INVENTED:`, unmodelled exe work is
  `shortcut:`, un-traced callees are `UNRESOLVED`
- with a bound `charset` ResourceRef and a bound `renderinst` handle: `create` adds a
  type-3 `"r_text"` resource under the parent (parentless for a null/unbound parent),
  binds `renderinst` to it, and sets `text_flags & 0x8000`, empty `text_data`,
  `text_align == 2`, `text_scale == 1.0f`, `text_color == g_default_text_color`,
  `text_pos == {x,y,0}`; `update` then stores the script `text` String in `text_data`
- with no charset resource: `create` is a no-op returning null; with no `renderinst`
  handle or no target: both natives are no-ops returning null (no fault)
- `update` with a missing, null or non-String `text` field stores an empty string
- the two registry rows point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- (resolved in gate, Claude: assembly 0x4e5b9b `MOV ECX, EAX` passes the 0x4ecfc0 result
  as `this`; 0x4e8f47 keeps it in ESI for bind/bone/typed writes. update 0x4e5eff reads
  `[handle+0xc]` with no null check on the handle.)
- 0x524840 internals `FUN_005188e0`/`FUN_00518880` (creation), `FUN_0051e090` (type
  link), `FUN_00422070` (game registration), `+0x11c` render kind, `+0x98 = 1.0f`,
  `DAT_00773ad0` are untraced; the port allocates with `Runtime::instantiate` and
  overrides the type to 3 (`INVENTED`/`shortcut`).
- The bounds reset in 0x4e8f40 (`+0xb0 = 2`, `+0xb4..+0xc8 = 0`) is not modelled
  (no render/bounds consumer in `src`): `shortcut:`.
- Typed-data layer (`FUN_0051f4f0` mask `0x80000001`, `RI->vf[+0xc]`) collapsed onto the
  bound `Resource`; `INVENTED`.
- Bone00 write (`FUN_004e77f0`/`FUN_004ec470`, 0x4e8f40) has no `src` service; storing
  `(x, y, 0)` raw in `text_pos` is an `INVENTED` mapping (card 062's getter multiplies
  by 0.1f).
- `FUN_004ecae0`'s class check is not re-derived; the port accepts only `Value::Str` and
  treats everything else as "". `DAT_007a2620` (the native-ptr field descriptor) is taken
  from notes/06 and not re-derived; not needed in `src`.
- Card 062 (still todo) proposes the same `Resource` text fields and the
  `g_default_text_color` global; if 062 is implemented first, reuse them instead of
  adding duplicates.
