---
id: 079
status: implemented
branch: card/079
depends: []
---
# script: GameType callback registration + GameRef.getInfo

## Goal
Replace five `nullptr` registry rows in `src/script/natives.cpp`:
`GameType.registerCallback(I)V` (0x4dc740), `unregisterCallback(I)V` (0x4dc7c0),
`unregisterCallbacks()V` (0x4dc800), and both `GameRef.getInfo` overloads
(0x4daa80, `(II)I` and `(ILjava/lang/String;)I`, one exe function).
The callback natives append/remove nodes in the Game object's per-mode lists; the node
carries a handle bound to the receiver's resource. `unregisterCallbacks` is a **no-op in
the exe** (0x477320 is `ret 4`). `getInfo`'s guard chain is ported faithfully; the
per-type native handler it calls does not exist in src, so every live path returns the
exe's failure value `0` (`shortcut:`/`INVENTED:`). This card supersedes the `getInfo`
part of blocked card 055; 055's `setActiveCollision`/`kismajomCheck` stay out.
Mode values are the shipped `GameType.java` constants (2.2.1 sources,
`system/Scripts/lang/src/GameType.java:183-191`): `GII_CONTROL = 8`, `GII_DRIVE = 9`,
`GII_ANIMATE = 0x1c`, with `enableControlHook`/`enableAnimateHook` calling
`registerCallback` and the `disable*` pair calling `unregisterCallback`.

## Exe evidence (Claude-traced, decompile + raw bytes)
Shared helpers (already used by earlier cards):
- `getArgs` = 0x4b1af0: out0 = receiver script object, then one 32-bit slot per
  signature char. src: `a[0]` receiver, `a[1..]` args.
- `instanceToHandle` = 0x4ecc70 -> src `value_handle(a[0])`: script object -> ref node
  `{+0 prev, +4 next, +8 id, +0xc RI}` = src `Runtime::Handle{target,id,prev,next}`
  (field order differs: exe `id` +8 / RI +0xc = src `id` / `target`).
- RI +0x7c = src `Resource::type`; RI +0x84 = src `Resource::flags`; RI +0x80 = ids.
- `0x40cac0(ECX = ref node, arg = RI)` = src `Runtime::bind`: sets `id = RI->id`,
  links the node into `RI+0x78`'s handle list; a null RI clears the node.

### 0x4dc740 GameType.registerCallback(I)V
    getArgs(ctx, &self, &mode);
    h = instanceToHandle(self);                 // 0x4ecc70
    // asm: mov ecx,0x5fde90 (Game); push mode; push h; call 0x47d670; ret no value
    Game__registerCallback(ECX = Game(0x5fde90), h, mode);

No log, no null-handle check.

### 0x4dc7c0 GameType.unregisterCallback(I)V
    getArgs(ctx, &self, &mode);
    h = instanceToHandle(self);
    Game__unregisterCallback(ECX = Game(0x5fde90), h, mode);   // 0x47e390

No log, no null-handle check.

### 0x4dc800 GameType.unregisterCallbacks()V
    getArgs(ctx, &self);
    h = instanceToHandle(self);
    FUN_00477320(ECX = Game(0x5fde90), h);
0x477320 raw bytes are `c2 04 00` (`ret 4`); Ghidra: `void FUN_00477320(void){ return; }`.
The handle argument is not even dereferenced, so **unregisterCallbacks does nothing**
(despite its name; `Chassis.finalize` calls it, shipped
`parts/scripts/bodypart/Chassis.java:253`).

### 0x47b090 ResourceRefDLNode__ctor(ECX = node, arg = h) — 0x34-byte callback node
    zero node+4 .. node+0x30; node+0 = vftable;
    0x40cac0(ECX = &node->handle (+0x24), arg = h->res (+0xc));
Node layout: `+0 vtbl, +4 next, +8 prev, +0x24 embedded ref node
{+0x24 link, +0x28 next, +0x2c id, +0x30 RI}`. So the node's handle is bound to
**h->target** (the receiver's resource), with the resource's id.
Exe raw: `FUN_0040cac0(*(undefined4 *)(param_1 + 0xc));` — `param_1` is the handle.

### 0x47d670 Game__registerCallback(ECX = Game, h, mode) — thiscall ret 8
    switch (mode) {
    case 8:     sentinel = Game+0x28; last_ptr = Game+0x30; count = Game+0x214; break;
    case 9:     sentinel = Game+0x74; last_ptr = Game+0x7c; count = Game+0x218; break;
    case 0x1c:  sentinel = Game+0xc0; last_ptr = Game+0xc8; count = Game+0x21c; break;
    default:    return;                  // any other mode: no-op
    }
    n = alloc(0x34);  if (!n) return;    // caller ignores the failure
    ResourceRefDLNode__ctor(ECX = n, h); // 0x47b090
    prev = *last_ptr;                    // previous last node
    *(int*)(prev + 4) = n;               // prev->next = n
    *(int*)(n + 4) = sentinel;           // n->next = sentinel
    *(int*)(n + 8) = prev;               // n->prev = prev
    *last_ptr = n;                       // sentinel->prev = n
    *count += 1;                         // Game counters
Appends at the tail; duplicates are not merged; `h` is dereferenced in 0x47b090
(`param_1 + 0xc`), so a null handle would crash the exe.

### 0x47e390 Game__unregisterCallback(ECX = Game, h, mode) — thiscall ret 8
    switch (mode) {
    case 8:     n = *(int*)(Game+8);     break;   // list head
    case 9:     n = *(int*)(Game+0x54);  break;
    case 0x1c:  n = *(int*)(Game+0xa0);  break;
    default:    return;
    }
    if (*(int*)(n + 4) == 0) return;     // empty list (n->next == 0)
    if (n == 0) return;                  // decompiled order: the +4 read comes first
    while (*(int*)(n + 0x2c) != *(int*)(h + 8)) {   // node handle id vs h->id
        n = *(int*)(n + 4);              // n = n->next
        if (n == 0 || *(int*)(n + 4) == 0) n = 0;
        if (n == 0) return;
    }
    // asm: add ecx,0x24; call 0x488080   -> 0x488080(ECX = &n->handle)
    Handle__unlink(&n->handle);          // clears id/RI/links; the node STAYS in the list
Only the **first** node of that mode whose handle id equals `h->id` is processed. The
node itself is not freed here; the per-frame consumers reap id-0 nodes.

### Consumers (not ported here; nodes are otherwise inert in src)
- 0x47ba10 **animate** pass: `n = *(Game+0xa0)`; skip while `n->next == 0`/sentinel;
  if `n+0x2c` (handle id) == 0 -> `n->vf0(1)` (free node) and `Game+0x21c--`;
  else if `n+0x30` (RI) and `RI+0x7c == 1`: make resident/typed data; if typed
  `+0x4c == 0 && +0x50 != 0` -> `0x47dd50(typed,"animate",0,0x4f)` (script call),
  else type-manager handler `[+0xc]+0x34(typed+0x4c, dt)`.
- 0x47bc20 **control**: same shape, head `*(Game+8)`, count +0x214 ->
  `0x47bc80(dt,&node->handle)` (script `"control"`).
- 0x47be30 **drive**: head `*(Game+0x54)`, count +0x218 ->
  `0x47be90(dt,&node->handle)` (script `"drive"`).
Call order in the main loop: sim step 0x47e050 (physics -> control/drive), timers
0x47b700, animate 0x47ba10, deferred drain 0x47d070.
The Game ctor was not traced: how `*(Game+8/+0x54/+0xa0)` is initialised (head dummy /
sentinel linkage) is UNRESOLVED, but register appends at the tail and the consumers walk
head->tail, which is what src models.

### 0x4daa80 GameRef.getInfo(II)I / getInfo(ILjava/lang/String;)I
    getArgs(ctx, &self, &query, &sub);            // sub = int or String object pointer
    h = instanceToHandle(self);
    if (h == 0) {
        log("vm_gameref_getinfo: scripted resource " + %08x(h) +
            " not found in file \"" + callinfo->file (+0x74) + "\" at line " +
            callinfo->line (+0x70));
        DAT_007a2838 = 0;
        return 0;
    }
    if (h->id (+8) == 0) return 0;
    ri = h->target (+0xc);  if (ri == 0) return 0;
    if (ri->type (+0x7c) != 1) ri->vf(+0x14)(1.0f);        // make resident
    if (0x51f4f0(0x80000000, 0, 0) < 0) return 0;          // typed-data status
    typed = ri->vf(+0xc)(1.0f);                            // 0x3f800000 = 1.0f
    if (typed == 0 || *(int*)(typed + 0x4c) == 0) return 0; // no native type instance
    e = 0x468b00(0x80000000, 1.0f, 0, 0);                  // type-registry entry
    if (e == 0) return 0;
    return (*(e->+0xc)->vtbl[+0x3c])(*(int*)(typed+0x4c), query, sub);
Every failure branch returns int 0. `arg1/arg2` are passed through unchanged; the
native does not type-check them. Default `GameType.getInfo(int)` in Java returns 0 but
is **not** reached from this native (see 0x47e450 below).

### 0x47c640 Game__getInfoPost(ECX = Game, ref, infoId, out) — NOT the native path
    if (ref->id (+8) != 0) {
        ri = ref->res (+0xc);  t = ri ? ri->type (+0x7c) : 0;
        if (t == 1) {                     // game instance
            if (ri) { make-resident; if (0x51f4f0(0xa0000000,0,0) >= 0) {
                typed = ri->vf(+0xc)(1.0f);
                if (typed) return 0x47e450(typed, infoId, out); } }
        } else if (t != 8) {              // type 8 = game-object type resource
            log("GetInfo: Unexpected TypeOf!");
            return out;                   // returns param_3, not 0
        } else {
            if (ri) { make-resident; if (0x51f4f0(0x80000000,0,0) >= 0) {
                typed = ri->vf(+0xc)(1.0f);
                if (typed) return (*(typed->+0xc)->vtbl[+0x3c])(...); } }
                // Ghidra: "Could not recover jumptable at 0x0047c6e6" — arguments unknown
        }
    }
    return 0;
`ref` is a ResourceRef node (id +8, RI +0xc), not a Handle. Engine callers use this
internal path (e.g. GroundRef.setFog 0x4de640 with id 0x4a; notes/14 §audio id 0x1b),
**not** `GameRef.getInfo`. `0xa0000000` = physics/vehicle typed-data mask.

### 0x47e450 Game__getInfoTyped(ECX = Game, typed, infoId, out)
    if (typed->+0x4c == 0) {
        if (typed->+0x50 != 0)
            return 0x47dd50(typed, "getInfo", 1, 2, infoId, out, 0x4f);  // script getInfo(int)
    } else {
        ri = *(int**)(typed + 0x44);
        if (ri) { make-resident; if (0x51f4f0(0x80000000,0,0) >= 0) {
            d = ri->vf(+0xc)(1.0f);
            if (d) return (*(d->+0xc)->vtbl[+0x3c])(*(int*)(typed+0x4c), infoId, out); } }
    }
    return 0;
The script fallback only fires when a game object has **no native type instance**; src
has no typed-data layer at all, so this service is documented, not wired.

## Files
- edit: `src/game/runtime.h` (`Runtime::Callback`, `register_callback`,
  `unregister_callback`, `callbacks_` member)
- edit: `src/game/runtime.cpp` (the two helpers + dtor cleanup)
- edit: `src/script/natives.cpp` (three GameType handlers, `gr_get_info`, five registry rows)

## Changes
1. `src/game/runtime.h`, public section after the timer API (`run_timers`):
   ```cpp
   // Game callback lists (0x47d670): one 0x34-byte ResourceRefDLNode per
   // registerCallback, whose handle is bound to the receiver's resource
   // (0x47b090). Modes 8 = GII_CONTROL, 9 = GII_DRIVE, 0x1c = GII_ANIMATE.
   struct Callback {
       Handle handle;   // node+0x24
       int mode = 0;    // the exe keeps three lists; one mode-tagged list is equivalent
   };
   // 0x47d670: append; other modes are a no-op. Null handle: INVENTED guard.
   void register_callback(Handle *h, int mode);
   // 0x47e390: unlink the handle of the first node of `mode` with id == h->id.
   void unregister_callback(Handle *h, int mode);
   ```
   private: `std::list<std::unique_ptr<Callback>> callbacks_;`
2. `src/game/runtime.cpp`:
   ```cpp
   // 0x47d670 Game__registerCallback (ECX = Game; ret 8): append a
   // ResourceRefDLNode bound to h->target to the mode's list.
   void Runtime::register_callback(Handle *h, int mode)
   {
       // INVENTED: 0x47b090 dereferences h (+0xc); a null handle crashes the exe.
       if (!h) return;
       if (mode != 8 && mode != 9 && mode != 0x1c) return;   // exe default
       auto cb = std::make_unique<Callback>();
       cb->mode = mode;
       bind(&cb->handle, h->target);                          // 0x47b090 -> 0x40cac0
       callbacks_.push_back(std::move(cb));                   // append at the tail
   }

   // 0x47e390 Game__unregisterCallback (ECX = Game; ret 8): first node of that
   // mode whose handle id equals h->id; 0x488080 unlinks its handle only.
   void Runtime::unregister_callback(Handle *h, int mode)
   {
       if (!h) return;                                        // INVENTED (exe reads h+8)
       for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
           Callback *cb = it->get();
           if (cb->mode != mode || cb->handle.id != h->id) continue;
           unlink(&cb->handle);                               // 0x488080
           // shortcut: the consumers 0x47ba10/0x47bc20/0x47be30 that reap id-0
           // nodes are not ported; drop the dead node here to bound the list.
           callbacks_.erase(it);
           return;
       }
   }
   ```
   `~Runtime`, before the `handles_` cleanup:
   `// INVENTED: Game dtor not traced; unbind callback handles like the timers.`
   `for (auto &c : callbacks_) unlink(&c->handle); callbacks_.clear();`
3. `src/script/natives.cpp`, after `gt_remove_all_timers` (the GameType block):
   - `gt_register_callback` (0x4dc740): `need_rt`, `want(...,2)`, `get_i(a[1])`,
     `*out = Value::make_null()`, `Runtime::Handle *h = value_handle(a[0]);` then
     `game_rt()->register_callback(h, mode);` (null h handled by the INVENTED guard).
   - `gt_unregister_callback` (0x4dc7c0): same shape, `unregister_callback(h, mode)`.
   - `gt_unregister_callbacks` (0x4dc800): `need_rt`, `want(...,1)`,
     `*out = Value::make_null();` and the comment
     `// 0x477320 is an empty function (bytes c2 04 00 = ret 4): no-op.`
     Do not call anything on the runtime.
4. `gr_get_info` (0x4daa80), next to `gr_queue_event`: share one handler for both
   registry rows.
   ```cpp
   // 0x4daa80 GameRef.getInfo(II)I / getInfo(ILjava.lang.String;)I
   bool gr_get_info(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("GameRef.getInfo", err)) return false;
       if (!want("GameRef.getInfo", a, 3, err)) return false;
       *out = Value::make_int(0);
       Runtime::Handle *h = value_handle(a[0]);              // 0x4ecc70
       if (!h) {
           std::fprintf(stderr, "vm_gameref_getinfo: scripted resource not found\n");
           return true;
       }
       if (h->id == 0 || !h->target) return true;            // exe +8 / +0xc guards
       // shortcut: 0x4daa80 then makes the RI resident, requires typed data with a
       // native instance (typed+0x4c), resolves the type-registry entry 0x468b00
       // and calls its manager vtbl+0x3c(instance, query, sub). src has no typed-
       // data / native-type-manager layer (see the `shortcut:` in Runtime::queue_event);
       // exe failure value 0.
       return true;
   }
   ```
   `a[1]`/`a[2]` are forwarded positions; do not convert or validate them (the exe
   copies raw 32-bit slots, and both overloads use this one function).
5. Registry rows (all `engine = false`):
   ```cpp
   {"java.lang.GameType", "registerCallback", "(I)V", gt_register_callback, false},
   {"java.lang.GameType", "unregisterCallback", "(I)V", gt_unregister_callback, false},
   {"java.lang.GameType", "unregisterCallbacks", "()V", gt_unregister_callbacks, false},
   {"java.util.resource.GameRef", "getInfo", "(II)I", gr_get_info, false},
   {"java.util.resource.GameRef", "getInfo", "(ILjava.lang.String;)I", gr_get_info, false},
   ```
6. Do not change `gt_create_native_instance`, `rr_delete_native`, `rr_new_native`,
   `rr_set_id`, `rr_set_ref` (adjacent rows keep `true`).

## UNRESOLVED
- **Callback-list header init**: register writes a sentinel at Game+0x28/+0x74/+0xc0
  and the last-node pointer at +0x30/+0x7c/+0xc8; unregister/consumers read the head
  at Game+8/+0x54/+0xa0 (exactly −0x20) and counts +0x214/+0x218/+0x21c. The Game
  constructor / head-dummy allocation was not traced (symbol/xref tools were unavailable
  in this session), so how those head pointers and sentinels are linked is unresolved.
  src models the observable order instead: tail append, head-to-tail first match.
  Raw lines: `iVar1 = *(int *)(in_ECX + 0x30); ... *(int *)(iVar2 + 4) = in_ECX + 0x28;`
  and `iVar1 = *(int *)(in_ECX + 0xa0);`.
- **Consumers are not ported**: 0x47ba10/0x47bc20/0x47be30 (script `animate`, `control`,
  `drive` dispatch, and native typed handlers) need the physics/animation frame passes;
  src has no such passes, so registered callbacks are bookkeeping-only until a follow-up
  card. `unregister_callback` therefore erases the dead node (`shortcut:`), while the
  exe leaves it for the consumer pass to reap.
- **getInfo type-manager service is missing**: `0x51f4f0` typed-data status, RI vtables
  `+0x14` (make resident) / `+0xc` (typed data), typed `+0x4c` native instance and
  `0x468b00`'s entry `+0xc -> vtbl+0x3c` are all unmodelled (see the `shortcut:` in
  `Runtime::queue_event`). The port
  returns 0 (the exe's value on every missing-handle/resource/typed-data/handler path);
  for a live game type the exe would return the per-type handler's int. INVENTED only
  for that live path.
- **0x47c640 type-8 branch**: Ghidra cannot recover the indirect call at 0x47c6e6
  ("Could not recover jumptable ... arguments unknown"); the returned `param_3` on
  "GetInfo: Unexpected TypeOf!" is copied from the decompile. This path is used by
  engine callers (GroundRef.setFog 0x4de640 id 0x4a, audio id 0x1b), not by the native.
- **0x47e450 script fallback**: `FUN_0047dd50(typed,"getInfo",1,2,id,sub,0x4f)` would map
  onto `Runtime::call_method` (0x47dd70), but `GameRef.getInfo` never reaches 0x47e450
  (it dispatches through `0x468b00` directly), so it stays unwired. Traced for the
  future typed-data layer.
- **Missing-handle behaviour**: none of the three callback natives logs or null-checks;
  0x47b090/0x47e390 dereference the handle (0x47d670/0x47e390 would crash on null).
  src no-ops instead (`INVENTED:`), consistent with the other natives' guards.

## Done when
- each ported function carries its `// 0xADDR Name` comment; untraced logic is
  `INVENTED:` / `shortcut:` / `UNRESOLVED:` as above; no exe behaviour "improved"
- `registerCallback(8|9|0x1c)` appends; any other mode is a no-op; a duplicate appends
  a second node; `unregisterCallback(mode)` removes the first node of that mode whose
  bound handle id matches the receiver's handle id, and other modes are untouched;
  `unregisterCallbacks()` does nothing
- `getInfo` with a missing handle writes `vm_gameref_getinfo: scripted resource not
  found` to stderr and returns int 0; with handle id 0 / no target returns 0 silently;
  live types return 0 (`shortcut:` as above); both overloads share the handler
- registry rows for the five natives point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
