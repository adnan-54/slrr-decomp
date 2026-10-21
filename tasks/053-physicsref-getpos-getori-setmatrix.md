---
id: 053
status: blocked  # needs untraced helpers or a missing service (physics body / collision / type manager); see UNRESOLVED
branch:
depends: []
---
# PhysicsRef: getPos/getOri/setMatrix on the scripted resource

## Goal
Turn `java.util.resource.PhysicsRef.getPos()`, `getOri()` and
`setMatrix(Vector3,Ypr)` from registry stubs into natives that read/write the pose of the
scripted resource behind the receiver's `Native.ptr` handle.
The exe reads and writes the **attached physics body's** pose state
(`typed+0xb4`, double-buffered, refreshed with 0x504180); that body service does not exist
in `src/` yet (`PhysicsRef.create/createBox/createSphere` are still stubs and no
`Runtime::Resource` points at a `RigidBody`). Until it does, the natives use the bound
`Runtime::Resource::pos/ori` that the GameRef natives already expose; this mapping is
`INVENTED:` and must be marked as such.

## Exe evidence (cleaned pseudocode)
`ptr` = field index `DAT_007a2620` (the `Native.ptr` ref node `{prev,next,id,RI}`;
`getIntField` = 0x4ecc70, notes/06). `state` = `typed+0xb4` body-info record.
Field names live in `.rdata` as one-char strings: Vector3 `x`/`y`/`z` at
0x5a1da8/0x5a1dbc/0x5a1dd4 (setMatrix input) and 0x5a21c8/0x5a21d4/0x5a21e0 (getPos output);
Ypr `y`/`p`/`r` at 0x5a1df8/0x5a1e10/0x5a1e2c (setMatrix input) and
0x5a2380/0x5a2384/0x5a2388 (getOri output). The shipped `Ypr.java` declares
`float y,p,r;` (`bin/2.2.1 sources/system/Scripts/lang/src/Ypr.java`).
`getFloatField` 0x4ec980: missing/non-float field → 0 (logs); `setFloatField` 0x4ed080.

### 0x4e1c40 PhysicsRef.getPos()Ljava.lang.Vector3;
```
receiver = args[0]
node = getIntField(receiver, ptr)
if (node == 0) {
    log("vm_physicsref_getpos: scripted resource %08X not found in file \"%s\" at line %d");
    return null;
}
obj = gameObject(node)                                    // 0x4eabc0
if (obj != 0) {
    state = *(int*)(obj + 0xb4);
    pos   = (float*)(state + 0x64 + *(int*)(state + 0x20c) * 0xd4);   // 212-byte stride
} else {
    pos = (float*)receiver;                               // UNRESOLVED, see below
}
v = new java.lang.Vector3
setFloatField(v, "x", pos[0]);
setFloatField(v, "y", pos[1]);
setFloatField(v, "z", pos[2]);
return v;
```

### 0x4e1a40 PhysicsRef.getOri()Ljava.lang.Ypr;
```
receiver = args[0]
node = getIntField(receiver, ptr)
if (node == 0) {
    log("vm_physicsref_getori: scripted resource %08X not found in file \"%s\" at line %d");
    return null;
}
obj = gameObject(node)                                    // 0x4eabc0
ypr[3] = <uninitialized stack locals>                     // UNRESOLVED, see below
if (obj != 0) {
    state = *(int*)(obj + 0xb4);
    mat_to_ypr(ypr, state + 0xa4 + *(int*)(state + 0x20c) * 0xd4);    // 0x569970
}
y = new java.lang.Ypr
setFloatField(y, "y", ypr[0]);
setFloatField(y, "p", ypr[1]);
setFloatField(y, "r", ypr[2]);
return y;
```

### 0x4e1e40 PhysicsRef.setMatrix(Ljava.lang.Vector3;Ljava.lang.Ypr;)V
```
receiver = args[0]; pos_arg = args[1]; ori_arg = args[2]
node = getIntField(receiver, ptr)
if (node == 0) {
    log("vm_physicsref_setmatrix: scripted resource %08X not found in file \"%s\" at line %d");
    return;
}
pos = {0,0,0};
if (pos_arg != 0) pos = {getFloatField(pos_arg,"x"), getFloatField(pos_arg,"y"), getFloatField(pos_arg,"z")};
Ypr(0,0,0);                                   // 0x569b40 thiscall ctor, ECX = &ori, 3 zero dwords
ori = {0,0,0};
if (ori_arg != 0) ori = {getFloatField(ori_arg,"y"), getFloatField(ori_arg,"p"), getFloatField(ori_arg,"r")};
obj = gameObject(node)                        // 0x4eabc0
if (obj == 0) return;                         // silent no-op
state = *(int*)(obj + 0xb4);
idx = *(int*)(state + 0x20c);
write pos into state + 0x64 + idx*0xd4 and into state + 0x64 + (idx ^ 1)*0xd4;  // both buffers
matrix = ypr_to_matrix(ori);                  // 0x5666d0, dest state + 0xa4 + idx*0xd4
quat   = matrix_to_quat(matrix);              // 0x567ea0, dest state + 0xf8 + idx*0xd4
refresh(state);                               // 0x504180
```

### 0x4eabc0 gameObject(node) — the missing physics service
```
if (node->+8 == 0) return 0;                  // id 0: no resource instance
ri = node->+0xc;                              // RI (notes/13 §1)
if (ri == 0) return 0;
if (ri->+0x7c != 1) ri->vtable[+0x14](0);     // make resident
if (Resource__getTyped(0xa0000000,0,0) < 0) return 0;
obj = ri->vtable[+0xc](0x3f800000 /* 1.0f */);
if (obj == 0) return 0;
if (obj->+0xb4 != 0) return obj;
FUN_0047b970(node);                           // release path when typed data has no body
return 0;
```
The vtable slot names and the `+0xb4` body-info record come from notes/13 §1; the body-info /
pose-buffer service itself is not modelled in `src/`.

## Files
- edit: `src/script/natives.cpp` — add `ph_get_pos`, `ph_get_ori`, `ph_set_matrix` in the
  ResourceRef section (after `rr_get_next_child`, before the GameRef block ~line 1182); change the
  three registry rows at lines 4670-4672 from `nullptr, true` to the new functions with `false`.
- read only: `src/game/runtime.h` (`Runtime::Handle`, `Resource::pos/ori`).

## Changes
1. Follow the existing GameRef implementations (`gr_get_pos` 0x4e3890, `gr_get_ori` 0x4dac40,
   `gr_set_matrix` 0x4db950, `src/script/natives.cpp:1229-1260,1481-1501`) and helpers
   `receiver_object`, `value_resource`, `new_point`, `read_vec_arg`, `read_ypr_arg`.
   ```cpp
   // 0x4e1c40 PhysicsRef.getPos: new Vector3 from the scripted resource pose.
   // INVENTED: the exe reads the attached body's state buffer (state+0x64+idx*0xd4);
   // no body service exists in src/, so the bound Runtime::Resource::pos is used.
   bool ph_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("PhysicsRef.getPos", err)) return false;
       Object *o;
       if (!receiver_object("PhysicsRef.getPos", a, &o, err)) return false;
       Runtime::Resource *r = value_resource(a[0]);
       if (!r) { *out = Value::make_null(); return true; }   // exe: ptr==0 -> log + null
       Object *v = new_point(vm, "java.lang.Vector3", r->pos, false, err);
       if (!v) return false;
       *out = Value::make_obj(v);
       return true;
   }
   ```
2. `ph_get_ori` is the same with `"java.lang.Ypr"`, `r->ori`, `ypr=true`.
3. `ph_set_matrix`: `want(...,3)`, `read_vec_arg`/`read_ypr_arg` (a null argument yields
   `{0,0,0}`, as in the exe), then if `value_resource(a[0])` is non-null copy `pos`/`ori` into
   `r->pos`/`r->ori`; return null. Add
   `// shortcut: the exe writes both pose buffers, matrix/quat and refreshes (0x504180); only one pose is stored here (INVENTED).`
4. No logging for the missing-handle case: `src/` resource natives return null silently
   (`gr_get_pos` does the same instead of reproducing the exe game log).

## Done when
- each ported function has a `// 0xADDR Name` comment (`0x4e1c40 PhysicsRef.getPos`,
  `0x4e1a40 PhysicsRef.getOri`, `0x4e1e40 PhysicsRef.setMatrix`); every non-exe mapping is
  marked `INVENTED:` and the reduced pose write is `shortcut:`
- the three registry rows point at the new functions with `engine` = false
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/053

## UNRESOLVED
- getPos, `gameObject()` returns 0: the exe falls back to reading three floats at the
  **receiver script Instance** `+0x00/+0x04/+0x08` (raw: `puVar4 = local_14;` then
  `FUN_004ed080(puVar2,&DAT_005a21c8,*puVar4)` etc.). An Instance starts with a vtable
  (`*puVar2 = 0x59a430` at 0x4e1ce6), so this is a garbage read with no meaningful rebuild
  mapping; the card maps it to `null`.
- getOri, `gameObject()` returns 0: `local_24/local_20/local_1c` are never assigned before
  `FUN_004ed080(puVar2,&DAT_005a2380,local_24)`, i.e. the exe returns uninitialized stack
  floats; the card maps it to `null`.
- The exe only tests `node != 0`; a node with id 0 still reaches `gameObject()` and therefore the
  garbage fallback. The port treats "handle without a bound resource" as the null/no-op case
  (`INVENTED:`), which is also what the GameRef natives do.
- `gameObject()` internals: the RI vtable slot names (`+0x14` resident, `+0xc` typed object) and
  `Resource__getTyped(0xa0000000)` come from notes/13 §1 and were not re-traced for this card;
  the `+0xb4` state block exists only in the missing physics service.
