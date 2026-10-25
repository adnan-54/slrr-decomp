---
id: 093
status: merged
branch: card/093
depends: []
---
# render/script: RenderRef setLight / setFlare / setType / changeResource

## Goal
Replace four `java.util.resource.RenderRef` stub rows: `setLight(III)V` (0x4e29f0),
`setFlare(ResourceRef,IFFII)V` (0x4e2950), `setType(ResourceRef)V` (0x4e2eb0) and
`changeResource(ResourceRef,ResourceRef)V` (0x4e2090). All four write the
render-instance typed data block that `src/` does not model (the exe keeps it at
`RI+0x108`, vtable `0x5a7ec0`; cards 067/075/077 already map such state onto the
bound `Runtime::Resource`). `changeResource` turns out to be a **no-op** for that
block (its virtual `vf2` at `0x5a7ec0+8` is `0x46acd0 = xor eax,eax; ret 8`);
`setLight`/`setFlare`/`setType` become stores on the receiver's `Resource`.

## Exe evidence (cleaned pseudocode, renamed fields)
Common conventions (already established in cards 067/075/077/071):
`FUN_004b1af0` = `getArgs`, unpacking into `self` then one slot per declared argument;
`FUN_004ecc70(self, DAT_007a2620)` = `value_handle` (`h->id` = exe `+8`,
`h->target` = exe `+0xc`); `FUN_0040cac0` = `ResRefLink__set` (ECX = 0x10-byte ref
node `{+0 next, +4 prev, +8 id, +0xc resource}`); `FUN_0051f4f0(0x80000001,0,0)` =
"make resident + typed fetch", `RI->vf[+0xc](1.0f)` (`0x51be30`) = the typed data,
which is literally `RI+0x108`. Neither fetch can fail in `src`; the resident calls
(`RI+0x7c != 1 -> vf[+0x14]`) and the four ref-link/counter side reads below are
collapsed (`shortcut:`), as in the earlier cards.
Constants: `0x3f800000` = 1.0f; `0x00390625` = 0.00390625f = **1/256** (not 1/255);
`0x80000001` = typed-fetch mask.
Typed-data vtable, verified by xrefs and bytes: `RI`'s vtable `0x5a7ed0`
(`ResourceInstance_Render`) slot `+0xc` = `0x51be30` = `return RI+0x108`; the ctor
at `0x51b1e5` writes `0x5a7ec0` at `*T` (`ResourceInstance_Render_Data`, 3 slots:
`0x46f1a0, 0x46acd0, 0x46acd0`); slot `+8` = `0x46acd0` = `XOR EAX,EAX; RET 8`.
The only xrefs to `0x5a7ec0` are the two data ctors (0x51b1e5, 0x51b64c).

### 0x4e29f0 java.util.resource.RenderRef.setLight(III)V
    getArgs(ctx, &self, &c1, &c2, &c3);            // prologue defaults: c1=0xffffff,
                                                   // c2=0x404040, c3=0xffffff
    h = Native__instanceToHandle(self);            // ECX = result afterwards
    RenderRef__setLight(c1, c2, c3, 0);            // 0x4ebef0, thiscall ECX = h

### 0x4ebef0 RenderRef__setLight(c1, c2, c3, extra) — ECX = h
    RI = h->target; if (RI == 0) return;
    (resident; typed fetch mask 0x80000001; return if it fails)
    T = RI->vf[+0xc](1.0f); if (T == 0) return;
    T->+0x10 / +0x14 / +0x18 = channel(c1, R/G/B);   // ((c>>16)&0xff)*1/256 etc.
    T->+0x30 / +0x34 / +0x38 = channel(c2, R/G/B);
    T->+0x20 / +0x24 / +0x28 = channel(c3, R/G/B);
    if (extra != 0) { T->+0x4c = 8 bytes of *extra; T->+0x54 = extra[8]; }  // 0 here
`channel(c, s)` = `((float)((c >> s) & 0xff) + 0.0f) * 0.00390625f`, s = 16, 8, 0.
Argument 1 -> `+0x10..`, argument 2 -> `+0x30..`, argument 3 -> `+0x20..` (note the
2/3 swap in offset order).

### 0x4e2950 java.util.resource.RenderRef.setFlare(ResourceRef,IFFII)V
    getArgs(ctx, &self, &res, &i1, &f1, &f2, &i2, &i3);
    h = Native__instanceToHandle(self);
    node = {0, 0, 0, 0};                           // 0x10-byte stack RefNode
    ResRefLink__set(node, res->target);            // ECX = &node (+4 = id, +0xc = target)
    RenderRef__setFlare(h, node.next, node.prev, node.target, i1, f1, f2, i2, i3);

### 0x4ebe00 RenderRef__setFlare(node_next, node_prev, node_id, target,
                                i1, f1, f2, i2, i3) — ECX = h
    RI = h->target; if (RI == 0) goto cleanup;
    (resident; typed fetch; failure -> cleanup)
    T = RI->vf[+0xc](1.0f); if (T == 0) goto cleanup;
    ResRefLink__set(T + 0xc4, target);             // ECX = T+0xc4: flare texture link
    T->+0xd4  = f1;
    T->+0xd8  = f2;
    T->+0xe0  = i1;
    T->+0xe4  = i3;
    T->+0x100 = i2;
  cleanup:                                         // unlink the temporary stack node
    if (target != 0) res->+0x78 / node links are restored from node_next/node_prev;
                     if (node_prev) node_prev->next = node_next;
Visible effect: typed data `+0xc4` ref node bound to `res`'s target (id at `+0xcc`,
target at `+0xd0`) and the five values above. The temporary node is only a lifetime
anchor; `src` stores the `Resource*` directly.

### 0x4e2eb0 java.util.resource.RenderRef.setType(ResourceRef)V
    getArgs(ctx, &self, &res);
    h = Native__instanceToHandle(self);
    RenderRef__setType(res);                        // 0x4ec630, thiscall ECX = h

### 0x4ec630 RenderRef__setType(res) — ECX = h
    RI = h->target; if (RI == 0) return;
    (resident; typed fetch mask 0x80000001; failure -> return)
    T = RI->vf[+0xc](1.0f); if (T == 0) return;
    FUN_0051e090(T, res);                           // tail call, ECX = T

### 0x51e090 RenderInstData__setType(T, res) — ECX = T
    if (res->id (+8) != T->+0x110) {
        ResRefLink__set(T + 0x108, res->target);    // relink the instance type
        T->+0x118 = 1;
    }
An unbound `res` (id 0, target null) clears the `T+0x108` node when the stored id
differs. No log, no null check on `res` (the exe would fault on a null argument).

### 0x4e2090 java.util.resource.RenderRef.changeResource(ResourceRef,ResourceRef)V
    getArgs(ctx, &self, &a, &b);                    // both default to 0
    h = Native__instanceToHandle(self);             // no log, no null check
    if (h == 0) return;
    RI = h->target; if (RI == 0) return;
    if (RI->type != 1) RI->vf[+0x14](1.0f);         // make resident
    if (FUN_0051f4f0(0x80000001,0,0) < 0) return;
    T = RI->vf[+0xc](1.0f); if (T == 0) return;
    T->vf[+8](a, b);                                // ECX = T; cdecl 2 args
`T->vf[+8]` for render-instance data is `0x46acd0` = `XOR EAX,EAX; RET 8`: the call
ignores both handles and returns 0, so the native has no observable effect. (The
two refs are not even dereferenced.)

## Files
- edit: `src/game/runtime.h` (`Runtime::Resource`, after the bone fields)
- edit: `src/script/natives.cpp` (four handlers + four registry rows ~6988-6992)
- read only: `src/script/natives.cpp` (`value_handle`, `value_resource`,
  `receiver_object`, `want`, `get_i`, `get_f`, `need_rt` ~40-100 / 1386-1477)

## Changes
1. `src/game/runtime.h`, `Runtime::Resource`, next to `bones`:
   ```cpp
   // Render-instance typed data (RI+0x108, vtable 0x5a7ec0; 0x4ebef0/0x4ebe00/
   // 0x51e090). INVENTED mapping: the exe keeps these in the typed data block,
   // src has no typed-data layer, so they live on the bound Resource.
   float light_c0[3] = {};        // +0x10,+0x14,+0x18 setLight arg 1 (RGB)
   float light_c1[3] = {};        // +0x30,+0x34,+0x38 setLight arg 2
   float light_c2[3] = {};        // +0x20,+0x24,+0x28 setLight arg 3
   Resource *flare_texture = nullptr; // +0xc4 ref node target (0x4ebe00)
   float flare_f0 = 0.0f;         // +0xd4 first F
   float flare_f1 = 0.0f;         // +0xd8 second F
   int32_t flare_i0 = 0;          // +0xe0 first I
   int32_t flare_i1 = 0;          // +0x100 second I
   int32_t flare_i2 = 0;          // +0xe4 third I
   Resource *type_resource = nullptr; // +0x108 ref node target (0x51e090)
   uint32_t type_resource_id = 0;     // +0x110 ref node id (compared by setType)
   bool type_relinked = false;        // +0x118 = 1 when the type link changed
   ```
   Do not rename or duplicate existing fields.
2. `src/script/natives.cpp`, after `rr_set_matrix_bone` (~line 5833), four handlers:
   ```cpp
   // 0x4e29f0 RenderRef.setLight(III)V
   bool rr_set_light(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.setLight", err)) return false;
       Object *o;
       int32_t c[3];
       if (!receiver_object("RenderRef.setLight", a, &o, err) ||
           !want("RenderRef.setLight", a, 4, err) ||
           !get_i("RenderRef.setLight", a[1], &c[0], err) ||
           !get_i("RenderRef.setLight", a[2], &c[1], err) ||
           !get_i("RenderRef.setLight", a[3], &c[2], err))
           return false;
       *out = Value::make_null();
       Runtime::Resource *r = value_resource(a[0]);
       if (!r) return true;      // INVENTED: the exe dereferences a null handle
       static const int sh[3] = {16, 8, 0};
       float *dst[3] = {r->light_c0, r->light_c1, r->light_c2};
       for (int k = 0; k < 3; ++k)
           for (int i = 0; i < 3; ++i)
               dst[k][i] = (float)((c[k] >> sh[i]) & 0xff) * 0.00390625f;
       // shortcut: resident/typed fetch (0x51f4f0) and the dead extra-arg path
       // (+0x4c/+0x54, always 0 from 0x4e29f0) are collapsed.
       return true;
   }
   ```
   Keep the offset order: argument 1 -> `+0x10`, argument 2 -> `+0x30`,
   argument 3 -> `+0x20` (`dst` above already encodes it).
   ```cpp
   // 0x4e2950 RenderRef.setFlare(ResourceRef,IFFII)V
   bool rr_set_flare(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.setFlare", err)) return false;
       Object *o;
       int32_t i1, i2, i3;
       float f1, f2;
       if (!receiver_object("RenderRef.setFlare", a, &o, err) ||
           !want("RenderRef.setFlare", a, 7, err) ||
           !get_i("RenderRef.setFlare", a[2], &i1, err) ||
           !get_f("RenderRef.setFlare", a[3], &f1, err) ||
           !get_f("RenderRef.setFlare", a[4], &f2, err) ||
           !get_i("RenderRef.setFlare", a[5], &i2, err) ||
           !get_i("RenderRef.setFlare", a[6], &i3, err))
           return false;
       *out = Value::make_null();
       Runtime::Resource *r = value_resource(a[0]);
       if (!r) return true;      // INVENTED: the exe dereferences a null handle
       Runtime::Handle *tex = value_handle(a[1]);
       r->flare_texture = tex ? tex->target : nullptr;   // 0x4ebe00 T+0xc4 link
       r->flare_f0 = f1;         // +0xd4
       r->flare_f1 = f2;         // +0xd8
       r->flare_i0 = i1;         // +0xe0
       r->flare_i1 = i2;         // +0x100
       r->flare_i2 = i3;         // +0xe4
       // shortcut: the stack RefNode used only as a lifetime anchor, the resident
       // call and the 0x51f4f0 fetch are collapsed.
       return true;
   }
   ```
   ```cpp
   // 0x4e2eb0 RenderRef.setType(ResourceRef)V
   bool rr_set_type(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.setType", err)) return false;
       Object *o;
       if (!receiver_object("RenderRef.setType", a, &o, err) ||
           !want("RenderRef.setType", a, 2, err))
           return false;
       *out = Value::make_null();
       Runtime::Resource *r = value_resource(a[0]);
       if (!r) return true;      // INVENTED: the exe dereferences a null handle
       Runtime::Handle *ref = value_handle(a[1]);
       uint32_t id = ref ? ref->id : 0;
       if (id != r->type_resource_id) {          // 0x51e090
           r->type_resource = ref ? ref->target : nullptr;
           r->type_resource_id = id;
           r->type_relinked = true;
       }
       // shortcut: the resident call and the 0x51f4f0 fetch are collapsed.
       return true;
   }
   ```
   ```cpp
   // 0x4e2090 RenderRef.changeResource(ResourceRef,ResourceRef)V
   bool rr_change_resource(Vm &, const Args &a, Value *out, std::string &err)
   {
       if (!need_rt("RenderRef.changeResource", &err)) return false;
       Object *o;
       if (!receiver_object("RenderRef.changeResource", a, &o, &err) ||
           !want("RenderRef.changeResource", a, 3, &err))
           return false;
       *out = Value::make_null();
       // 0x4e2090 -> T->vf[+8](a, b); the render-instance data vtable 0x5a7ec0
       // slot +8 is 0x46acd0 (XOR EAX,EAX; RET 8): both refs are ignored.
       // shortcut: resident/typed-fetch side effects collapse to nothing in src.
       return true;
   }
   ```
3. Registry rows (~lines 6988-6992): point `"changeResource"`, `"setLight"`,
   `"setFlare"`, `"setType"` at `rr_change_resource`, `rr_set_light`, `rr_set_flare`,
   `rr_set_type`; `engine = false`. Leave every other RenderRef row alone.

## Done when
- each handler carries its exe address; the Resource mapping is `INVENTED:`, the
  collapsed resident/typed-fetch/RefNode work is `shortcut:`
- `setLight(0x123456, 0x404040, 0xffffff)` stores RGB/256 (`0x12/256, 0x34/256,
  0x56/256`) in `light_c0` (`+0x10`), `light_c1` (`+0x30`), `light_c2` (`+0x20`)
  respectively — check against 0x4ebef0, no unit test
- `setFlare` binds `flare_texture` to the ResourceRef target and stores
  f1/f2/i1/i2/i3 at `+0xd4/+0xd8/+0xe0/+0x100/+0xe4` (0x4ebe00); an unbound
  argument clears `flare_texture`
- `setType` relinks only when the argument handle id differs from
  `type_resource_id` and then sets `type_relinked`; an unbound argument clears the
  link like 0x51e090
- `changeResource` has no effect for bound render instances (0x46acd0); it still
  validates the receiver and returns null
- all four return null, log nothing; a missing receiver handle is a no-op (INVENTED
  safety: the exe faults)
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- 0x4ebef0's `extra` path (`T+0x4c` 8 bytes, `T+0x54`) is unreachable from
  0x4e29f0, which always passes `0`; not modelled.
- Whether `getArgs` leaves the setLight prologue defaults (`0xffffff, 0x404040,
  0xffffff`) when an argument slot is not convertible is not traced; the port uses
  the strict `get_i` (the VM dispatches by the registered `(III)V` signature, so
  the slots are ints). `0x404040` is the default of argument 2.
- The semantic names of `setLight`'s three colours and `setFlare`'s `IFFII`
  parameters are not recovered (only their offsets); fields are named by offset.
- Render-side consumers of `+0x10..+0x38`, `+0xc4..+0x100` and `+0x108..+0x118`
  (flare emitters, light table, instance kind) are not traced; nothing in `src`
  reads them yet.
- `T+0x118` (relink flag) consumer and the type-data fetch `0xa0000001`
  (getTypeID 0x4e23b0) are untraced.
- `changeResource` is a no-op only because the render-instance data vtable is
  `0x5a7ec0`; the only xrefs to that vtable are the ctors at 0x51b1e5/0x51b64c,
  so no subclass override was found, but the resource-class hierarchy was not
  exhaustively enumerated.
