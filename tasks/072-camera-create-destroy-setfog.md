---
id: 072
status: todo
branch:
depends: [074, 075]
---
# render: java.render.Camera create / destroy / setFog + GroundRef.setFog

## Goal
Replace four `nullptr` registry rows with handlers: `java.render.Camera.create`
(0x4d8de0), `Camera.destroy` (0x4d8fc0), `Camera.setFog` (0x4d9140) and
`java.util.resource.GroundRef.setFog` (0x4de640). `src/` has no scene-camera or
ground-info service, so the port keeps the exe's numeric state on the resource
bound to the script handle: `create` builds a type-3 `"s_renderref_camera"` node
(0x524840) carrying the camera words and rebinds the receiver handle to it;
`setFog` stores the fog words; `destroy` queues the handle for the deferred delete; `GroundRef.setFog`
stores the same fog words instead of the unmodelled 0x47c640 info dispatch. All
four natives return null (the exe natives are `void`).

## Exe evidence (cleaned pseudocode, renamed fields)
Common (already documented, do not re-trace):
- `getArgs` = 0x4b1af0: out0 = receiver, then one 32-bit slot per signature char.
  In src the receiver is `a[0]`, args are `a[1..n]`.
- `unwrap` = 0x4ecc70 -> `game_rt()->handle(o)` / `value_handle(v)`.
- `bind` = 0x40cac0 (ECX = ref node, arg = Resource) -> `game_rt()->bind(h, r)`.
- `create_resource(type, name)` is card 063's 0x525120->0x5188e0 stand-in.
- `0x51f4f0(mask,0,0) >= 0` then `ri->vf+0xc(1.0f)` is the "make resident /
  get typed data block" pair (notes/12 §9); no src equivalent, skipped.

### 0x4d8de0 java.render.Camera.create(ResourceRef,Viewport,IFFFFFII)V
Signature = `create(parent, viewport, id, fov, near, far, f4, f5, i1, i2)`.
Assembly-verified arg mapping (`getArgs` outs): `a[0]` receiver, `a[1]` parent
ResourceRef, `a[2]` Viewport, `a[3]` id I, `a[4..8]` five F, `a[9]` i1 I,
`a[10]` i2 I.
```
h  = unwrap(self)                                   # EDI = 0x4ecc70 result
vec = {0.0f, 0.0f, 3.0f}                            # [ebp-0x48..-0x40]
vp  = FUN_00422320(a[2])                            # game+0x198 / game+0x14c list hit on vp-node+0x34 == a[2].id
aspect_arg = 1.0f / (vp+0xec / vp+0xf0)             # 1.0f = [0x593598]; vp pixel w/h
cam = FUN_00524dc0(&DAT_005fde70, aspect_arg, a[4], a[5], a[6], a[7], a[8], a[9])
cam_data = cam.vf3(1.0f)                            # see 0x524dc0 below
cam_data+0x2c = a[10]                               # i2, done here after 0x524dc0
local_node = {0,0,0,0}                              # [ebp-0x54]
bind(local_node, cam)                               # 0x40cac0, ECX = &local_node
inst = FUN_00524840(a[1], &local_node, "s_renderref_camera", 0)   # type-3 render instance
bind(h, inst)                                       # 0x40cac0, ECX = EDI (handle)
FUN_004e77f0(h, "bone00"); FUN_004ec470(bone_id, 0, &vec, buf48)  # place at bone00
FUN_00420900(a[2], h)                               # attach camera to viewport -> FUN_00465130(h)
FUN_00420080(a[2], h, a[3], -1)                     # viewport camera list -> FUN_00464f60(h, a[3], -1)
unlink(local_node)                                  # temporary type link removed again
```
There is **no** null-handle check: the exe would fault at the 0x40cac0 bind.
Zero args are never read; `a[1]` is only passed to 0x524840 (parent placement).

### 0x524dc0 Render__createCameraType (called by create)
```
mgr = FUN_00518a40()
info = {type = 0xc, mgr, 2, 0, 0}; name "_rendertype_camera"   # "typ_render_camera"
cam  = FUN_005188e0(mgr, *(DAT_005fde70+8), &info, 0, 0)        # or 0x518880 with +0xc
cam+0x98 = 1.0f
data = cam.vf3(1.0f)
data+0xc  = 1.0f / param_2          # param_2 = aspect_arg => viewport w/h
data+0x10 = param_3 = a[4]          # fov
data+0x14 = param_4 * 10.0f = a[5] * 10.0f   # near, 10.0f = [0x59373c]
data+0x18 = param_5 * 10.0f = a[6] * 10.0f   # far
data+0x34 = param_6 = a[7]          # f4
data+0x38 = param_7 = a[8]          # f5
data+0x30 = param_8 = a[9]          # i1
```

### 0x524840 Render__createInstance (called by create; only this use is needed)
```
info = {type = 3, mgr, 1, 0, 0}; name = param_3 or "_renderinst"
inst = FUN_005188e0/FUN_00518880(mgr, *(param_1+8), &info, *(param_1+0xc), 0, 0)
inst+0x84 |= 0x200; link type; instance kind inst+0x11c from type resource type
  (camera 0xc -> 1)
bind(param_2 /* &local_node */, inst)                # ECX = param_2
```
0x4d8de0 passes `param_4 = 0` (no extra `bone%02d` bones) and the name
`s_renderref_camera` (0x5a12c4, verified).

### 0x420900 / 0x420080 attach helpers (called by create)
```
0x420900(viewport, camera_handle): ECX = game (DAT_005da014); find the viewport
  node (game+0x14c then game+0x198, node+0x34 == viewport.id); if found
  FUN_00465130(camera_handle)                       # viewport camera-list add
0x420080(viewport, camera_handle, id, -1):
  if viewport handle target type == 0x12 and the node is found:
      FUN_00464f60(camera_handle, id, -1)           # camera list entry with id
  else if type == 0x15 return 1; else return 0
```

### 0x4d9140 java.render.Camera.setFog(IFF)V
Assembly-verified: `a[1]` colour I, `a[2]` start F, `a[3]` end F.
```
h = unwrap(self)
if (h == 0) { log "vm_cam_setfog: scripted resource %x not found in file ..."; return; }
ri = h->target
if (ri == 0) return
if (ri->type != 1) ri->vf+0x14(1.0f)                # make resident
if (0x51f4f0(0xa0000000,0,0) >= 0) {
    inst = ri->vf+0xc(1.0f)                         # instance typed data
    if (inst != 0) {
        fog = *(Resource**)(inst + 0x114)           # embedded ref node +0xc
        if (fog != 0) {
            if (fog->type != 1) fog->vf+0x14(1.0f)
            if (0x51f4f0(0xa0000000,0,0) >= 0) {
                fd = fog->vf+0xc(1.0f)
                if (fd != 0) {
                    fd+0x24 = 1.0f                  # enabled
                    fd+0x28 = colour
                    fd+0x1c = start * 10.0f
                    fd+0x20 = end * 10.0f
                    return
                }
            }
        }
    }
}
```

### 0x4d8fc0 java.render.Camera.destroy()V
```
h = unwrap(self)
if (h == 0) { log "vm_cam_destroy: scripted resource %x not found in file ..."; return; }
ri = h->target
if (ri != 0) {
    if (ri->type != 1) ri->vf+0x14(1.0f)
    if (0x51f4f0(0xa0000000,0,0) >= 0) {
        inst = ri->vf+0xc(1.0f)
        if (inst != 0 && (*(uint32_t*)(inst + 0x110) & 0xffff0000) == 0xffff0000)
            FUN_004e9450(ECX = inst + 0x108);       # embedded fog ref node
    }
    FUN_004e9450(ECX = h);                          # queue-delete the receiver handle
}
```
Assembly check in 0x4d8fc0: `lea ecx,[eax+0x108]; mov eax,[ecx+8];
and eax,0xffff0000; cmp eax,0xffff0000; jne; call 0x4e9450` (ECX = the embedded
node), then `mov ecx,edi; call 0x4e9450` (ECX = the receiver handle).
0x4e9450 = card 063's deferred delete (`h->id != 0 && h->target != 0` -> 0x47b970).

### 0x4de640 java.util.resource.GroundRef.setFog(IFF)V
Assembly-verified: `a[1]` colour I, `a[2]` start F, `a[3]` end F.
```
h = unwrap(self)
if (h != 0) {                                       # no log on 0
    rec = new 0x10
    *(u8*)rec      = 1
    *(float*)(rec+4)  = start * 10.0f
    *(float*)(rec+8)  = end   * 10.0f
    *(u32*)(rec+0xc)  = colour
    FUN_0047c640(h, 0x4a, rec)                      # ECX = 0x5fde90 (Game)
}
```
### 0x47c640 (info post; called with id 0x4a)
```
if (h->id /* +8 */ != 0) {
    ri = h->target; t = ri ? ri->type /* +0x7c */ : 0
    if (t == 1) {
        if (ri) { make-resident; if (0x51f4f0(0xa0000000,0,0) >= 0) {
            inst = ri->vf+0xc(1.0f)
            if (inst) return FUN_0047e450(inst, 0x4a, rec)
        } }
    } else if (t != 8) { "GetInfo: Unexpected TypeOf!"; return rec; }
    else { if (ri) { ... 0x51f4f0(0x80000000,0,0); inst = ri->vf+0xc(1.0f);
                     if (inst) return inst->vf+0x3c(0x4a, rec) } }
}
return 0
```
### 0x47e450 (called by 0x47c640 for type 1)
```
if (inst+0x4c == 0 && inst+0x50 != 0) return FUN_0047dd50(inst, "getInfo", 1, 2, id, 0x4f)
else if (inst+0x4c != 0) {
    r = *(Resource**)(inst+0x44); make-resident;
    if (0x51f4f0(0x80000000) >= 0) { d = r->vf+0xc(1.0f);
        if (d) return d->vf+0x3c(*(inst+0x4c), id, rec) }
}
return 0
```

## Changes
1. `src/game/runtime.h`, `struct Resource` — after `bool viewport_active = false;`:
   ```cpp
   // res_camera typed data (0x524dc0, notes/12 §9): +0xc aspect (viewport w/h),
   // +0x10 fov, +0x14 near*10, +0x18 far*10, +0x2c i2, +0x30 i1, +0x34 f4, +0x38 f5.
   // fog data (0x4d9140): +0x1c start*10, +0x20 end*10, +0x24 enable 1.0f, +0x28 colour.
   // INVENTED: the exe keeps the camera words in the type-0xc resource's typed data
   // and the fog words in a separate resource reached via instance data +0x114;
   // src stores both on the resource bound to the script handle.
   float camera_aspect = 1.0f;      // +0xc
   float camera_fov = 0.0f;         // +0x10
   float camera_near = 0.0f;        // +0x14
   float camera_far = 0.0f;         // +0x18
   int32_t camera_i2 = 0;           // +0x2c
   int32_t camera_i1 = 0;           // +0x30
   float camera_f4 = 0.0f;          // +0x34
   float camera_f5 = 0.0f;          // +0x38
   int32_t camera_viewport_id = 0;  // arg2 -> 0x420080/FUN_00464f60; INVENTED storage
   float fog_start = 0.0f;          // +0x1c
   float fog_end = 0.0f;            // +0x20
   bool fog_enabled = false;        // +0x24 (exe stores 1.0f); INVENTED bool
   int32_t fog_color = 0;           // +0x28
   ```
   Do not rename or duplicate existing fields.
2. `src/script/natives.cpp` — four handlers after `vp_resize` (line 5183), before
   `}  // namespace`, using `need_rt`, `receiver_object`, `value_resource`,
   `want`, `get_i`, `get_f`, `Value::make_null`:
   - `// 0x4d8de0 Camera.create(ResourceRef,Viewport,IFFFFFII)V` `cam_create`:
     `want("Camera.create", a, 11)`; read `a[3]` id, `a[4]` fov, `a[5]` near,
     `a[6]` far, `a[7]` f4, `a[8]` f5, `a[9]` i1, `a[10]` i2. Then
     `Runtime::Handle *h = game_rt()->handle(o); if (!h) { *out = null; return true; }`
     (exe has no guard, it would fault), `Runtime::Resource *r =
     game_rt()->create_resource(3, "s_renderref_camera");` fill
     `camera_aspect = 1.0f`, `camera_fov = fov`, `camera_near = near * 10.0f`,
     `camera_far = far * 10.0f`, `camera_f4/f5`, `camera_i1/i2`,
     `camera_viewport_id = id`; `game_rt()->bind(h, r); *out = null;`.
     Do **not** derive the aspect from the Viewport rect: the exe uses the game
     viewport node's pixel `+0xec/+0xf0` (FUN_00422320) and src has no
     backbuffer-size service, so use the exe's no-data value `1.0f` (same as
     `vp_get_aspect`, card 061). Mark `INVENTED:`.
     Then bone00 (card 075 helpers, gate note): `const float p[3] = {0.0f, 0.0f, 3.0f};
     ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);  // 0x4e77f0/0x4ec470, vec {0,0,3}`
     (after `bind`, so `h->target == r`).
     `shortcut:` the ResourceRef parent/package placement, the local type link and
     the viewport attach (0x420900/0x420080) are not modelled.
   - `// 0x4d8fc0 Camera.destroy()V` `cam_destroy`: `want(..., a, 1)`;
     `Runtime::Handle *h = game_rt()->handle(o);`
     `if (h && h->id && h->target) game_rt()->queue_delete(h);  // 0x4e9450 (card 074)`
     `*out = null;`. `INVENTED:` no log for the missing handle. `shortcut:` the
     embedded fog ref node (inst+0x108/0x110/0x114) is not modelled (src has no fog
     resource). Same shape as `vp_destroy` after card 074.
   - `// 0x4d9140 Camera.setFog(IFF)V` `cam_set_fog`: `want(..., a, 4)`,
     `get_i(a[1])`, `get_f(a[2])`, `get_f(a[3])`; when
     `Runtime::Resource *r = value_resource(a[0])` is non-null set
     `r->fog_enabled = true; r->fog_color = colour; r->fog_start = start * 10.0f;
     r->fog_end = end * 10.0f;` in the exe's store order; `*out = null;`.
     `INVENTED:` fog stored on the bound resource (no fog resource).
     `INVENTED:` no log for the missing handle (exe logs `vm_cam_setfog`).
   - `// 0x4de640 GroundRef.setFog(IFF)V` `gr_set_fog`: identical parse and
     storage as `cam_set_fog`; `INVENTED:` missing handle silently does nothing
     (same as the exe). `shortcut:` the 0x10-byte record + 0x47c640/0x47e450
     info post (id `0x4a`) is not modelled; the four values are kept on the
     resource.
   Registry rows: line 5445 `create` -> `cam_create`, 5446 `destroy` ->
   `cam_destroy`, 5447 `setFog` -> `cam_set_fog`, line 5545
   `java.util.resource.GroundRef setFog` -> `gr_set_fog`; all four get
   `engine = false`.

## Files
- edit: src/game/runtime.h (`Runtime::Resource` fields)
- edit: src/script/natives.cpp (four handlers after `vp_resize` + four registry rows)
- read only: src/script/natives.cpp (vp_* helpers, `value_resource`, registry)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is
  `shortcut:`, non-exe mappings are `INVENTED:`, untraced callees `UNRESOLVED`
- no bound handle: all four natives are no-ops returning null and `create`
  allocates nothing
- bound handle: `create` adds a type-3 `"s_renderref_camera"` resource with
  `camera_aspect = 1.0f`, `camera_fov = a[4]`, `camera_near = a[5]*10`,
  `camera_far = a[6]*10`, `camera_f4 = a[7]`, `camera_f5 = a[8]`,
  `camera_i1 = a[9]`, `camera_i2 = a[10]`, `camera_viewport_id = a[3]`, and
  rebinds the receiver handle to it and creates bone00 at `(0,0,3)` (stored ×10); `setFog` stores enable/colour/start*10/end*10
  on that resource; `destroy` detaches the handle; `GroundRef.setFog` stores the
  same four values on the receiver's resource
- registry rows 5445-5447 and 5545 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- 0x524dc0: `FUN_005188e0(iVar1, *(int*)(param_1+8), ...)` / `FUN_00518880(..., *(int*)(param_1+0xc), ...)`
  — what `DAT_005fde70+8/+0xc` select as parent; the 0x20-byte name object built
  by `FUN_00569340(..., "_rendertype_camera", 0x1f)` is not passed to
  `FUN_005188e0` in the decompile. The port only uses type 0xc and the name.
- 0x524840 internals (bounds, `+0x11c` instance-kind switch, optional `bone%02d`
  table) are not traced; only the type-3 creation and the
  `s_renderref_camera` name are used.
- Who sets instance data `+0x114` (the fog resource) is not traced; 0x4d9140
  reads it and 0x4d8fc0 queue-deletes it through the embedded node at
  `inst+0x108`. No src equivalent.
- `FUN_00422320` (viewport node lookup, `vp+0xec/+0xf0` pixel size),
  `FUN_00465130` and `FUN_00464f60` (0x420900 / 0x420080 bodies) are untraced;
  src has no game viewport or viewport camera list.
- `FUN_0047e450` dispatch (`"getInfo"` script call via 0x47dd50, or
  `typed+0xc -> vf+0x3c`) and info id 0x4a semantics are untraced.
- `FUN_005663c0` (no-arg call in 0x4d8de0 after the unwrap) — purpose unknown;
  not modelled.
- The two `0x51f4f0` masks (`0xa0000000`, `0x80000000`) and `vf+0xc`/`vf+0x14`
  residency semantics are only known from notes/12 §9.
