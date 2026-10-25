---
id: 066
status: merged
branch: card/066
depends: []
---
# resource: ResourceRef.destroy / duplicate / scaleMesh

## Goal
Replace the three `java.util.resource.ResourceRef` stubs (registry rows 4700, 4712 and
4713 in `src/script/natives.cpp`) with natives that take the exe's missing-handle
branch and reproduce the exe's result on every path `src/` can reach. All three exe
natives are `void` (they `RET` without setting `EAX`), so the VM result is `null` on
every path.

The exe's real work needs services `src/` does not have:

- `destroy` queues the ref node on the global deferred-delete queue (`0x5fde90`) —
  there is no frame-end queue in `src/`. The port detaches the handle (card 063's
  mapping for the same helper).
- `duplicate` binds the receiver to the source resource, and for source types
  5 / 7 / 0xd / 0xe first clones the resource instance through the resource manager
  (`FUN_0051ee30`) — `src/` has no clone service. The port binds for all other types
  (including a null source RI, which clears the receiver) and is a no-op for the four
  clone types.
- `scaleMesh` needs the resource-instance typed-data layer and the render mesh node
  list (`T+0x18`, `FUN_004d8710`) — `src/` has neither. With a present handle it is a
  documented no-op.

## Exe evidence (cleaned pseudocode, renamed fields)

Helpers:
- `getArgs` = FUN_004b1af0 (0x4b1af0): out0 = receiver (script instance), then one out
  per signature char in Java order (`F`/`I` copy the 32-bit argument value, `L` copies
  the object pointer). Card 061/063/065, notes/06.
- `instanceToHandle` = FUN_004ecc70 (0x4ecc70): script instance → its 0x10-byte ref
  node (`DAT_007a2620` is the shared class key); 0 when absent. Ref node: `+0x0` prev,
  `+0x4` next, `+0x8` id, `+0xc` RI. Notes/13 §1. In `src/`: `value_handle(a[0])` →
  `Runtime::Handle::id` / `::target`.
- An object argument's RI is read directly as `arg_script_instance + 0xc`; raw line
  from the sibling `set(ResourceRef)` native 0x4e4350:
  `FUN_0040cac0(*(undefined4 *)(local_c + 0xc));` where `local_c` is the argument
  script instance. In `src/`: `value_resource(a[1])`.
- `bind` = FUN_0040cac0 (0x40cac0; hidden ECX = destination handle, arg = source RI):
  unlink the destination from its old RI's `+0x78` handle list, link it into the new
  RI's `+0x78` list, set `handle.id = *(RI + 0x80)`; a 0 RI leaves the handle
  unlinked/cleared. `src/`: `GameRt::bind(handle, Resource*)`.
- RI+0x7c = `Runtime::Resource::type` (src comment at `rr_type`: "0x4e43e0: RI ?
  RI+0x7c : 0"; also card 063 `hnd->res->type /* +0x7c */`).

### 0x4e32b0 ResourceRef.destroy()V
```
getArgs(param_1, &self);
h = instanceToHandle(self);
if (h != 0) FUN_004e9450();          // hidden ECX = h
// no missing-handle log in this native
```

### 0x4e9450 FUN_004e9450 (__thiscall ECX = handle)   [same helper as card 063]
```
if (h->id /* +0x8 */ != 0 && h->res /* +0xc */ != 0) {
    0x4e945d: ECX = 0x005fde90;      // global delete queue
    FUN_0047b970(h);
}
```

### 0x47b970 FUN_0047b970 (__thiscall ECX = delete queue 0x005fde90, arg = h)
```
res = h->res;
if (res != 0 && res->type /* +0x7c */ == 1) {          // ins_game only
    if (FUN_0051f4f0(0x80000000, 0, 0) >= 0) res->vf3(+0xc)(1.0f);
}
node = FUN_00567a20(0x34);                            // alloc 0x34-byte queue node
node = node ? FUN_0047b090(h) : 0;                    // payload (untraced)
push node on queue+0x158 / queue+0x160; queue+0x224++;
// deferred deletion of the handle (and its resource) by the main loop.
```
`0x80000000` is the typed-data mask; `0x3f800000` = 1.0f (raw immediate inside 0x47b970).

### 0x4e32e0 ResourceRef.duplicate(ResourceRef source)V
```
local_8 = 0;
getArgs(param_1, &self, &local_8);   // self = receiver, local_8 = source instance
h = instanceToHandle(self);
if (h != 0 && local_8 != 0) FUN_004e9470(local_8);   // hidden ECX = h
// no missing-handle log in this native
```

### 0x4e9470 FUN_004e9470 (__thiscall ECX = destination handle, arg = source instance)
```
src = *(int*)(source_instance + 0xc);                 // source RI
if (src != 0) {
    t = *(int*)(src + 0x7c);                          // RI type
    if (t == 0xe || t == 0xd || t == 5 || t == 7) {
        newRI = FUN_0051ee30();                       // ECX = src: clone resource instance
        FUN_0040cac0(newRI);                          // bind dest, ECX implicit
        return 1;
    }
}
FUN_0040cac0(src);                                    // bind dest to the same RI (0 clears)
return 0;
```

### 0x51ee30 FUN_0051ee30 (__thiscall ECX = source RI) -> new RI or 0
Allocates a new resource instance of the same type through the resource manager
(`FUN_00518a40` / `FUN_00518880`), copies the type info (`RI+0x7c..`, 0x14 bytes) and,
when present, the `+0x90` data blob; sets flag `0x200`; returns the new RI, or 0 on
allocation/typed-data failure (in which case the caller leaves the new RI null).
Exact copy set not resolved (see UNRESOLVED).

### 0x4e4210 ResourceRef.scaleMesh(F x, F y, F z)V
```
getArgs(param_1, &self, &x, &y, &z);
h = instanceToHandle(self);
if (h != 0) {
    float v[3] = {x, y, z};
    FUN_004ebbf0(&v);                                 // hidden ECX = h
    return;
}
log("vm_resourceref_scalemesh: scripted resource " + hex(0) +
    " not found in file \"" + callinfo->file /*+0x74*/ + "\" at line " +
    callinfo->line /*+0x70*/);
```

### 0x4ebbf0 FUN_004ebbf0 (__thiscall ECX = handle; call site passes &v)
```
RI = h->res /* h+0xc */;
if (RI == 0) return;
if (*(int*)(RI + 0x7c) != 1) RI->vf(+0x14)(1.0f);     // make resident
if ((int32_t)FUN_0051f4f0(0x80000000, 0, 0) < 0) return;
T = RI->vf(+0xc)(1.0f);                               // typed data (raw 0x3f800000)
if (T == 0 || *(int*)(T + 0x18) == 0) return;
FUN_004d8710(/* ECX = *(T+0x18) */);                  // applies the scale vector
```
`FUN_004d8710` semantics (card 062): walks `this+0x64` groups (`this+0x60` array),
each entry's `+8` count / `+4` array, and calls `node->vf(+0x4c)(vec)` on every node.
The `&v` argument and the callee's receiver are not recovered by the decompiler (see
UNRESOLVED). Missing-handle log text is the string at 0x5a0df0.

## Files
- edit: `src/script/natives.cpp` only — add three functions after `rr_get_next_child`
  (ends line 1181), before the `// ---- java.util.resource.GameRef` block (line 1183);
  change registry rows 4700 (`destroy`), 4712 (`duplicate`) and 4713 (`scaleMesh`)
  from `nullptr, true` to the new functions with `false`.
- read only: `src/game/runtime.h` (`Runtime::Handle`, `Runtime::Resource`,
  `Runtime::detach` / `bind`).

## Changes
1. Three natives; `want` + `value_handle` like card 065's ResourceRef group:
   ```cpp
   // 0x4e32b0 ResourceRef.destroy()V
   bool rr_destroy(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.destroy", a, 1, err)) return false;
       *out = Value::make_null();
       Runtime::Handle *h = value_handle(a[0]);
       // 0x4e9450 (ECX = h) requires both id (+0x8) and RI (+0xc); only then
       // 0x47b970 queues the deferred delete on global 0x5fde90.
       // shortcut: no frame-end delete queue in src; detach removes the ref node
       // now (card 063's mapping for the same helper). The Resource node stays.
       if (h && h->id != 0 && h->target) game_rt()->detach(a[0].obj);
       return true;
   }

   // 0x4e32e0 ResourceRef.duplicate(ResourceRef source)V
   bool rr_duplicate(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.duplicate", a, 2, err)) return false;
       *out = Value::make_null();
       Runtime::Handle *h = value_handle(a[0]);
       if (!h || a[1].kind != Value::Obj || !a[1].obj) return true;
       Runtime::Resource *src = value_resource(a[1]);
       if (src) {
           uint32_t t = src->type;                       // RI+0x7c
           if (t == 0xe || t == 0xd || t == 5 || t == 7) {
               // shortcut: 0x4e9470 clones these types through FUN_0051ee30;
               // src/ has no resource-clone service, so the receiver is unchanged.
               return true;
           }
       }
       // 0x4e9470 else branch -> 0x40cac0(src): null src clears the receiver.
       game_rt()->bind(h, src);
       return true;
   }

   // 0x4e4210 ResourceRef.scaleMesh(float x, float y, float z)V
   bool rr_scale_mesh(Vm &, const Args &a, Value *out, std::string *err)
   {
       float x, y, z;
       if (!want("ResourceRef.scaleMesh", a, 4, err) ||
           !get_f("ResourceRef.scaleMesh", a[1], &x, err) ||
           !get_f("ResourceRef.scaleMesh", a[2], &y, err) ||
           !get_f("ResourceRef.scaleMesh", a[3], &z, err))
           return false;
       (void)x; (void)y; (void)z;
       *out = Value::make_null();
       if (!value_handle(a[0])) {
           std::fprintf(stderr, "vm_resourceref_scalemesh: scripted resource not found\n");
           return true;
       }
       // shortcut: 0x4ebbf0 needs the instance typed data (vtbl +0xc / 0x51f4f0
       // mask 0x80000000) and applies the scale to render nodes via FUN_004d8710
       // (T+0x18, node vf +0x4c); no typed-data or node-list service in src/.
       return true;
   }
   ```
   Argument order follows the exe: all arguments are parsed before the handle check;
   only `scaleMesh` logs on a missing handle. `destroy`/`duplicate` never log.
2. Registry:
   ```cpp
   {"java.util.resource.ResourceRef", "destroy", "()V", rr_destroy, false},
   {"java.util.resource.ResourceRef", "duplicate", "(Ljava.util.resource.ResourceRef;)V", rr_duplicate, false},
   {"java.util.resource.ResourceRef", "scaleMesh", "(FFF)V", rr_scale_mesh, false},
   ```

## Done when
- each function has its `// 0xADDR ResourceRef.<name>` comment; every unported side
  effect is covered by a `shortcut:` comment naming the exe call and why it cannot run
- no handle: all three return `null`; only `scaleMesh` writes
  `vm_resourceref_scalemesh: scripted resource not found` to stderr
  (src convention, natives.cpp:1655)
- present handle: `destroy` detaches the receiver's handle only when `id != 0 &&
  target != 0`; `duplicate` with a null/absent source RI clears the receiver, with a
  source of type 1/8/0x12/... binds the receiver to the same `Resource`, and with a
  source of type 5/7/0xd/0xe leaves it unchanged; `scaleMesh` is a no-op
- registry rows 4700, 4712, 4713 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- **Clone service for `duplicate`**: types 0xe/0xd/5/7 take
  `FUN_0051ee30` (0x51ee30) which builds a new instance via the resource manager and
  returns a new RI; `src/` has no equivalent, so that branch is a no-op and the
  receiver keeps its old binding (the exe would bind the clone). Exe return for the
  absent-service case: the native is `void` → `null`. `FUN_0051ee30`'s exact copy
  set / failure conditions (`in_ECX[9]`, `+0x80`, `+0x90`, `+0x2a..0x3c` fields) are
  not derived; raw line: `piVar3 = (int *)FUN_00518880(iVar2,*(undefined4 *)(in_ECX[9] + 0x80),local_28,in_ECX[9],0,0);`.
- **Deferred delete queue**: `FUN_0047b090` builds the 0x34-byte node whose payload
  is not traced; `0x47b970` links it at queue `0x5fde90` (`+0x158/+0x160` tail,
  `+0x224` count) and the main loop frees the handle/resource later. `src/` has no
  frame-end queue, so `destroy` detaches immediately (card 063 precedent).
- **`FUN_004ebbf0` arguments**: the call site is `FUN_004ebbf0(&local_20)` with the
  scale vector at `local_20..local_18 = {x,y,z}`, but Ghidra decompiles the helper as
  `void FUN_004ebbf0(void)` and does not show the argument or the hidden ECX/arg of
  the terminal `FUN_004d8710()` call. The mesh-apply path is therefore documented,
  not ported. Raw lines: `local_20 = local_c; local_1c = local_10; local_18 = local_14;
  FUN_004ebbf0(&local_20);` and `FUN_004d8710();` (terminal call at 0x4ebc4e).
- **Typed-data layer**: `FUN_0051f4f0` (mask 0x80000000) returns 0 or 0x80000001
  (see card 065); `RI->vf(+0x14)` (make resident), `RI->vf(+0xc)` (typed data) and
  `T+0x18` (mesh node list) are not mapped to any `src/` field. `FUN_004d8710`
  node-walk semantics are taken from card 062, not re-traced here.
- **`DAT_007a2620`** is the shared ResourceRef class key (notes/13); its value was
  not read byte-wise in this session.

## Gate (Claude)
Approved: duplicate/destroy follow the ResourceRef.set convention (arg handle -> target, natives.cpp rr_set_ref 0x4e4350). Use `game_rt()->bind(h, (Runtime::Resource *)src)` with an explicit Resource* (bind is overloaded).
