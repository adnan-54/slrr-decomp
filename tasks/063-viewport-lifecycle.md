---
id: 063
status: implemented
branch: card/063
depends: []
---
# render: Viewport lifecycle (create / activate / deactivate / destroy)

## Goal
Replace four stubs in the `java.render.Viewport` registry block (src/script/natives.cpp
rows 4643-4646) with natives that give a script Viewport object a real `res_viewport`
resource. `create` allocates the engine resource (type `0x12`, name `"_viewport"`, data
`+0xc` depth and `+0x10..+0x1c` relative rect x,y,w,h) and binds the receiver's existing
native handle to it; `activate`/`deactivate` move it between the game's drawn list and
the inactive list; `destroy` deactivates it and queues the handle for deletion.
`src/` has no typed-data layer, so the resource data words live on the bound
`Runtime::Resource` (the exe's RI) — an `INVENTED:` mapping. This card also adds those
fields (card 061, blocked, will read them instead of editing runtime.h itself).

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers already documented (no need to re-trace):
- `getArgs` = FUN_004b1af0 (0x4b1af0): out0 = receiver (script instance), then one out per
  signature char (`I`/`F` copy the 32-bit argument value) — card 061 / notes/06.
- `unwrap` = FUN_004ecc70 (0x4ecc70) `unwrap(instance, DAT_007a2620)`: receiver → its
  `Native.ptr` ref node `{+0 link, +4 next, +8 id, +0xc Resource*}`, 0 when absent.
  In src this is `game_rt()->handle(o)` → `Runtime::Handle` (`target` = Resource).
- `bind` = FUN_0040cac0 (0x40cac0, `ResRefLink__set`, __thiscall ECX = ref node, arg =
  Resource): unlink from the old resource's `+0x78` list, then `node+0xc = res`,
  `node+8 = res+0x80` (resource id), push on `res+0x78`. This is `Runtime::bind(Handle*,
  Resource*)` (notes/12 §9, src/game/runtime.cpp:202).
- `0x5188e0` = resource-manager creation point, `0x518a40` = its allocator (notes/06
  §"GameRef.create"); internals not re-derived here.

### 0x4e6af0 Viewport.create(IFFFF)V
    getArgs(param_1, &self, &depth, &x, &y, &w, &h);       // receiver, I, F, F, F, F
    hnd = unwrap(self);
    if (hnd == 0) {                                        // 0x4e6b79
        log "vm_viewport_create: scripted resource %x not found in file \"%s\" at line %d";
        return;
    }
    res = FUN_00525120(0 /*parent, 0x4e6b61 PUSH 0x0*/, depth, x, y, w, h);
    0x4e6b6b: ECX = hnd;  FUN_0040cac0(res);               // bind receiver handle -> res

Only the handle case creates anything; the depth int is used, all four floats are used.

### 0x525120 FUN_00525120 Render__createViewport
    mgr = FUN_00518a40();                                  // resource manager / new slot
    if (mgr == 0) return 0;
    info = {type = 0x12, mgr, 0, 0, 0};  name = "_viewport";
    res = FUN_005188e0(mgr, parent = 0, &info, 0, 0);
    FUN_0051f790(1);                                       // make resident
    res+0x98 = 1.0f;                                       // piVar2[0x26] = 0x3f800000
    T = res->vf3(+0xc)(1.0f);                              // typed data block
    T+0xc  = depth;
    T+0x10 = x;  T+0x14 = y;  T+0x18 = w;  T+0x1c = h;
    FUN_0040cac0(/* ECX not shown */);                     // internal list link, see UNRESOLVED
    FUN_00420970(&local_20, T);                            // register with the game
    (then inserts the node built in local_18/local_14/local_1c/local_20 into a list)

So the data layout is `+0x10` left(x), `+0x14` top(y), `+0x18` width, `+0x1c` height —
the same words card 061's getters/resize read and write.

### 0x4e6ab0 Viewport.activate(I)V
    getArgs(param_1, &self, &ignored_i);                   // the I argument is never read
    hnd = unwrap(self);
    if (hnd != 0)
        FUN_004200d0(game /* ECX = DAT_005da014, 0x4e6adb */, hnd, 0xffffffff, 0xffffffff);
    return;

### 0x4200d0 FUN_004200d0 (game method, ECX = DAT_005da014)
    if (hnd->res /* +0xc */ != 0 && hnd->res->type /* +0x7c */ == 0x12)
        FUN_00421e40(hnd, -1, -1);                         // move viewport node to drawn list game+0x14c
    return 0;

### 0x4e6c50 Viewport.deactivate()V
    getArgs(param_1, &self);
    hnd = unwrap(self);
    if (hnd != 0) FUN_00420b20(hnd);
    return;

### 0x420b20 FUN_00420b20
    if (hnd->res == 0) return 0;
    switch (hnd->res->type /* +0x7c */) {
      case 0x12: FUN_00421f50(hnd); return 0;              // remove viewport node from game+0x14c
      case 0x0c: return 1;
      case 0x15: return FUN_00421da0();
      case 0x14: return 1;
    }
    return 0;

### 0x4e6c90 Viewport.destroy()V
    getArgs(param_1, &self);
    hnd = unwrap(self);
    if (hnd != 0) {
        FUN_00420b20(hnd);                                 // same dispatch as deactivate
        FUN_004e9450();                                    // ECX = hnd
        return;
    }
    log "vm_viewport_destroy: scripted resource %x not found in file \"%s\" at line %d";
    return;

### 0x4e9450 FUN_004e9450 (__thiscall ECX = handle)
    if (hnd+8 /* id */ != 0 && hnd+0xc /* Resource* */ != 0) {
        0x4e945d: ECX = 0x005fde90;  FUN_0047b970(hnd);
    }

### 0x47b970 FUN_0047b970 (__thiscall ECX = delete queue 0x005fde90, arg = handle)
    res = hnd->res;
    if (res != 0 && res->type /* +0x7c */ == 1) {          // ins_game only
        if (FUN_0051f4f0(0x80000000, 0, 0) >= 0) res->vf3(+0xc)(1.0f);
    }
    node = FUN_00567a20(0x34);
    if (node != 0) node = FUN_0047b090(hnd);
    push node on queue+0x158 / queue+0x160;  queue+0x224++;
    // deferred deletion of the handle (and its resource) by the main loop.

## Changes
1. `src/game/runtime.h`, `struct Resource` after `float detail = 0.0f;`:
   ```cpp
   // res_viewport typed data (0x525120, notes/12 §9): +0xc depth/order,
   // +0x10..+0x1c relative x,y,w,h.
   int32_t viewport_depth = 0;    // +0xc
   float viewport_rect[4] = {};   // +0x10 x, +0x14 y, +0x18 w, +0x1c h
   bool viewport_active = false;  // INVENTED: game drawn-list (+0x14c) membership
   ```
   Also declare, next to `attach`/`detach`:
   ```cpp
   // 0x525120 -> 0x5188e0: a created engine resource (no record); "_viewport" is type 0x12.
   Resource *create_resource(uint32_t type, const std::string &name);
   ```
   Do not change existing fields.
2. `src/game/runtime.cpp`, after `Runtime::detach`:
   ```cpp
   // 0x525120 -> 0x5188e0: created engine resource, no record. Same owned-node
   // allocation as instantiate (which keeps its ins_game / script / parent logic).
   Runtime::Resource *Runtime::create_resource(uint32_t type, const std::string &name)
   {
       auto up = std::make_unique<Resource>();
       Resource *r = up.get();
       instances_.push_back(std::move(up));
       r->id = next_instance_id_++;
       r->type = type;
       r->name = name.empty() ? "_gameinst" : name;
       r->instance = true;
       return r;
   }
   ```
3. `src/script/natives.cpp`, four functions immediately before `}  // namespace`
   (line 4383), using `need_rt`, `receiver_object`, `value_resource`, `want`, `get_i`,
   `get_f` (lines 43-97 / 973-997):
   - `vp_create`: `need_rt("Viewport.create")`, `receiver_object`, `want(..., a, 6)`,
     `get_i(a[1], &depth)`, `get_f(a[2..5], &rect[0..3])`; then
     `Runtime::Handle *h = game_rt()->handle(o); if (!h) { *out = Value::make_null(); return true; }`
     (exe logs and does nothing; src does not reproduce the game log);
     `Runtime::Resource *r = game_rt()->create_resource(0x12, "_viewport");`
     `r->viewport_depth = depth; for i<4 r->viewport_rect[i] = rect[i];`
     `game_rt()->bind(h, r); *out = Value::make_null();`.
   - `vp_activate`: `need_rt`, `receiver_object`, `want(..., a, 2)` (the int arg is
     unused, as in the exe); `Runtime::Resource *r = value_resource(a[0]);`
     `if (r && r->type == 0x12) r->viewport_active = true;` `*out = null`.
   - `vp_deactivate`: `want(..., a, 1)`; same with `viewport_active = false`.
   - `vp_destroy`: `want(..., a, 1)`; `Runtime::Handle *h = game_rt()->handle(o);`
     `Runtime::Resource *r = h ? h->target : nullptr;`
     `if (r && r->type == 0x12) r->viewport_active = false;`
     `if (h) game_rt()->detach(o);` `*out = null`.
     (`detach` = the deferred handle deletion; the owned Resource node stays — shortcut.)
   Registry rows 4643-4646: `{"java.render.Viewport", "create", "(IFFFF)V", vp_create, false}`,
   `destroy` → `vp_destroy`, `activate` → `vp_activate`, `deactivate` → `vp_deactivate`.

## Files
- edit: src/game/runtime.h, src/game/runtime.cpp, src/script/natives.cpp
- read only: none

## Done when
- each function has a `// 0xADDR Viewport.<name>` comment; the data-on-Resource mapping,
  `viewport_active`, and the missing-handle behaviour are `INVENTED:`; the deferred-delete
  replacement and the unmodelled game lists are `shortcut:`; untraced callees are `UNRESOLVED`
- no bound handle: all four natives are no-ops returning null and create adds no node
- bound handle: `create` adds a type-`0x12` `"_viewport"` node with `viewport_depth` and the
  four `viewport_rect` words, and rebinds that same handle to it; `activate`/`deactivate`
  toggle `viewport_active` only when the target type is `0x12`; `destroy` clears it and
  detaches the handle (the owned node remains)
- registry rows 4643-4646 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `FUN_00421e40` / `FUN_00421f50` bodies are not traced; their effect (move the Viewport
  node between the drawn list `game+0x14c` and the inactive list) is taken from notes/12
  §3/§9. The port models it as the `viewport_active` flag.
- `FUN_00420970(&local_20, T)` and the following list insertion in 0x525120 are not
  modelled; `src/` has no game viewport lists.
- `piVar2[0x26] = 0x3f800000` (0x525120: `res+0x98 = 1.0f`) — meaning unknown; not modelled.
- In 0x525120 the decompiler omits the receiver of `FUN_0040cac0(piVar2);` (raw line), so the
  internal link is unclear; it is not needed because create's own bind is verified at
  0x4e6b6b (`MOV ECX, EDI`).
- `FUN_0047b090` (payload of the deferred-delete queue node) is untraced. `src/` has no
  frame-end queue, so `destroy` detaches the handle instead of freeing the Resource node.
- `FUN_005188e0` / `FUN_00518a40` creation internals are not re-derived; the port allocates
  from `Runtime::instances_` via `create_resource`.
- Nothing in `src/` attaches a handle to a plain `new Viewport` object; if the script class
  constructor does not call `ResourceRef.newNative` (as the Java side does), `create` always
  takes the exe's missing-handle branch.

## Gate (Claude)
0x525120 re-decompiled: type 0x12, "_viewport", res+0x98 = 1.0f, typed +0xc depth,
+0x10..+0x1c x,y,w,h all match. Approved.
