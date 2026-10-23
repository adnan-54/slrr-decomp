---
id: 054
status: implemented
branch: card/054
depends: []
---
# PhysicsRef: create / createBox / createSphere

## Goal
Turn `java.util.resource.PhysicsRef.create(ResourceRef,int,String)`,
`createBox(ResourceRef,FFF,String)` and `createSphere(ResourceRef,F,String)` from
registry stubs into natives that reproduce the exe's argument checks and create the
scripted resource instance: a non-scripted physics child instance (resource type 2 =
`INSTANCE_PHYSICS`) under the parent `ResourceRef`, named by the alias (null alias →
`"_physinst"`), with the receiver's handle rebound to that instance and pose zero /
identity. The rigid body and collider geometry cannot be built: `src/` has no physics
world/body service. All three exe natives return **void**, so the port returns
`Value::make_null()` in every case, including the missing-handle and missing-service
cases (no error, no fault).

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers: `getArgs` = `Native__getArgs 0x4b1af0` (writes receiver, then declared args in
order; notes/06); `handle(self)` = `Instance__getIntField 0x4ecc70(self, DAT_007a2620)`
returns the 0x10-byte ref node `{+0 next, +4 prev, +8 id, +0xc RI}` (notes/06,
notes/14 §2.3); `FUN_0040cac0` = bind a ref node to a resource (notes/14 §2.3).
Argument order below was confirmed from the raw instruction streams of 0x4e1470,
0x4e16a0, 0x4e1880.

### 0x4e1470 PhysicsRef.create(ResourceRef parent, int typeRid, String alias)
```
create(self, parent, typeRid, alias)
h = handle(self)                                  // 0x4ecc70
if (h == 0 || parent == 0) {
    log("vm_physicsref_create: scripted resource %08X not found in file \"%s\" at line %d",
        h, vm+0x74, vm+0x70)                      // h is the node pointer (0 in the h==0 case)
    return                                        // V
}
node = {0,0,0,0}                                  // stack ref node: +0 next, +4 prev, +8 id, +0xc RI
node.bind(typeRid)                                // 0x51f9b0 ECX=&node: id=typeRid,
                                                  //   RI=resolve(typeRid) (0x5183a0), insert at RI+0x78
ori  = Ypr(0,0,0)                                 // 0x569b40 thiscall ctor, ECX=&ori, 3 zero dwords
mat  = euler_to_matrix(ori)                       // 0x5666d0, ECX=&mat
body = make_rigid_body(0.0f, &node)               // 0x4f7b50(mass=0, node): mass 0 => static;
                                                  //   uses node->+0xc (RI) to get the typed definition
body->flags(+0xb4) |= 1
ok = attach(parent, body, {0,0,0}, mat, alias)    // 0x4e8a50, ECX = h (see below)
unlink_handle(node)                               // remove temp node from RI+0x78 (node+0x0/+0x4)
return                                            // V
```
If `typeRid` resolves to no RI, `0x4f7b50` returns 0 and the exe faults at
`OR dword ptr [EAX + 0xb4], 0x1` (0x4e1563). The port returns void instead (see
INVENTED below).

### 0x4e16a0 PhysicsRef.createBox(ResourceRef parent, float x, float y, float z, String alias)
```
h = handle(self)                                  // 0x4ecc70
if (h == 0 || parent == 0) {
    log("vm_physicsref_create_box: scripted resource %08X not found in file \"%s\" at line %d",
        h, vm+0x74, vm+0x70)
    return
}
ori = Ypr(0,0,0); mat = euler_to_matrix(ori)      // 0x569b40 / 0x5666d0
body = make_body_shape(0.0f, 5,                // 0x4f7d60(mass=0, type=5 box,
                       x * 0x00593590,            //   half extents; float at 0x00593590 = 0.5,
                       y * 0x00593590,            //   notes/06: box half extents = size*0.5)
                       z * 0x00593590, 0)
body->flags(+0xb4) |= 1
attach(parent, body, {0,0,0}, mat, alias)         // 0x4e8a50, ECX = h
return
```

### 0x4e1880 PhysicsRef.createSphere(ResourceRef parent, float r, String alias)
```
h = handle(self)
if (h == 0 || parent == 0) {
    log("vm_physicsref_create_sphere: scripted resource %08X not found in file \"%s\" at line %d",
        h, vm+0x74, vm+0x70)
    return
}
ori = Ypr(0,0,0); mat = euler_to_matrix(ori)
body = make_body_shape(0.0f, 1, r, 0, 0, 0)       // 0x4f7d60(mass=0, type=1 sphere, radius r),
                                                  //   radius is passed through, not halved
body->flags(+0xb4) |= 1
attach(parent, body, {0,0,0}, mat, alias)         // 0x4e8a50, ECX = h
return
```

### 0x4e8a50 attach(parent, body, pos, mat, alias) — shared, thiscall ECX = receiver node h
```
if (parent == 0 || body == 0) return 0
obj  = physics_object(body, pos, mat)             // 0x4f7830 ECX=g_physWorld 0x5dadf0:
                                                  //   alloc 0x218, init world object, add body to world
inst = create_physics_instance(parent, obj, alias) // 0x524620
bind_node(h, inst)                                // 0x40cac0 ECX=h, arg=inst: h->+0xc = inst
bind_node(obj+0x48, h->RI)                        // 0x40cac0 ECX=obj+0x48, arg=h->+0xc
bind_node(obj+0x38, parent->RI)                   // 0x40cac0 ECX=obj+0x38, arg=parent->+0xc
if (h->RI != 0 && (h->RI->flags(+0x84) & 0x100))
    call_0x518e90(ECX = [0x5db008], h->RI, 0x3dcccccd /*0.1f*/, 0, 0)  // helper meaning not traced
return 1
```
So the receiver's handle (the same node returned by `handle(self)`) ends up bound to the
new physics instance — this is what makes `getPos`/`setMatrix` (card 053) address the
created body.

### 0x524620 create_physics_instance(parent, obj, alias)
```
if (no free resource slot (0x518a40 returns 0)) -> returns 0; caller then faults
info = {type = 2 /* INSTANCE_PHYSICS */, slot, 1, 0, name buffer}
name = alias != 0 ? alias : "_physinst"           // truncated to 0x1f
ri = parent->+0xc != 0
        ? create_resource_with_parent(0x518880, parent->+8, &info, parent->+0xc)
        : create_resource(0x5188e0, parent->+8, &info)
ri->flags(+0x84) |= 0x200
typed = ri->vtable[+0xc](1.0f)
typed->+0xb4 = obj                                // bind the physics object to the typed data
return ri
```

## Missing service (explicit)
`src/` has no rigid-body world (`g_physWorld` 0x5dadf0), no body factory
(0x4f7b50/0x4f7d60), no 0x218 physics-object type (0x4f7830) and no physics typed data
(`typed+0xb4`). The closest existing service is `Runtime::instantiate` + `Runtime::bind`
(used by `gr_create`, 0x4da620): model only the **resource instance** side of 0x524620.
The exe natives return void; on the missing body service the port still returns void
(`Value::make_null()`) and does not raise an error.

## Files
- edit: `src/script/natives.cpp` — add `ph_create`, `ph_create_box`, `ph_create_sphere`
  (and one local `alias_arg` helper) in the `java.util.resource` section, after the
  `rr_*` ResourceRef natives; change registry rows at lines 4667-4669 from
  `nullptr, true` to the new functions with `false`.
- read only: `src/game/runtime.h` (`Resource::type/name`, `Runtime::handle`,
  `Runtime::resolve`, `Runtime::instantiate`, `Runtime::bind`).

## Changes
1. Shared shape: receiver object via `receiver_object(who,a,&o,err)`, `want` for the
   declared arity (4 / 6 / 4 including the receiver), `Runtime::Handle *h =
   game_rt()->handle(o)`; `*out = Value::make_null()` first.
2. `// 0x524620`: `std::string alias_arg(const Value &v) { return v.kind ==
   Value::Str ? v.s : std::string("_physinst"); }` (null alias default, exe fact).
3. Missing-handle / null-parent cases: return void immediately. Do not log: the rebuild
   resource natives stay silent for missing handles (`gr_get_pos` does the same).
4. `ph_create`: `int32_t type_rid` via `get_i`; `Runtime::Resource *type =
   rt->resolve((uint32_t)type_rid)`; if null return void (`// INVENTED: exe faults on
   the null body`). Then `instantiate(type, value_resource(a[1]), alias, "", false, err)`.
5. `ph_create_box`: `get_f` for x/y/z, then `instantiate(nullptr, value_resource(a[1]),
   alias, "", false, err)`; keep a comment with the exe geometry
   `// 0x4f7d60: box half extents = x*0.5f, y*0.5f, z*0.5f; not stored (no physics).`
6. `ph_create_sphere`: `get_f` for r, same instantiate; comment
   `// 0x4f7d60: sphere radius = r; not stored (no physics).`
7. After instantiate: `inst->type = 2;` (`// 0x524620 creation info: INSTANCE_PHYSICS`),
   `inst->name = alias;` (exe stores the alias or the `_physinst` default) and
   `rt->bind(h, inst);` (0x4e8a50 first 0x40cac0). The obj+0x38/+0x48 binds and the
   wake call have no counterpart — mark
   `// shortcut: rigid body, 0x218 physics object and typed+0xb4 link not modelled; upgrade when the physics world exists (0x4f7830/0x4f7b50/0x4f7d60/0x524620).`
8. Registry rows 4667-4669: functions `ph_create`, `ph_create_box`, `ph_create_sphere`,
   `engine` = false.

## Done when
- each ported function has a `// 0xADDR Name` comment (`0x4e1470 PhysicsRef.create`,
  `0x4e16a0 PhysicsRef.createBox`, `0x4e1880 PhysicsRef.createSphere`); every non-exe
  mapping is marked `INVENTED:` and every dropped service is marked `shortcut:`
- registry rows 4667-4669 point at the new functions with `engine` = false
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/054

## UNRESOLVED
- RESOLVED (Claude, read_bytes): `0x00593590` = 0x3f000000 = 0.5.
  Its value is taken from notes/06, line 418: "box with **half extents** = size*0.5";
  raw line: `MOVSS XMM1, dword ptr [0x00593590]` then `MULSS XMM0, XMM1` for x/y/z.
- `0x4f7d60` body/shape internals (world `ECX = [0x5dadf0]`, body 0xb8 bytes, shape
  record 0xc4 bytes, type 1 sphere / type 5 box, `+0xb4 |= 0x400`) are not modelled;
  only the arguments and the static-mass (`param_1 = 0` → inverse mass 0, flags 4) are
  known. No src counterpart exists.
- `0x4f7830` (allocates a 0x218 world object from body+pos+mat, adds it to the world via
  `0x501f30`) and the `obj+0x38` / `obj+0x48` ref nodes are not modelled.
- `0x4e8a50` return value: 0 only when `parent == 0 || body == 0` (both are already
  rejected by the callers) or 1 otherwise; the caller ignores it. `0x524620` can return
  0 when `0x518a40` finds no free slot; the exe then dereferences it
  (`piVar4[0x21] = piVar4[0x21] | 0x200` at 0x5246c6). The port maps that to "no
  instance created, still return void" (`INVENTED:` no fault).
- The exe error log lines are not reproduced (the rebuild's resource natives return
  null/void silently, as `gr_get_pos` does); the `%08X` argument is the handle node
  pointer, the `%s`/`%d` come from `vm+0x74` / `vm+0x70` (notes/06).

## Why now (Claude)
Shipped GameInit (vmboot --selfcheck) stops at `java.render.Osd.createButton line 1239: native not implemented (stub): PhysicsRef.createBox`. Port the no-service path described above; after it, report the new vmboot stop line (`tools/vmboot-capped.sh --selfcheck | tail -3`).
